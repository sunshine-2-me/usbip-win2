/*
 * Copyright (c) 2022-2025 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 * 
 * @see https://github.com/desowin/usbpcap/tree/master/USBPcapDriver
 */

#include <ntifs.h>

#include "driver.h"
#include "trace.h"
#include "driver.tmh"

#include "irp.h"
#include "pnp.h"
#include "int_dev_ctrl.h"
#include "control_device.h"
#include "owner_table.h"

#include <libdrv\remove_lock.h>

namespace
{

using namespace usbip;

_Function_class_(DRIVER_UNLOAD)
_IRQL_requires_(PASSIVE_LEVEL)
_IRQL_requires_same_
PAGED void driver_unload(_In_ DRIVER_OBJECT *drvobj)
{
	PAGED_CODE();
	Trace(TRACE_LEVEL_INFORMATION, "%04x", ptr04x(drvobj));

	usbip::filter::control_device_destroy();
	usbip::filter::owner_table_cleanup();

	WPP_CLEANUP(drvobj);
}

_Function_class_(DRIVER_DISPATCH)
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_same_
auto dispatch_lower(_In_ DEVICE_OBJECT *devobj, _Inout_ IRP *irp)
{
	if (filter::is_control_device(devobj)) {
		// Catch-all for control device IRPs we don't implement (e.g. PnP).
		return CompleteRequest(irp, STATUS_INVALID_DEVICE_REQUEST);
	}

	auto &f = *get_filter_ext(devobj);

	libdrv::RemoveLockGuard lck(f.remove_lock);
	if (auto err = lck.acquired()) {
		Trace(TRACE_LEVEL_ERROR, "Acquire remove lock %!STATUS!", err);
		return CompleteRequest(irp, err);
	}

	return ForwardIrp(f, irp);
}

/*
 * Per-user CreateFile gate. For the control device, succeed unconditionally
 * (only LocalSystem and Administrators can reach this device thanks to its
 * SDDL). For an emulated USB PDO that has a registered owner, refuse opens
 * from any caller whose primary/impersonation SID does not match.
 */
_Function_class_(DRIVER_DISPATCH)
_Dispatch_type_(IRP_MJ_CREATE)
_IRQL_requires_max_(PASSIVE_LEVEL)
_IRQL_requires_same_
NTSTATUS dispatch_create(_In_ DEVICE_OBJECT *devobj, _Inout_ IRP *irp)
{
	if (filter::is_control_device(devobj)) {
		return filter::control_device_create_irp(devobj, irp);
	}

	auto &f = *get_filter_ext(devobj);

	libdrv::RemoveLockGuard lck(f.remove_lock);
	if (auto err = lck.acquired()) {
		return CompleteRequest(irp, err);
	}

	if (f.is_hub) {
		return ForwardIrp(f, irp);
	}

	auto &dev = f.device;

	// Resolve owner lazily. Once resolved (or proven absent), cache the result.
	if (!dev.owner_resolved) {
		PSID sid = nullptr;
		ULONG sid_size = 0;
		ULONG session  = 0;
		auto st = filter::owner_table_lookup_pdo(f.pdo, &sid, &sid_size, &session);
		if (NT_SUCCESS(st)) {
			dev.owner_sid = sid;          // ownership transferred
			dev.owner_session = session;
		}
		dev.owner_resolved = true;
	}

	// No registered owner -> no policy, behave as a transparent filter.
	if (!dev.owner_sid) {
		return ForwardIrp(f, irp);
	}

	// Capture caller SID and compare.
	SECURITY_SUBJECT_CONTEXT subj;
	SeCaptureSubjectContext(&subj);

	bool allowed = false;
	if (auto token = SeQuerySubjectContextToken(&subj)) {
		PVOID info = nullptr;
		if (NT_SUCCESS(SeQueryInformationToken(token, TokenUser, &info))) {
			auto user = static_cast<TOKEN_USER*>(info);
			if (RtlValidSid(user->User.Sid) &&
			    RtlEqualSid(user->User.Sid, dev.owner_sid)) {
				allowed = true;
			}
			ExFreePool(info);
		}
	}
	SeReleaseSubjectContext(&subj);

	if (allowed) {
		return ForwardIrp(f, irp);
	}

	Trace(TRACE_LEVEL_WARNING, "filter create: ACCESS_DENIED for pdo %04x", ptr04x(f.pdo));
	return CompleteRequest(irp, STATUS_ACCESS_DENIED);
}

_Function_class_(DRIVER_DISPATCH)
_Dispatch_type_(IRP_MJ_CLOSE)
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_same_
NTSTATUS dispatch_close(_In_ DEVICE_OBJECT *devobj, _Inout_ IRP *irp)
{
	if (filter::is_control_device(devobj)) {
		return filter::control_device_close_irp(devobj, irp);
	}
	return dispatch_lower(devobj, irp);
}

_Function_class_(DRIVER_DISPATCH)
_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_same_
NTSTATUS dispatch_device_control(_In_ DEVICE_OBJECT *devobj, _Inout_ IRP *irp)
{
	if (filter::is_control_device(devobj)) {
		return filter::control_device_ioctl_irp(devobj, irp);
	}
	return dispatch_lower(devobj, irp);
}

} // namespace


/*
 * warning C28168: The function 'dispatch_lower' does not have a _Dispatch_type_ annotation 
 * matching dispatch table position 'IRP_MJ_CREATE'...
 */
_Function_class_(DRIVER_INITIALIZE)
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
CS_INIT EXTERN_C NTSTATUS DriverEntry(_In_ DRIVER_OBJECT *drvobj, _In_ UNICODE_STRING *RegistryPath)
{
	ExInitializeDriverRuntime(0); // @see ExAllocatePool2

	WPP_INIT_TRACING(drvobj, RegistryPath);
	Trace(TRACE_LEVEL_INFORMATION, "%04x, '%!USTR!'", ptr04x(drvobj), RegistryPath);

	drvobj->DriverUnload = driver_unload;
	drvobj->DriverExtension->AddDevice = add_device;

#pragma warning(push)
#pragma warning(disable:28168)
	for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i) {
		drvobj->MajorFunction[i] = dispatch_lower;
	}
#pragma warning(pop)

	drvobj->MajorFunction[IRP_MJ_PNP] = pnp;
	drvobj->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = int_dev_ctrl;

	// Per-user isolation: gate CreateFile and own a control device for the broker.
	drvobj->MajorFunction[IRP_MJ_CREATE]         = dispatch_create;
	drvobj->MajorFunction[IRP_MJ_CLOSE]          = dispatch_close;
	drvobj->MajorFunction[IRP_MJ_DEVICE_CONTROL] = dispatch_device_control;

	if (auto err = filter::owner_table_init(); !NT_SUCCESS(err)) {
		Trace(TRACE_LEVEL_ERROR, "owner_table_init %!STATUS!", err);
		return err;
	}

	if (auto err = filter::control_device_create(drvobj); !NT_SUCCESS(err)) {
		Trace(TRACE_LEVEL_ERROR, "control_device_create %!STATUS!", err);
		filter::owner_table_cleanup();
		return err;
	}

	return STATUS_SUCCESS;
}
