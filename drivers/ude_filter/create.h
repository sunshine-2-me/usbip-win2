/*
 * IRP_MJ_CREATE session isolation for usbip2_filter.
 */
#pragma once

#include <libdrv\codeseg.h>

#include <wdm.h>

namespace usbip
{

_Function_class_(DRIVER_DISPATCH)
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_same_
NTSTATUS dispatch_create(_In_ DEVICE_OBJECT *devobj, _Inout_ IRP *irp);

} // namespace usbip
