/*
 * Copyright (c) 2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include <ntifs.h>
#include <ntstrsafe.h>

#pragma warning(push)
#pragma warning(disable : 4996)

#include "driver.h"
#include "security.h"
#include "trace.h"
#include "security.tmh"

#ifndef USBIP_SECURITY_INCLUDE_BUILTIN_ADMINS
#define USBIP_SECURITY_INCLUDE_BUILTIN_ADMINS 1
#endif

#ifndef PROCESS_QUERY_INFORMATION
#define PROCESS_QUERY_INFORMATION (ACCESS_MASK)(0x0400)
#endif

namespace usbip {

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS build_nt_auth_single_rid_sid(_Out_writes_bytes_(SECURITY_MAX_SID_SIZE) UCHAR *buf, ULONG rid)
{
        auto *s = reinterpret_cast<SID *>(buf);
        RtlZeroMemory(buf, SECURITY_MAX_SID_SIZE);
        s->Revision = SID_REVISION;
        s->SubAuthorityCount = 1;
        s->IdentifierAuthority = SECURITY_NT_AUTHORITY;
        s->SubAuthority[0] = rid;
        if (!RtlValidSid(reinterpret_cast<PSID>(buf))) {
                return STATUS_INVALID_SID;
        }
        return STATUS_SUCCESS;
}

static _IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED BOOLEAN is_well_known_sid_for_shared_attach(_In_ PSID Sid)
{
        PAGED_CODE();

        static const ULONG well_known_rids[] = {
                18, // LocalSystem  S-1-5-18
                19, // LocalService S-1-5-19
                20, // NetworkService S-1-5-20
        };

        UCHAR buf[SECURITY_MAX_SID_SIZE]{};

        for (auto rid : well_known_rids) {
                if (!NT_SUCCESS(build_nt_auth_single_rid_sid(buf, rid))) {
                        continue;
                }
                if (RtlEqualSid(Sid, reinterpret_cast<PSID>(buf))) {
                        return TRUE;
                }
        }

        return FALSE;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void capture_plugin_requestor_sid(_In_opt_ WDFREQUEST capture_request, _Inout_ device_ctx_ext &ext)
{
        PAGED_CODE();

        ext.owner_sid_valid = false;
        ext.owner_sid_length = 0;
        RtlZeroMemory(ext.owner_sid, sizeof(ext.owner_sid));

        if (!capture_request) {
                TraceDbg("owner sid: null request — shared attach");
                return;
        }

        auto irp = WdfRequestWdmGetIrp(capture_request);
        if (!irp) {
                TraceDbg("owner sid: no IRP");
                return;
        }

        auto process = IoGetRequestorProcess(irp);
        if (!process) {
                TraceDbg("owner sid: no requestor process");
                return;
        }

        HANDLE proc_handle{};
        if (auto st = ObOpenObjectByPointer(
                    process,
                    OBJ_KERNEL_HANDLE,
                    nullptr,
                    PROCESS_QUERY_INFORMATION,
                    *PsProcessType,
                    KernelMode,
                    &proc_handle)) {
                Trace(TRACE_LEVEL_ERROR, "ObOpenObjectByPointer(process) %!STATUS!", st);
                return;
        }

        HANDLE token_handle{};
        if (auto st = ZwOpenProcessTokenEx(proc_handle, TOKEN_QUERY, OBJ_KERNEL_HANDLE, &token_handle)) {
                Trace(TRACE_LEVEL_ERROR, "ZwOpenProcessTokenEx %!STATUS!", st);
                ZwClose(proc_handle);
                return;
        }

        ZwClose(proc_handle);

        ULONG qlen{};
        if (auto st = ZwQueryInformationToken(token_handle, TokenUser, nullptr, 0, &qlen);
            st != STATUS_BUFFER_TOO_SMALL || !qlen) {
                Trace(TRACE_LEVEL_ERROR, "ZwQueryInformationToken(size) %!STATUS!", st);
                ZwClose(token_handle);
                return;
        }

        auto user_info = static_cast<PTOKEN_USER>(ExAllocatePoolWithTag(PagedPool, qlen, pooltag));
        if (!user_info) {
                Trace(TRACE_LEVEL_ERROR, "ExAllocatePool TokenUser %lu", qlen);
                ZwClose(token_handle);
                return;
        }

        if (auto st = ZwQueryInformationToken(token_handle, TokenUser, user_info, qlen, &qlen)) {
                Trace(TRACE_LEVEL_ERROR, "ZwQueryInformationToken %!STATUS!", st);
                ExFreePoolWithTag(user_info, pooltag);
                ZwClose(token_handle);
                return;
        }

        ZwClose(token_handle);

        PSID usersid = user_info->User.Sid;
        if (!RtlValidSid(usersid)) {
                Trace(TRACE_LEVEL_ERROR, "TokenUser.Sid invalid");
                ExFreePoolWithTag(user_info, pooltag);
                return;
        }

        if (is_well_known_sid_for_shared_attach(usersid)) {
                Trace(TRACE_LEVEL_INFORMATION, "owner sid: system/service — unstamped/shared");
                ExFreePoolWithTag(user_info, pooltag);
                return;
        }

        ULONG sid_length = RtlLengthSid(usersid);
        if (!sid_length || sid_length > sizeof(ext.owner_sid)) {
                Trace(TRACE_LEVEL_ERROR, "SID length unreasonable %lu", sid_length);
                ExFreePoolWithTag(user_info, pooltag);
                return;
        }

        RtlCopyMemory(ext.owner_sid, usersid, sid_length);
        ext.owner_sid_length = sid_length;
        ext.owner_sid_valid = TRUE;

        Trace(TRACE_LEVEL_INFORMATION, "captured attach owner sid, bytes %lu", sid_length);

        ExFreePoolWithTag(user_info, pooltag);
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS build_owner_sddl(
        _In_ const device_ctx_ext &ext,
        _Out_writes_bytes_(wchar_count * sizeof(WCHAR)) PWCHAR sddl_dst,
        _In_ ULONG wchar_count,
        _Out_ ULONG *wchar_out)
{
        PAGED_CODE();

        *wchar_out = 0;

        if (!ext.owner_sid_valid || !ext.owner_sid_length) {
                return STATUS_INVALID_PARAMETER;
        }

        auto sid_psid = reinterpret_cast<PSID>(const_cast<UCHAR *>(ext.owner_sid));

        UNICODE_STRING sid_str{};

        auto st = RtlConvertSidToUnicodeString(&sid_str, sid_psid, static_cast<BOOLEAN>(TRUE));
        if (!NT_SUCCESS(st)) {
                Trace(TRACE_LEVEL_ERROR, "RtlConvertSidToUnicodeString %!STATUS!", st);
                return st;
        }

        const wchar_t *fmt =
#if USBIP_SECURITY_INCLUDE_BUILTIN_ADMINS
                L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;%ws)";
#else
                L"D:P(A;;GA;;;SY)(A;;GA;;;%ws)";
#endif

        st = RtlStringCbPrintfW(sddl_dst, wchar_count * sizeof(WCHAR), fmt, sid_str.Buffer);
        RtlFreeUnicodeString(&sid_str);

        if (!NT_SUCCESS(st)) {
                Trace(TRACE_LEVEL_ERROR, "RtlStringCbPrintfW sddl %!STATUS!", st);
                return st;
        }

        size_t nbytes{};
        st = RtlStringCbLengthW(sddl_dst, wchar_count * sizeof(WCHAR), &nbytes);
        if (!NT_SUCCESS(st)) {
                return st;
        }

        *wchar_out = static_cast<ULONG>(nbytes / sizeof(WCHAR) + 1);
        return STATUS_SUCCESS;
}

} // namespace usbip

#pragma warning(pop)
