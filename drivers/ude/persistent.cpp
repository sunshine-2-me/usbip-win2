/*
 * Copyright (c) 2023-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include <ntifs.h>

#include "persistent.h"
#include "trace.h"
#include "persistent.tmh"

#include "driver.h"
#include "context.h"
#include "vhci.h"

#include <libdrv/strconv.h>
#include <resources/messages.h>

#include <ntstrsafe.h>

namespace 
{

using namespace usbip;

/*
 * Context space for WDFREQUEST which is used for attach attempts.
 */
struct attach_ctx
{
        ULONG location_hash; // hash(host,port,busid)

        WDFDEVICE vhci;
        WDFTIMER timer;

        WDFMEMORY inbuf;
        WDFMEMORY outbuf;

        unsigned int retry_cnt;
        unsigned int delay;

        // Per-user isolation. If non-zero, inbuf is sized for
        // vhci::ioctl::plugin_hardware_for_user instead of plugin_hardware
        // and the issuing IOCTL is PLUGIN_HARDWARE_FOR_USER.
        bool for_user;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(attach_ctx, get_attach_ctx);

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto empty(_In_ const UNICODE_STRING &s)
{
        PAGED_CODE();
        return libdrv::empty(s) || !*s.Buffer;
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
auto reattach_req_count(_Inout_ vhci_ctx &vhci)
{
        wdf::Lock(vhci.reattach_req_lock);
        return WdfCollectionGetCount(vhci.reattach_req);
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
auto reattach_req_add(_Inout_ vhci_ctx &vhci, _In_ WDFOBJECT request)
{
        wdf::Lock lck(vhci.reattach_req_lock);

        if (auto err = WdfCollectionAdd(vhci.reattach_req, request)) {
                Trace(TRACE_LEVEL_ERROR, "%04x, WdfCollectionAdd %!STATUS!", ptr04x(request), err);
                return false;
        }

        return true;
}

/*
 * WdfCollectionRemove issues bugcheck if object is not found in collection.
 * imp_WdfCollectionRemove:WDFOBJECT XXX not in WDFCOLLECTION XXX, 0xc0000225(STATUS_NOT_FOUND)
 */
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void reattach_req_remove(_Inout_ vhci_ctx &vhci, _In_ WDFOBJECT request)
{
        auto col = vhci.reattach_req;
        wdf::Lock lck(vhci.reattach_req_lock);

        for (auto n = WdfCollectionGetCount(col), i = 0UL; i < n; ++i) {

                if (WdfCollectionGetItem(col, i) == request) {
                        WdfCollectionRemoveItem(col, i);
                        break;
                }
        }
}

/**
 * @param location_hash remove unconditionally if zero
 */
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
auto reattach_req_remove(_Inout_ vhci_ctx &vhci, _In_ ULONG location_hash)
{
        wdf::ObjectRef ref;
        auto col = vhci.reattach_req;

        wdf::Lock lck(vhci.reattach_req_lock);

        for (auto n = WdfCollectionGetCount(col), i = 0UL; i < n; ++i) {

                auto req = WdfCollectionGetItem(col, i);
                
                if (!location_hash || location_hash == get_attach_ctx(req)->location_hash) {
                        ref.reset(req);
                        WdfCollectionRemoveItem(col, i);
                        break;
                }
        }

        return ref;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto get_persistent_devices(_In_ WDFKEY key)
{
        PAGED_CODE();
        ObjectDelete col;
        
        if (WDFCOLLECTION h; auto err = WdfCollectionCreate(WDF_NO_OBJECT_ATTRIBUTES, &h)) {
                Trace(TRACE_LEVEL_ERROR, "WdfCollectionCreate %!STATUS!", err);
                return col;
        } else {
                col.reset(h);
        }

        WDF_OBJECT_ATTRIBUTES str_attr;
        WDF_OBJECT_ATTRIBUTES_INIT(&str_attr);
        str_attr.ParentObject = col.get();

        UNICODE_STRING value_name;
        RtlUnicodeStringInit(&value_name, persistent_devices_value_name);

        if (auto err = WdfRegistryQueryMultiString(key, &value_name, &str_attr, col.get<WDFCOLLECTION>())) {
                Trace(TRACE_LEVEL_ERROR, "WdfRegistryQueryMultiString('%!USTR!') %!STATUS!", &value_name, err);
                col.reset();
        }

        return col;
}

/*
 * @see set_persistent/get_persistent
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto get_persistent_devices(_Inout_ ULONG &cnt, _In_ ULONG max_cnt)
{
        PAGED_CODE();
        ObjectDelete col;

        if (Registry key; NT_SUCCESS(open(key, DriverRegKeyPersistentState))) {
                col = get_persistent_devices(key.get());
        }

        cnt = col ? min(WdfCollectionGetCount(col.get<WDFCOLLECTION>()), max_cnt) : 0;
        return col;
}

/*
 * Parse the textual representation of a SID ("S-1-5-21-..."/"S-1-5-18"/...) into
 * a self-relative SID stored in @a buf. Self-contained kernel implementation;
 * no advapi32 equivalent exists in kernel mode.
 *
 * @param str  null-terminated wide string (NOT a UNICODE_STRING; first non-digit
 *             after the trailing sub-authority terminates the parse)
 * @param buf  caller-owned buffer of at least SECURITY_MAX_SID_SIZE bytes
 * @param buf_size  size of @a buf in bytes
 * @param out_size  receives RtlLengthSid(buf) on success
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS parse_string_sid(
        _In_ PCWSTR str, _Out_writes_bytes_(buf_size) PSID buf,
        _In_ ULONG buf_size, _Out_ ULONG &out_size)
{
        PAGED_CODE();
        out_size = 0;

        if (!str || str[0] != L'S' || str[1] != L'-' || str[2] != L'1' || str[3] != L'-') {
                return STATUS_INVALID_PARAMETER;
        }

        auto p = str + 4;

        // Authority: decimal up to 2^32-1 or hex 0x... up to 2^48-1.
        ULONG64 auth = 0;
        if (p[0] == L'0' && (p[1] == L'x' || p[1] == L'X')) {
                p += 2;
                while (*p && *p != L'-') {
                        ULONG digit;
                        if (*p >= L'0' && *p <= L'9') digit = *p - L'0';
                        else if (*p >= L'a' && *p <= L'f') digit = 10 + (*p - L'a');
                        else if (*p >= L'A' && *p <= L'F') digit = 10 + (*p - L'A');
                        else return STATUS_INVALID_PARAMETER;
                        auth = auth * 16 + digit;
                        ++p;
                }
        } else {
                while (*p && *p != L'-') {
                        if (*p < L'0' || *p > L'9') return STATUS_INVALID_PARAMETER;
                        auth = auth * 10 + (*p - L'0');
                        ++p;
                }
        }

        SID_IDENTIFIER_AUTHORITY id_auth{};
        for (int i = 5; i >= 0; --i) {
                id_auth.Value[i] = static_cast<UCHAR>(auth & 0xFF);
                auth >>= 8;
        }

        ULONG sub_auths[SID_MAX_SUB_AUTHORITIES]{};
        UCHAR sub_count = 0;

        while (*p == L'-') {
                ++p;
                if (sub_count >= SID_MAX_SUB_AUTHORITIES) {
                        return STATUS_INVALID_PARAMETER;
                }
                if (*p < L'0' || *p > L'9') return STATUS_INVALID_PARAMETER;

                ULONG sub = 0;
                while (*p >= L'0' && *p <= L'9') {
                        sub = sub * 10 + (*p - L'0');
                        ++p;
                }
                sub_auths[sub_count++] = sub;
        }

        if (*p != L'\0' || !sub_count) {
                return STATUS_INVALID_PARAMETER;
        }

        auto required = RtlLengthRequiredSid(sub_count);
        if (buf_size < required) {
                return STATUS_BUFFER_TOO_SMALL;
        }

        if (NTSTATUS st = RtlInitializeSid(buf, &id_auth, sub_count); !NT_SUCCESS(st)) {
                return st;
        }

        for (UCHAR i = 0; i < sub_count; ++i) {
                *RtlSubAuthoritySid(buf, i) = sub_auths[i];
        }

        if (!RtlValidSid(buf)) {
                return STATUS_INVALID_PARAMETER;
        }

        out_size = RtlLengthSid(buf);
        return STATUS_SUCCESS;
}

/*
 * @param r must be zeroed
 * @param device_str host,port,busid[;sid=<sid_string>]
 * @param sid_buf optional, receives the parsed self-relative SID (must be at least
 *                SECURITY_MAX_SID_SIZE bytes). On success *sid_size is set.
 *                If absent, an embedded ;sid=... suffix is rejected as invalid.
 * @see hash_location
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto parse_device_str(
        _Inout_ device_attributes &r,
        _In_ const UNICODE_STRING &device_str,
        _Out_writes_bytes_opt_(sid_buf_size) PSID sid_buf = nullptr,
        _In_ ULONG sid_buf_size = 0,
        _Out_opt_ ULONG *sid_size = nullptr)
{
        PAGED_CODE();
        const auto sep = L',';

        libdrv::split(r.node_name, r.busid, device_str, sep);
        if (empty(r.node_name)) {
                return STATUS_INVALID_PARAMETER;
        }

        libdrv::split(r.service_name, r.busid, r.busid, sep);

        if (empty(r.service_name) || empty(r.busid)) {
                return STATUS_INVALID_PARAMETER;
        }

        // Per-user isolation: optional ;sid=S-1-5-... suffix in busid.
        if (sid_size) {
                *sid_size = 0;
        }

        const wchar_t tag[] = L";sid=";
        const USHORT tag_chars = ARRAYSIZE(tag) - 1;
        const auto busid_chars = static_cast<USHORT>(r.busid.Length / sizeof(wchar_t));

        USHORT sid_at = 0;
        bool found = false;
        if (r.busid.Buffer && busid_chars >= tag_chars) {
                for (USHORT i = 0; i + tag_chars <= busid_chars; ++i) {
                        if (RtlCompareMemory(r.busid.Buffer + i, tag,
                                              tag_chars * sizeof(wchar_t)) == tag_chars * sizeof(wchar_t)) {
                                sid_at = i;
                                found = true;
                                break;
                        }
                }
        }

        if (found) {
                if (!sid_buf) {
                        return STATUS_INVALID_PARAMETER;
                }

                auto chars_after = static_cast<USHORT>(busid_chars - sid_at - tag_chars);

                wchar_t sid_str[96]{};
                if (chars_after >= ARRAYSIZE(sid_str)) {
                        return STATUS_INVALID_PARAMETER;
                }

                RtlCopyMemory(sid_str,
                              r.busid.Buffer + sid_at + tag_chars,
                              chars_after * sizeof(wchar_t));
                sid_str[chars_after] = L'\0';

                ULONG cb = 0;
                if (auto err = parse_string_sid(sid_str, sid_buf, sid_buf_size, cb); NT_ERROR(err)) {
                        Trace(TRACE_LEVEL_ERROR, "parse_string_sid('%S') %!STATUS!", sid_str, err);
                        return err;
                }

                if (sid_size) {
                        *sid_size = cb;
                }

                // Trim ;sid=... off busid in-place.
                r.busid.Length = sid_at * sizeof(wchar_t);
                r.busid.MaximumLength = r.busid.Length;
        }

        return hash_location(r.location_hash, r);
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED bool create_inbuf(
        _Inout_ WDFMEMORY &result, _Inout_ vhci::ioctl::plugin_hardware* &req, _Inout_ WDF_OBJECT_ATTRIBUTES &attr)
{
        PAGED_CODE();
        NT_ASSERT(!result);

        if (auto err = WdfMemoryCreate(&attr, NonPagedPoolNx, 0, sizeof(*req), &result, reinterpret_cast<PVOID*>(&req))) {
                Trace(TRACE_LEVEL_ERROR, "WdfMemoryCreate %!STATUS!", err);
                req = nullptr;
        }

        return req;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED bool create_inbuf_for_user(
        _Inout_ WDFMEMORY &result, _Inout_ vhci::ioctl::plugin_hardware_for_user* &req, _Inout_ WDF_OBJECT_ATTRIBUTES &attr)
{
        PAGED_CODE();
        NT_ASSERT(!result);

        if (auto err = WdfMemoryCreate(&attr, NonPagedPoolNx, 0, sizeof(*req), &result, reinterpret_cast<PVOID*>(&req))) {
                Trace(TRACE_LEVEL_ERROR, "WdfMemoryCreate %!STATUS!", err);
                req = nullptr;
        }

        return req;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED bool create_outbuf(
        _Inout_ WDFMEMORY &result, _In_ vhci::ioctl::plugin_hardware *req, _Inout_ WDF_OBJECT_ATTRIBUTES &attr)
{
        PAGED_CODE();
        NT_ASSERT(!result);

        constexpr auto len = offsetof(vhci::ioctl::plugin_hardware, port) + sizeof(req->port);

        if (auto err = WdfMemoryCreatePreallocated(&attr, req, len, &result)) {
                Trace(TRACE_LEVEL_ERROR, "WdfMemoryCreatePreallocated %!STATUS!", err);
        }

        return result;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED bool create_outbuf_for_user(
        _Inout_ WDFMEMORY &result, _In_ vhci::ioctl::plugin_hardware_for_user *req, _Inout_ WDF_OBJECT_ATTRIBUTES &attr)
{
        PAGED_CODE();
        NT_ASSERT(!result);

        constexpr auto len = offsetof(vhci::ioctl::plugin_hardware_for_user, port) + sizeof(req->port);

        if (auto err = WdfMemoryCreatePreallocated(&attr, req, len, &result)) {
                Trace(TRACE_LEVEL_ERROR, "WdfMemoryCreatePreallocated %!STATUS!", err);
        }

        return result;
}

_Function_class_(EVT_WDF_REQUEST_COMPLETION_ROUTINE)
_IRQL_requires_same_
void on_plugin_hardware(
        _In_ WDFREQUEST request, _In_ WDFIOTARGET, _In_ WDF_REQUEST_COMPLETION_PARAMS*, _In_ WDFCONTEXT)
{
        ObjectDelete ptr(request);

        auto &req = *get_attach_ctx(request);
        auto &vhci = *get_vhci_ctx(req.vhci);

        auto retry_cnt = req.retry_cnt++; // from zero

        auto st = WdfRequestGetStatus(request);
        auto failed = NT_ERROR(st);
        auto retry = failed && retry_cnt < vhci.reattach_max_attempts &&
                     can_reattach(req.vhci, req.location_hash, st);

        if (auto ok = retry && !get_flag(vhci.removing); !ok) {
                auto s = !failed ? " " : // "" prints as "<NULL>"
                         !retry ? ", cannot retry" : 
                         ", vhci is being removing";

                TraceDbg("req %04x, %!STATUS!%s", ptr04x(request), st, s);
                return;
        }

        WDF_REQUEST_REUSE_PARAMS params;
        WDF_REQUEST_REUSE_PARAMS_INIT(&params, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);

        if (auto err = WdfRequestReuse(request, &params)) {
                Trace(TRACE_LEVEL_ERROR, "WdfRequestReuse(%04x) %!STATUS!", ptr04x(request), err);
                return;
        }

        auto delay = req.delay;
        req.delay = get_next_delay(delay, vhci.reattach_max_delay);

        TraceDbg("req %04x, %!STATUS!, retry #%u in %u secs.", ptr04x(request), st, retry_cnt, delay);
        NT_VERIFY(!WdfTimerStart(req.timer, WDF_REL_TIMEOUT_IN_SEC(delay))); // @see on_attach_timer

        ptr.release();
}

/*
 * WDF_REQUEST_SEND_OPTIONS opts;
 * WDF_REQUEST_SEND_OPTIONS_INIT(&opts, 0);
 * WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opts, WDF_REL_TIMEOUT_IN_SEC(60));
 */
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void send_plugin_hardware(
        _In_ WDFIOTARGET target, _In_ WDFMEMORY inbuf, _In_ WDFMEMORY outbuf, _Inout_ ObjectDelete &req)
{
        auto request = req.get<WDFREQUEST>();
        auto &r = *get_attach_ctx(request);
        TraceDbg("req %04x, for_user %!BOOLEAN!", ptr04x(request), r.for_user);

        // Persistent reattach with an explicit owner SID uses the dedicated
        // PLUGIN_HARDWARE_FOR_USER IOCTL. The kernel-issued request is treated
        // as LocalSystem by the dispatcher, which is the only caller allowed to
        // use that IOCTL. Without an SID we use PLUGIN_HARDWARE_ONCE which
        // captures the System token of the kernel-side caller (legacy / admin).
        auto code = r.for_user ? vhci::ioctl::PLUGIN_HARDWARE_FOR_USER
                               : vhci::ioctl::PLUGIN_HARDWARE_ONCE;

        if (auto err = WdfIoTargetFormatRequestForIoctl(target, request,
                        code, inbuf, nullptr, outbuf, nullptr)) {
                Trace(TRACE_LEVEL_ERROR, "WdfIoTargetFormatRequestForIoctl %!STATUS!", err);
                return;
        }

        WdfRequestSetCompletionRoutine(request, on_plugin_hardware, WDF_NO_CONTEXT);

        if (!WdfRequestSend(request, target, WDF_NO_SEND_OPTIONS)) {
                auto err = WdfRequestGetStatus(request);
                Trace(TRACE_LEVEL_ERROR, "WdfRequestSend %!STATUS!", err);
        } else {
                req.release();
        }
}

/*
 * If an EvtTimerFunc callback function running at PASSIVE_LEVEL calls WdfObjectDelete,
 * this results in deadlock. Request is a parent of a timer, the timer will be
 * implicitly deleted when the request is deleted.
 */
_Function_class_(EVT_WDF_TIMER)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void on_attach_timer(_In_ WDFTIMER timer)
{
        ObjectDelete req(WdfTimerGetParentObject(timer));

        auto &r = *get_attach_ctx(req.get());
        auto &vhci = *get_vhci_ctx(r.vhci);

        if (get_flag(vhci.removing)) [[unlikely]] {
                TraceDbg("req %04x, vhci is being removing", ptr04x(req.get()));
        } else {
                send_plugin_hardware(vhci.target_self, r.inbuf, r.outbuf, req);
        }
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED bool create_timer(_Inout_ WDFTIMER &result, _Inout_ WDF_OBJECT_ATTRIBUTES &attr)
{
        PAGED_CODE();
        NT_ASSERT(!result);

        WDF_TIMER_CONFIG cfg;
        WDF_TIMER_CONFIG_INIT(&cfg, on_attach_timer);
        cfg.TolerableDelay = TolerableDelayUnlimited;

        if (auto err = WdfTimerCreate(&cfg, &attr, &result)) {
                Trace(TRACE_LEVEL_ERROR, "WdfTimerCreate %!STATUS!", err);
        }

        return result;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto init_attach_ctx(
        _Inout_ vhci_ctx &vhci,
        _Inout_ attach_ctx &r,
        _In_ const device_attributes &attr,
        _In_reads_bytes_opt_(sid_size) PSID owner_sid,
        _In_ ULONG sid_size)
{
        PAGED_CODE();

        r.location_hash = attr.location_hash;
        NT_ASSERT(r.location_hash);

        r.delay = vhci.reattach_first_delay;

        size_t len;
        if (r.for_user) {
                auto &req = *static_cast<vhci::ioctl::plugin_hardware_for_user*>(WdfMemoryGetBuffer(r.inbuf, &len));
                NT_ASSERT(len == sizeof(req));

                RtlZeroMemory(&req, sizeof(req));
                req.size = sizeof(req);

                if (sid_size && sid_size <= sizeof(req.sid) && owner_sid) {
                        RtlCopyMemory(req.sid, owner_sid, sid_size);
                        req.sid_size = sid_size;
                } else {
                        return false;
                }

                return NT_SUCCESS(fill_location(req, attr));
        }

        auto &req = *static_cast<vhci::ioctl::plugin_hardware*>(WdfMemoryGetBuffer(r.inbuf, &len));
        NT_ASSERT(len == sizeof(req));

        RtlZeroMemory(&req, sizeof(req));
        req.size = sizeof(req);

        return NT_SUCCESS(fill_location(req, attr));
}

_Function_class_(EVT_WDF_OBJECT_CONTEXT_CLEANUP)
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void cleanup_attach_request(_In_ WDFOBJECT obj)
{
        PAGED_CODE();
        TraceDbg("%04x", ptr04x(obj));

        auto r = get_attach_ctx(obj);
        auto vhci = get_vhci_ctx(r->vhci);

        reattach_req_remove(*vhci, obj);
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto create_attach_request(
        _In_ WDFDEVICE vhci,
        _In_ vhci_ctx &ctx,
        _In_ const device_attributes &dev,
        _In_reads_bytes_opt_(sid_size) PSID owner_sid,
        _In_ ULONG sid_size)
{
        PAGED_CODE();

        WDF_OBJECT_ATTRIBUTES attr;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, attach_ctx);
        attr.EvtCleanupCallback = cleanup_attach_request;
        attr.ParentObject = vhci;

        auto req = create_request(ctx.target_self, attr);
        if (!req) {
                return req;
        }

        Trace(TRACE_LEVEL_INFORMATION, "%04x, %!USTR!:%!USTR!/%!USTR!, hash %lx, sid_size %lu",
                ptr04x(req.get()), &dev.node_name, &dev.service_name, &dev.busid, dev.location_hash, sid_size);

        auto &r = *get_attach_ctx(req.get());
        r.vhci = vhci;
        r.for_user = (owner_sid && sid_size);

        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ParentObject = req.get();

        bool ok = false;

        if (r.for_user) {
                vhci::ioctl::plugin_hardware_for_user *buf{};
                ok = create_inbuf_for_user(r.inbuf, buf, attr) &&
                     create_outbuf_for_user(r.outbuf, buf, attr) &&
                     create_timer(r.timer, attr) &&
                     init_attach_ctx(ctx, r, dev, owner_sid, sid_size) &&
                     reattach_req_add(ctx, req.get());
        } else {
                vhci::ioctl::plugin_hardware *buf{};
                ok = create_inbuf(r.inbuf, buf, attr) &&
                     create_outbuf(r.outbuf, buf, attr) &&
                     create_timer(r.timer, attr) &&
                     init_attach_ctx(ctx, r, dev, nullptr, 0) &&
                     reattach_req_add(ctx, req.get());
        }

        if (!ok) {
                req.reset();
        }

        return req;
}

/*
 * WDF does not have a function for DriverRegKeySharedPersistentState yet.
 *
 * To debug persistent devices, add this line to .inf and make function to always return WdfDriverOpenParametersRegistryKey.
 * HKR, Parameters, PersistentDevices, 0x00010000, "pc,3240,3-3", "pc,3240,3-2","google.com,3240,3-1","microsoft.com,3240,3-4"
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED decltype(WdfDriverOpenParametersRegistryKey) *get_function(_In_ DRIVER_REGKEY_TYPE type)
{
        PAGED_CODE();

        switch (type) {
        case DriverRegKeyParameters:
                return WdfDriverOpenParametersRegistryKey;
        case DriverRegKeyPersistentState:
                return WdfDriverOpenPersistentStateRegistryKey;
//      case DriverRegKeySharedPersistentState: // since NTDDI_WIN10_FE
        default:
                return nullptr;
        }
}

} // namespace 

/*
 * There can be many reattach requests that are canceled but its timer callback has not been called yet.
 * Because of that the limit of active requests is 4x higher than the number of hub ports.
 * @see stop_attach_attempts
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void usbip::start_attach_attempts(
        _In_ WDFDEVICE vhci, _Inout_ vhci_ctx &ctx, _In_ const device_attributes &attr, _In_ bool delayed,
        _In_reads_bytes_opt_(sid_size) PSID owner_sid, _In_ ULONG sid_size)
{
        PAGED_CODE();

        if (get_flag(ctx.removing)) {
                TraceDbg("vhci is being removing");
        } else if (auto cnt = reattach_req_count(ctx); cnt >= 4*static_cast<ULONG>(ctx.devices_cnt)) {
                Trace(TRACE_LEVEL_WARNING, "too many active attach requests, %lu", cnt);
        } else if (auto req = create_attach_request(vhci, ctx, attr, owner_sid, sid_size); !req) {
                //
        } else if (auto &r = *get_attach_ctx(req.get()); !delayed) {
                send_plugin_hardware(ctx.target_self, r.inbuf, r.outbuf, req);
        } else {
                ++r.retry_cnt;

                enum { DELAY = 30 }; // long delay after UdecxUsbDevicePlugOutAndDelete
                NT_VERIFY(!WdfTimerStart(r.timer, WDF_REL_TIMEOUT_IN_SEC(DELAY)));

                TraceDbg("req %04x, delayed for %u secs.", ptr04x(req.get()), DELAY);
                req.release();
        }
}

/*
 * You cannot access request's context space here even when an extra reference is acquired.
 * The request can be in undefined state (completing/completed), a BSOD can occur.
 *
 * The timer callback will be fired. If it calls WdfRequestSend, STATUS_CANCELLED
 * will be immetiately returned due to previosly called WdfRequestCancelSentRequest.
 * WdfRequestReuse does not clear cancellation flag.
 *
 * If timer in system queue, its callback will delete cancelled request sooner or later.
 * The latter is a problem, these requests will be piling up and can cause Denial Of Service.
 */
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
int usbip::stop_attach_attempts(_Inout_ vhci_ctx &vhci, _In_ ULONG location_hash)
{
        int cnt = 0;

        while (auto req = reattach_req_remove(vhci, location_hash)) {

                ++cnt;
                auto delivered = WdfRequestCancelSentRequest(req.get<WDFREQUEST>());

                TraceDbg("hash %lx -> req %04x, cancel request was delivered %!BOOLEAN!",
                          location_hash, ptr04x(req.get()), delivered);
        }

        return cnt;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void usbip::plugin_persistent_devices(_In_ WDFDEVICE vhci)
{
        PAGED_CODE();
        auto &ctx = *get_vhci_ctx(vhci);

        ULONG cnt{};
        auto col = get_persistent_devices(cnt, ctx.devices_cnt);

        for (ULONG i = 0; i < cnt; ++i) {

                auto str = (WDFSTRING)WdfCollectionGetItem(col.get<WDFCOLLECTION>(), i);

                UNICODE_STRING device_str;
                WdfStringGetUnicodeString(str, &device_str);

                // Per-user isolation: persistent record may carry trailing ;sid=...
                // Stack-allocated buffer is zeroed and used only for this iteration.
                UCHAR sid_buf[SECURITY_MAX_SID_SIZE]{};
                ULONG sid_size = 0;

                if (device_attributes attr{}; auto err = parse_device_str(
                                attr, device_str, sid_buf,
                                static_cast<ULONG>(sizeof(sid_buf)), &sid_size)) {
                        Trace(TRACE_LEVEL_ERROR, "parse_device_str(%!USTR!) %!STATUS!", &device_str, err);
                } else {
                        start_attach_attempts(vhci, ctx, attr,
                                              false /* delayed */,
                                              sid_size ? sid_buf : nullptr,
                                              sid_size);
                }
        }
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS usbip::fill_location(
        _Inout_ vhci::imported_device_location &r, _In_ const device_attributes &attr)
{
        PAGED_CODE();

        struct {
                char *dst;
                USHORT dst_sz;
                const UNICODE_STRING &src;
        } const v[] = {
                { r.host, sizeof(r.host), attr.node_name },
                { r.service, sizeof(r.service), attr.service_name },
                { r.busid, sizeof(r.busid), attr.busid },
        };

        for (auto &[dst, dst_sz, src]: v) {
                if (auto err = libdrv::unicode_to_utf8(dst, dst_sz, src)) {
                        Trace(TRACE_LEVEL_ERROR, "unicode_to_utf8('%!USTR!') %!STATUS!", &src, err);
                        return err;
                }
        }

        return STATUS_SUCCESS;
}

/*
 * WdfDriverOpenParametersRegistryKey should not be used for write,
 * use WdfDriverOpenPersistentStateRegistryKey instead.
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS usbip::open(_Inout_ Registry &key, _In_ DRIVER_REGKEY_TYPE type, _In_ ACCESS_MASK access)
{
        PAGED_CODE();
        WDFKEY h{};

        auto f = get_function(type);
        auto st = f ? f(WdfGetDriver(), access, WDF_NO_OBJECT_ATTRIBUTES, &h) : STATUS_INVALID_PARAMETER;

        if (NT_ERROR(st)) {
                Trace(TRACE_LEVEL_ERROR, "%!DRIVER_REGKEY_TYPE!, access %#lx, %!STATUS!", type, access, st);
        }

        key.reset(h);
        return st;
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
ObjectDelete usbip::create_request(_In_ WDFIOTARGET target, _In_ WDF_OBJECT_ATTRIBUTES &attr)
{
        ObjectDelete ptr;

        if (WDFREQUEST req; auto err = WdfRequestCreate(&attr, target, &req)) {
                Trace(TRACE_LEVEL_ERROR, "WdfRequestCreate %!STATUS!", err);
        } else {
                ptr.reset(req);
        }

        return ptr;
}

/*
 * WskGetAddressInfo() can return STATUS_INTERNAL_ERROR(0xC00000E5), but after some delay it will succeed.
 * This can happen after reboot if dnscache(?) service is not ready yet.
 *
 * USBIP_ERROR_ST_DEV_BUSY is here because you call attach, it can fail and start attach attemtps.
 * You call attach again, it can succeed, but background attach attempts will not be stopped.
 */
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
bool usbip::can_reattach(_In_ WDFDEVICE vhci, _In_ ULONG location_hash, _In_ NTSTATUS status)
{
        NT_ASSERT(NT_ERROR(status));

        switch (as_usbip_status(status)) {
        case USBIP_ERROR_ABI:
        case USBIP_ERROR_VERSION:
        case USBIP_ERROR_PROTOCOL:
                return false; // unrecoverable errors
        case USBIP_ERROR_ST_DEV_BUSY:
                return !vhci::has_device(vhci, location_hash);
        }

        return status != STATUS_CANCELLED;
}

 /*
 * @see parse_device_str
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS usbip::hash_location(_Inout_ ULONG &hash, _In_ const device_attributes &r)
{
        PAGED_CODE();
        hash = 0;

        static_assert(sizeof(L",,") == 3*sizeof(wchar_t)); // must have space for null terminator
        USHORT cb = r.node_name.Length + r.service_name.Length + r.busid.Length + sizeof(L",,"); // see format string

        unique_ptr buf(libdrv::uninitialized, PagedPool, cb);
        if (!buf) {
                Trace(TRACE_LEVEL_ERROR, "Cannot allocate %d bytes", cb);
                return USBD_STATUS_INSUFFICIENT_RESOURCES;
        }

        UNICODE_STRING str { 
                .MaximumLength = cb, 
                .Buffer = buf.get<wchar_t>()
        };

        if (auto err = RtlUnicodeStringPrintf(&str, L"%wZ,%wZ,%wZ", &r.node_name, &r.service_name, &r.busid)) {
                Trace(TRACE_LEVEL_ERROR, "RtlUnicodeStringPrintf %!STATUS!", err);
                return err;
        }

        if (auto err = RtlHashUnicodeString(&str, true, HASH_STRING_ALGORITHM_DEFAULT, &hash)) {
                Trace(TRACE_LEVEL_ERROR, "RtlHashUnicodeString('%!USTR!') %!STATUS!", &str, err);
                return err;
        }

        return STATUS_SUCCESS;
}
