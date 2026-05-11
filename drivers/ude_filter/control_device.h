/*
 * Copyright (c) 2026
 *
 * usbip2_filter exposes a control device that the broker (and the persistent
 * reattach path in usbip2_ude) uses to register per-PDO owner SIDs.
 *
 * The control device is restricted to LocalSystem only; all configured access
 * happens through usbip-broker, which runs as LocalSystem.
 */
#pragma once

#include <wdm.h>

namespace usbip::filter
{

/*
 * Create the control device and register its symbolic link.
 * Idempotent. Call from DriverEntry. Stores the resulting DEVICE_OBJECT* in
 * the global so that dispatch routines can route on it.
 */
NTSTATUS control_device_create(_In_ DRIVER_OBJECT *drv);

/*
 * Tear down. Call from DriverUnload.
 */
void control_device_destroy();

/*
 * Returns true if devobj is the control device (and IRPs targeted at it should
 * be handled by control_device_*_irp instead of forwarded down a filter chain).
 */
bool is_control_device(_In_ const DEVICE_OBJECT *devobj);

NTSTATUS control_device_create_irp(_In_ DEVICE_OBJECT *devobj, _Inout_ IRP *irp);
NTSTATUS control_device_close_irp (_In_ DEVICE_OBJECT *devobj, _Inout_ IRP *irp);
NTSTATUS control_device_ioctl_irp (_In_ DEVICE_OBJECT *devobj, _Inout_ IRP *irp);

} // namespace usbip::filter
