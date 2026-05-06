#include "bus.h"
#include "trace.h"
#include "bus.tmh"

#include "vhci.h"

#include <ntstrsafe.h>
#include <devpkey.h>

EXTERN_C NTSYSAPI NTSTATUS NTAPI IoGetRequestorSessionId(_In_ PIRP Irp, _Out_ PULONG pSessionId);

namespace
{

using namespace usbip;

constexpr ULONG invalid_session_id = ULONG(-1);

_Function_class_(EVT_WDF_CHILD_LIST_CREATE_DEVICE)
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS evt_child_list_create_device(
        _In_ WDFCHILDLIST,
        _In_ PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER identification,
        _Inout_ PWDFDEVICE_INIT child_init)
{
        PAGED_CODE();
        auto id = CONTAINING_RECORD(identification, bus::session_hc_id, header);

        DECLARE_CONST_UNICODE_STRING(device_id, L"USBIP\\VirtualHostController");
        auto st = WdfPdoInitAssignDeviceID(child_init, &device_id);
        if (NT_ERROR(st)) {
                return st;
        }

        DECLARE_UNICODE_STRING_SIZE(instance_id, 32);
        st = RtlUnicodeStringPrintf(&instance_id, L"Session_%08lx", id->session_id);
        if (NT_ERROR(st)) {
                return st;
        }

        st = WdfPdoInitAssignInstanceID(child_init, &instance_id);
        if (NT_ERROR(st)) {
                return st;
        }

        st = WdfPdoInitAddHardwareID(child_init, &device_id);
        if (NT_ERROR(st)) {
                return st;
        }

        WDFDEVICE pdo{};
        st = WdfDeviceCreate(&child_init, WDF_NO_OBJECT_ATTRIBUTES, &pdo);
        if (NT_ERROR(st)) {
                return st;
        }

        WDF_DEVICE_PROPERTY_DATA prop;
        WDF_DEVICE_PROPERTY_DATA_INIT(&prop, &DEVPKEY_Device_SessionId);

        st = WdfDeviceAssignProperty(pdo,
                                     &prop,
                                     DEVPROP_TYPE_UINT32,
                                     sizeof(id->session_id),
                                     &id->session_id);
        return st;
}

_Function_class_(EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void evt_io_device_control(
        _In_ WDFQUEUE Queue,
        _In_ WDFREQUEST Request,
        _In_ size_t,
        _In_ size_t,
        _In_ ULONG IoControlCode)
{
        NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST;

        if (IoControlCode == vhci::ioctl::SPAWN_SESSION_HC) {
                ULONG session_id = invalid_session_id;
                auto irp = WdfRequestWdmGetIrp(Request);
                st = IoGetRequestorSessionId(irp, &session_id);

                if (NT_SUCCESS(st) && session_id != invalid_session_id) {
                        bus::session_hc_id id{};
                        WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER_INIT(&id.header, sizeof(id));
                        id.session_id = session_id;

                        auto child_list = WdfFdoGetDefaultChildList(WdfIoQueueGetDevice(Queue));
                        st = WdfChildListAddOrUpdateChildDescriptionAsPresent(child_list, &id.header, nullptr);

                        if (NT_SUCCESS(st)) {
                                const auto created = (st != STATUS_OBJECT_NAME_EXISTS);
                                vhci::ioctl::spawn_session_hc *out{};
                                size_t out_len{};
                                auto out_st = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), reinterpret_cast<PVOID*>(&out), &out_len);
                                if (NT_SUCCESS(out_st)) {
                                        RtlZeroMemory(out, out_len);
                                        out->size = sizeof(*out);
                                        out->session_id = session_id;
                                        out->created = created ? TRUE : FALSE;
                                        WdfRequestSetInformation(Request, sizeof(*out));
                                } else {
                                        WdfRequestSetInformation(Request, 0);
                                }
                                st = STATUS_SUCCESS;
                        }
                } else if (NT_SUCCESS(st)) {
                        st = STATUS_INVALID_DEVICE_REQUEST;
                }
        }

        WdfRequestComplete(Request, st);
}

} // namespace


_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS usbip::bus::device_add(_Inout_ WDFDEVICE_INIT *init)
{
        PAGED_CODE();

        WdfDeviceInitSetDeviceType(init, FILE_DEVICE_BUS_EXTENDER);
        WdfDeviceInitSetCharacteristics(init, FILE_DEVICE_SECURE_OPEN, TRUE);

        if (auto err = WdfDeviceInitAssignSDDLString(init, &SDDL_DEVOBJ_SYS_ALL_ADM_ALL)) {
                return err;
        }

        WDF_CHILD_LIST_CONFIG child_cfg;
        WDF_CHILD_LIST_CONFIG_INIT(&child_cfg, sizeof(session_hc_id), evt_child_list_create_device);
        WdfFdoInitSetDefaultChildListConfig(init, &child_cfg, WDF_NO_OBJECT_ATTRIBUTES);

        WDF_OBJECT_ATTRIBUTES attr;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, bus_ctx);

        WDFDEVICE bus{};
        auto st = WdfDeviceCreate(&init, &attr, &bus);
        if (NT_ERROR(st)) {
                return st;
        }

        auto ctx = get_bus_ctx(bus);
        WDF_OBJECT_ATTRIBUTES lock_attr;
        WDF_OBJECT_ATTRIBUTES_INIT(&lock_attr);
        lock_attr.ParentObject = bus;

        st = WdfWaitLockCreate(&lock_attr, &ctx->lock);
        if (NT_ERROR(st)) {
                return st;
        }

        WDF_IO_QUEUE_CONFIG queue_cfg;
        WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue_cfg, WdfIoQueueDispatchSequential);
        queue_cfg.PowerManaged = WdfFalse;
        queue_cfg.EvtIoDeviceControl = evt_io_device_control;

        st = WdfIoQueueCreate(bus, &queue_cfg, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
        if (NT_ERROR(st)) {
                return st;
        }

        st = WdfDeviceCreateDeviceInterface(bus, &vhci::GUID_DEVINTERFACE_USBIP_BUS, nullptr);
        if (NT_ERROR(st)) {
                return st;
        }

        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void usbip::bus::mark_session_controller_in_use(_In_ WDFDEVICE vhci, _In_ bool in_use)
{
        if (in_use) {
                return;
        }

        auto pdo = WdfDeviceWdmGetPhysicalDevice(vhci);
        if (!pdo) {
                return;
        }

        ULONG session_id{};
        DEVPROPTYPE type{};
        ULONG req_size{};
        auto st = IoGetDevicePropertyData(
                pdo,
                &DEVPKEY_Device_SessionId,
                LOCALE_NEUTRAL,
                0,
                sizeof(session_id),
                &session_id,
                &req_size,
                &type);
        if (NT_ERROR(st) || type != DEVPROP_TYPE_UINT32) {
                return;
        }

        auto pdo_wdf = WdfWdmDeviceGetWdfDeviceHandle(pdo);
        if (!pdo_wdf) {
                return;
        }

        auto bus = WdfPdoGetParent(pdo_wdf);
        if (!bus) {
                return;
        }

        session_hc_id id{};
        WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER_INIT(&id.header, sizeof(id));
        id.session_id = session_id;

        auto child_list = WdfFdoGetDefaultChildList(bus);
        WdfChildListUpdateChildDescriptionAsMissing(child_list, &id.header);
}
