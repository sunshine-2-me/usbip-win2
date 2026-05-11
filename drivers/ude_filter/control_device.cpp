/*
 * Copyright (c) 2026
 */
#include <ntifs.h>
#include <wdmsec.h> // IoCreateDeviceSecure, SDDL_DEVOBJ_*

#include "control_device.h"
#include "owner_table.h"
#include "driver.h"
#include "trace.h"
#include "control_device.tmh"

#include <initguid.h>
#include <usbip/filter.h>

namespace usbip::filter
{

namespace
{

DEVICE_OBJECT *g_control_device = nullptr;
UNICODE_STRING g_control_link{};
bool g_link_created = false;

NTSTATUS handle_set_owner(_In_ const ioctl::set_owner *r, _In_ ULONG length)
{
        if (length != sizeof(*r) || r->size != sizeof(*r)) {
                return STATUS_INVALID_PARAMETER;
        }
        if (!r->instance_id_chars || r->instance_id_chars > ioctl::INSTANCE_ID_MAX_CHARS) {
                return STATUS_INVALID_PARAMETER;
        }
        if (r->instance_id[r->instance_id_chars - 1] != L'\0') {
                return STATUS_INVALID_PARAMETER;
        }
        if (!r->sid_size || r->sid_size > ioctl::OWNER_SID_MAX_SIZE) {
                return STATUS_INVALID_PARAMETER;
        }
        return owner_table_set(r->instance_id,
                               const_cast<UCHAR*>(r->sid),
                               r->sid_size,
                               r->session_id);
}

NTSTATUS handle_clear_owner(_In_ const ioctl::clear_owner *r, _In_ ULONG length)
{
        if (length != sizeof(*r) || r->size != sizeof(*r)) {
                return STATUS_INVALID_PARAMETER;
        }
        if (!r->instance_id_chars || r->instance_id_chars > ioctl::INSTANCE_ID_MAX_CHARS) {
                return STATUS_INVALID_PARAMETER;
        }
        if (r->instance_id[r->instance_id_chars - 1] != L'\0') {
                return STATUS_INVALID_PARAMETER;
        }
        return owner_table_clear(r->instance_id);
}

NTSTATUS handle_clear_all(_In_ const ioctl::clear_all_owners *r, _In_ ULONG length)
{
        if (length != sizeof(*r) || r->size != sizeof(*r)) {
                return STATUS_INVALID_PARAMETER;
        }
        owner_table_clear_all();
        return STATUS_SUCCESS;
}

NTSTATUS complete(IRP *irp, NTSTATUS st, ULONG_PTR info = 0)
{
        irp->IoStatus.Status = st;
        irp->IoStatus.Information = info;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return st;
}

} // namespace

NTSTATUS control_device_create(_In_ DRIVER_OBJECT *drv)
{
        if (g_control_device) return STATUS_SUCCESS;

        // SDDL: only LocalSystem and Built-in Administrators may open.
        DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");

        UNICODE_STRING name;
        RtlInitUnicodeString(&name, kControlDeviceName);

        DEVICE_OBJECT *dev = nullptr;
        auto st = IoCreateDeviceSecure(drv, 0, &name,
                                       FILE_DEVICE_UNKNOWN,
                                       FILE_DEVICE_SECURE_OPEN,
                                       FALSE,
                                       &sddl,
                                       const_cast<LPCGUID>(&GUID_DEVINTERFACE_USBIP_FILTER),
                                       &dev);
        if (!NT_SUCCESS(st)) {
                Trace(TRACE_LEVEL_ERROR, "IoCreateDeviceSecure %!STATUS!", st);
                return st;
        }

        UNICODE_STRING link;
        RtlInitUnicodeString(&link, kSymbolicLinkName);
        st = IoCreateSymbolicLink(&link, &name);
        if (!NT_SUCCESS(st)) {
                Trace(TRACE_LEVEL_ERROR, "IoCreateSymbolicLink %!STATUS!", st);
                IoDeleteDevice(dev);
                return st;
        }
        g_control_link = link;
        g_link_created = true;

        dev->Flags |= DO_BUFFERED_IO;
        dev->Flags &= ~DO_DEVICE_INITIALIZING;

        g_control_device = dev;
        Trace(TRACE_LEVEL_INFORMATION, "filter control device created");
        return STATUS_SUCCESS;
}

void control_device_destroy()
{
        if (g_link_created) {
                IoDeleteSymbolicLink(&g_control_link);
                g_link_created = false;
        }
        if (g_control_device) {
                IoDeleteDevice(g_control_device);
                g_control_device = nullptr;
        }
}

bool is_control_device(_In_ const DEVICE_OBJECT *devobj)
{
        return g_control_device && devobj == g_control_device;
}

NTSTATUS control_device_create_irp(_In_ DEVICE_OBJECT*, _Inout_ IRP *irp)
{
        return complete(irp, STATUS_SUCCESS);
}

NTSTATUS control_device_close_irp(_In_ DEVICE_OBJECT*, _Inout_ IRP *irp)
{
        return complete(irp, STATUS_SUCCESS);
}

NTSTATUS control_device_ioctl_irp(_In_ DEVICE_OBJECT*, _Inout_ IRP *irp)
{
        auto stack = IoGetCurrentIrpStackLocation(irp);
        auto code  = stack->Parameters.DeviceIoControl.IoControlCode;
        auto inlen = stack->Parameters.DeviceIoControl.InputBufferLength;
        auto buf   = irp->AssociatedIrp.SystemBuffer;

        NTSTATUS st;
        switch (code) {
        case ioctl::SET_OWNER:
                st = handle_set_owner(static_cast<ioctl::set_owner*>(buf), inlen);
                break;
        case ioctl::CLEAR_OWNER:
                st = handle_clear_owner(static_cast<ioctl::clear_owner*>(buf), inlen);
                break;
        case ioctl::CLEAR_ALL_OWNERS:
                st = handle_clear_all(static_cast<ioctl::clear_all_owners*>(buf), inlen);
                break;
        default:
                st = STATUS_INVALID_DEVICE_REQUEST;
                break;
        }
        return complete(irp, st);
}

} // namespace usbip::filter
