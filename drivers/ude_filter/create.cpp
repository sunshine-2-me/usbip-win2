/*
 * Copyright (c) 2026
 */

#include "create.h"

#include "device.h"
#include "irp.h"
#include "trace.h"
#include "create.tmh"

#include <libdrv\remove_lock.h>

#include <initguid.h>
#include <devpkey.h>

EXTERN_C
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_same_
NTSTATUS IoGetRequestorSessionId(_In_ PIRP Irp, _Out_ PULONG SessionId);

namespace
{

using namespace usbip;

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void load_owner_session(_Inout_ filter_ext &fltr)
{
        auto &d = fltr.device;

        if (d.owner_loaded) {
                return;
        }

        d.owner_loaded = TRUE;
        d.owner_session_valid = FALSE;
        d.owner_session_id = 0;

        DEVPROPTYPE type{};
        ULONG session{};
        ULONG result_len = sizeof(session);

        auto st = IoGetDevicePropertyData(
                fltr.pdo,
                &DEVPKEY_Device_SessionId,
                LOCALE_NEUTRAL,
                0,
                sizeof(session),
                &session,
                &result_len,
                &type);

        if (!NT_SUCCESS(st)) {
                TraceDbg("FiDO %04x, IoGetDevicePropertyData(SessionId) %!STATUS!", ptr04x(fltr.self), st);
                return;
        }

        if (type != DEVPROP_TYPE_UINT32 || result_len != sizeof(session)) {
                TraceDbg("FiDO %04x, unexpected DEVPKEY_Device_SessionId type/len", ptr04x(fltr.self));
                return;
        }

        if (session == 0) {
                return;
        }

        d.owner_session_id = session;
        d.owner_session_valid = TRUE;
        TraceDbg("FiDO %04x, owner session %lu", ptr04x(fltr.self), session);
}

} // namespace


_Function_class_(DRIVER_DISPATCH)
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_same_
NTSTATUS usbip::dispatch_create(_In_ DEVICE_OBJECT *devobj, _Inout_ IRP *irp)
{
        auto &fltr = *get_filter_ext(devobj);

        libdrv::RemoveLockGuard lck(fltr.remove_lock);
        if (auto err = lck.acquired()) {
                Trace(TRACE_LEVEL_ERROR, "Acquire remove lock %!STATUS!", err);
                return libdrv::CompleteRequest(irp, err);
        }

        if (fltr.is_hub) {
                return ForwardIrp(fltr, irp);
        }

        load_owner_session(fltr);

        auto &d = fltr.device;

        if (!d.owner_session_valid) {
                return ForwardIrp(fltr, irp);
        }

        if (irp->RequestorMode == KernelMode) {
                return ForwardIrp(fltr, irp);
        }

        ULONG requestor_session{};
        const auto st = IoGetRequestorSessionId(irp, &requestor_session);
        if (!NT_SUCCESS(st)) {
                TraceDbg("FiDO %04x, IoGetRequestorSessionId %!STATUS!", ptr04x(fltr.self), st);
                return ForwardIrp(fltr, irp);
        }

        if (requestor_session == d.owner_session_id) {
                return ForwardIrp(fltr, irp);
        }

        Trace(TRACE_LEVEL_INFORMATION,
              "FiDO %04x, deny create requestor session %lu != owner %lu",
              ptr04x(fltr.self),
              requestor_session,
              d.owner_session_id);

        return libdrv::CompleteRequest(irp, STATUS_ACCESS_DENIED);
}
