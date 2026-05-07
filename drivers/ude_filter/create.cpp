/*
 * Copyright (c) 2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include <ntifs.h>

#include <initguid.h>
#include <devpkey.h>
#include <ntstrsafe.h>

#include "create.h"
#include "device.h"
#include "driver.h"
#include "irp.h"
#include "trace.h"
#include "create.tmh"

#include <libdrv/remove_lock.h>

/*
 * Matches _NT_TARGET_VERSION 0xA000006: ExAllocatePool2 is gated on NTDDI >= NTDDI_WIN10_VB in wdm.h.
 */
#pragma warning(push)
#pragma warning(disable : 4996)

namespace
{

using namespace usbip;

// Matches USBIP_OWNER_SDDL_CCH_MAX in include/usbip/vhci.h (userspace IOCTL / stamp buffer).
constexpr ULONG sddl_buf_cch = 384;

#ifndef PROCESS_QUERY_INFORMATION
#define PROCESS_QUERY_INFORMATION (ACCESS_MASK)(0x0400)
#endif

#ifndef SE_GROUP_ENABLED
#define SE_GROUP_ENABLED (0x00000004UL)
#endif
#ifndef SE_GROUP_USE_FOR_DENY_ONLY
#define SE_GROUP_USE_FOR_DENY_ONLY (0x00000010UL)
#endif

// Fixed NT authority SIDs; RtlCreateWellKnownSid is not linkable here (imports as __imp_).
enum class KnownSid : UCHAR {
        LocalSystem, // S-1-5-18
        BuiltinAdministrators // S-1-5-32-544
};

NTSTATUS build_known_sid_buffers(_Out_writes_bytes_(SECURITY_MAX_SID_SIZE) UCHAR *buf, _Out_ PULONG sid_len_bytes,
                               KnownSid ks)
{
        auto *s = reinterpret_cast<SID *>(buf);
        RtlZeroMemory(buf, SECURITY_MAX_SID_SIZE);
        s->Revision = SID_REVISION;
        s->IdentifierAuthority = SECURITY_NT_AUTHORITY;
        switch (ks) {
        case KnownSid::LocalSystem:
                s->SubAuthorityCount = 1;
                s->SubAuthority[0] = 18; // SECURITY_LOCAL_SYSTEM_RID
                break;
        case KnownSid::BuiltinAdministrators:
                s->SubAuthorityCount = 2;
                s->SubAuthority[0] = 32; // SECURITY_BUILTIN_DOMAIN_RID
                s->SubAuthority[1] = 544; // DOMAIN_ALIAS_RID_ADMINS
                break;
        default:
                return STATUS_INVALID_PARAMETER;
        }
        if (!RtlValidSid(reinterpret_cast<PSID>(buf))) {
                return STATUS_INVALID_SID;
        }
        *sid_len_bytes = RtlLengthSid(reinterpret_cast<PSID>(buf));
        return STATUS_SUCCESS;
}

NTSTATUS open_requestor_user_token(_In_ IRP *irp, _Out_ HANDLE *token_handle)
{
        *token_handle = nullptr;

        auto process = IoGetRequestorProcess(irp);
        if (!process) {
                return STATUS_INVALID_PARAMETER;
        }

        HANDLE proc{};
        NTSTATUS st =
                ObOpenObjectByPointer(
                        process,
                        OBJ_KERNEL_HANDLE,
                        nullptr,
                        PROCESS_QUERY_INFORMATION,
                        *PsProcessType,
                        KernelMode,
                        &proc);
        if (!NT_SUCCESS(st)) {
                return st;
        }

        HANDLE tok{};
        st = ZwOpenProcessTokenEx(proc, TOKEN_QUERY, OBJ_KERNEL_HANDLE, &tok);
        ZwClose(proc);
        if (!NT_SUCCESS(st)) {
                return st;
        }

        *token_handle = tok;
        return STATUS_SUCCESS;
}

bool unicode_contains_z(_In_z_ PCWSTR haystack, _In_z_ PCWSTR needle)
{
        if (!needle[0]) {
                return true;
        }

        for (; *haystack; ++haystack) {
                auto p = haystack;
                auto q = needle;
                while (*p && *q && *p == *q) {
                        ++p;
                        ++q;
                }
                if (*q == L'\0') {
                        return true;
                }
        }
        return false;
}

NTSTATUS token_user_sid_string(_In_ HANDLE token_handle, _Out_ UNICODE_STRING *sid_str)
{
        RtlInitUnicodeString(sid_str, nullptr);

        ULONG qlen{};
        NTSTATUS st = ZwQueryInformationToken(token_handle, TokenUser, nullptr, 0, &qlen);

        if (st != STATUS_BUFFER_TOO_SMALL || !qlen) {
                return NT_SUCCESS(st) ? STATUS_INVALID_PARAMETER : st;
        }

        auto tu = static_cast<PTOKEN_USER>(ExAllocatePoolWithTag(PagedPool, qlen, pooltag));
        if (!tu) {
                return STATUS_INSUFFICIENT_RESOURCES;
        }

        st = ZwQueryInformationToken(token_handle, TokenUser, tu, qlen, &qlen);
        if (!NT_SUCCESS(st)) {
                ExFreePoolWithTag(tu, pooltag);
                return st;
        }

        PSID sid = tu->User.Sid;
        st = RtlConvertSidToUnicodeString(sid_str, sid, static_cast<BOOLEAN>(TRUE));

        ExFreePoolWithTag(tu, pooltag);
        return st;
}

bool token_primary_matches_known_sid(_In_ HANDLE token_handle, KnownSid known)
{
        ULONG qlen{};
        if (ZwQueryInformationToken(token_handle, TokenUser, nullptr, 0, &qlen) != STATUS_BUFFER_TOO_SMALL ||
            !qlen) {
                return false;
        }

        auto tu = static_cast<PTOKEN_USER>(ExAllocatePoolWithTag(PagedPool, qlen, pooltag));
        if (!tu) {
                return false;
        }

        if (!NT_SUCCESS(ZwQueryInformationToken(token_handle, TokenUser, tu, qlen, &qlen))) {
                ExFreePoolWithTag(tu, pooltag);
                return false;
        }

        UCHAR well[SECURITY_MAX_SID_SIZE]{};
        ULONG wb{};
        const NTSTATUS st_well = build_known_sid_buffers(well, &wb, known);

        PSID usr = tu->User.Sid;
        const bool eq = NT_SUCCESS(st_well) && RtlEqualSid(usr, reinterpret_cast<PSID>(well)) != FALSE;

        ExFreePoolWithTag(tu, pooltag);

        return eq;
}

bool token_groups_include_known_sid(_In_ HANDLE token_handle, KnownSid known)
{
        UCHAR well[SECURITY_MAX_SID_SIZE]{};
        ULONG wb{};
        if (!NT_SUCCESS(build_known_sid_buffers(well, &wb, known))) {
                return false;
        }

        ULONG qlen{};
        if (ZwQueryInformationToken(token_handle, TokenGroups, nullptr, 0, &qlen) != STATUS_BUFFER_TOO_SMALL ||
            !qlen) {
                return false;
        }

        auto grp = static_cast<PTOKEN_GROUPS>(ExAllocatePoolWithTag(PagedPool, qlen, pooltag));
        if (!grp) {
                return false;
        }

        if (!NT_SUCCESS(ZwQueryInformationToken(token_handle, TokenGroups, grp, qlen, &qlen))) {
                ExFreePoolWithTag(grp, pooltag);
                return false;
        }

        bool found = false;
        for (ULONG i = 0; i < grp->GroupCount; ++i) {
                const SID_AND_ATTRIBUTES &gas = grp->Groups[i];
                const bool usable = ((gas.Attributes & SE_GROUP_USE_FOR_DENY_ONLY) == 0) &&
                                    RtlEqualSid(gas.Sid, reinterpret_cast<PSID>(well)) != FALSE;
                if (!usable || ((gas.Attributes & SE_GROUP_ENABLED) == 0)) {
                        continue;
                }
                found = true;
                break;
        }

        ExFreePoolWithTag(grp, pooltag);
        return found;
}

NTSTATUS check_sddl_vs_token(_In_reads_z_(sddl_buf_cch) wchar_t *sddl, _In_ HANDLE tok)
{
        UNICODE_STRING sid{};
        auto st = token_user_sid_string(tok, &sid);
        if (!NT_SUCCESS(st)) {
                return st;
        }

        wchar_t pattern[360]{};
        st = RtlStringCbPrintfW(pattern, sizeof(pattern), L"(A;;GA;;;%ws)", sid.Buffer);
        RtlFreeUnicodeString(&sid);

        if (!NT_SUCCESS(st)) {
                return st;
        }

        if (unicode_contains_z(sddl, pattern)) {
                return STATUS_SUCCESS;
        }

        if (token_groups_include_known_sid(tok, KnownSid::BuiltinAdministrators) &&
            unicode_contains_z(sddl, L"(A;;GA;;;BA)")) {
                return STATUS_SUCCESS;
        }

        if (token_primary_matches_known_sid(tok, KnownSid::LocalSystem) &&
            unicode_contains_z(sddl, L"(A;;GA;;;SY)")) {
                return STATUS_SUCCESS;
        }

        return STATUS_ACCESS_DENIED;
}

} // namespace

#pragma warning(pop)

EXTERN_C NTSTATUS dispatch_create(_In_ DEVICE_OBJECT *devobj, _Inout_ IRP *irp)
{
        PAGED_CODE();

        auto &f = *get_filter_ext(devobj);

        libdrv::RemoveLockGuard lck(f.remove_lock);

        if (auto err = lck.acquired()) {
                Trace(TRACE_LEVEL_ERROR, "Acquire remove lock %!STATUS!", err);

                return CompleteRequest(irp, err);
        }

        if (f.is_hub) {
                return ForwardIrp(f, irp);
        }

        wchar_t sddl[sddl_buf_cch]{};

        DEVPROPTYPE prop_type{};
        ULONG required_len{};

        NTSTATUS st = IoGetDevicePropertyData(
                f.pdo,
                &DEVPKEY_Device_SecuritySDS,
                LOCALE_NEUTRAL,
                0,
                sizeof(sddl) - sizeof(wchar_t),
                sddl,
                &required_len,
                &prop_type);

        if (!NT_SUCCESS(st) || prop_type != DEVPROP_TYPE_STRING || required_len < sizeof(wchar_t)) {
                return ForwardIrp(f, irp);
        }

        const ULONG wchars = required_len / static_cast<ULONG>(sizeof(wchar_t));
        if (!wchars || wchars >= sddl_buf_cch) {
                return ForwardIrp(f, irp);
        }

        sddl[sddl_buf_cch - 1] = L'\0';

        if (irp->RequestorMode == KernelMode) {

                return ForwardIrp(f, irp);
        }

        HANDLE token{};

        st = open_requestor_user_token(irp, &token);
        if (!NT_SUCCESS(st)) {
                return CompleteRequest(irp, st);
        }

        st = check_sddl_vs_token(sddl, token);
        ZwClose(token);

        if (!NT_SUCCESS(st)) {
                return CompleteRequest(irp, st);
        }

        return ForwardIrp(f, irp);
}
