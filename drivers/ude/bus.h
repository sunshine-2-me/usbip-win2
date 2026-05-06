#pragma once

#include "context.h"

namespace usbip::bus
{

struct session_hc_id
{
        WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER header;
        ULONG session_id;
};

struct bus_ctx
{
        WDFWAITLOCK lock;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(bus_ctx, get_bus_ctx)

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS device_add(_Inout_ WDFDEVICE_INIT *init);

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void mark_session_controller_in_use(_In_ WDFDEVICE vhci, _In_ bool in_use);

} // namespace usbip::bus
