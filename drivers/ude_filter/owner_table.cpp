/*
 * Copyright (c) 2026
 */
#include <ntifs.h>

#include "owner_table.h"
#include "driver.h"
#include "trace.h"
#include "owner_table.tmh"

namespace usbip::filter
{

namespace
{

struct Entry
{
        LIST_ENTRY link;
        PWSTR  instance_id;     // owned, NUL-terminated
        ULONG  instance_id_chars; // including NUL
        PSID   sid;             // owned
        ULONG  sid_size;
        ULONG  session_id;
};

KSPIN_LOCK g_lock;
LIST_ENTRY g_list;
bool       g_initialised = false;

void free_entry(Entry *e)
{
        if (!e) return;
        if (e->instance_id) ExFreePoolWithTag(e->instance_id, pooltag);
        if (e->sid)         ExFreePoolWithTag(e->sid, pooltag);
        ExFreePoolWithTag(e, pooltag);
}

Entry* find_locked(_In_ PCWSTR instance_id)
{
        for (auto p = g_list.Flink; p != &g_list; p = p->Flink) {
                auto e = CONTAINING_RECORD(p, Entry, link);
                if (e->instance_id && _wcsicmp(e->instance_id, instance_id) == 0) {
                        return e;
                }
        }
        return nullptr;
}

/*
 * Read the device-instance-id of a PDO into a caller-owned buffer.
 * The result is NUL-terminated.
 */
NTSTATUS get_pdo_instance_id(_In_ DEVICE_OBJECT *pdo, _Out_ PWSTR *out_str, _Out_opt_ ULONG *out_chars)
{
        *out_str = nullptr;
        if (out_chars) *out_chars = 0;

        ULONG req_bytes = 0;
        NTSTATUS st = IoGetDeviceProperty(pdo, DevicePropertyPhysicalDeviceObjectName,
                                          0, nullptr, &req_bytes);
        if (st != STATUS_BUFFER_TOO_SMALL || !req_bytes) {
                return NT_SUCCESS(st) ? STATUS_UNSUCCESSFUL : st;
        }

        // Use BusQueryInstanceID via PnP IRP for a stable instance id.
        IO_STATUS_BLOCK iosb{};
        KEVENT event;
        KeInitializeEvent(&event, NotificationEvent, FALSE);

        auto top = IoGetAttachedDeviceReference(pdo);
        if (!top) {
                return STATUS_NO_SUCH_DEVICE;
        }

        auto irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, top, nullptr, 0, nullptr, &event, &iosb);
        if (!irp) {
                ObDereferenceObject(top);
                return STATUS_INSUFFICIENT_RESOURCES;
        }
        irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        auto stack = IoGetNextIrpStackLocation(irp);
        stack->MajorFunction = IRP_MJ_PNP;
        stack->MinorFunction = IRP_MN_QUERY_ID;
        stack->Parameters.QueryId.IdType = BusQueryInstanceID;

        st = IoCallDriver(top, irp);
        if (st == STATUS_PENDING) {
                KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, nullptr);
                st = iosb.Status;
        }
        ObDereferenceObject(top);

        if (!NT_SUCCESS(st)) {
                return st;
        }

        auto id_str = reinterpret_cast<PWSTR>(iosb.Information);
        if (!id_str || !*id_str) {
                if (id_str) ExFreePool(id_str);
                return STATUS_NOT_FOUND;
        }

        // Copy with our pool tag, free the I/O-Manager-provided string.
        auto chars = static_cast<ULONG>(wcslen(id_str)) + 1;
        auto bytes = chars * sizeof(WCHAR);
        auto copy = static_cast<PWSTR>(ExAllocatePoolWithTag(PagedPool, bytes, pooltag));
        if (!copy) {
                ExFreePool(id_str);
                return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlCopyMemory(copy, id_str, bytes);
        ExFreePool(id_str);

        *out_str = copy;
        if (out_chars) *out_chars = chars;
        return STATUS_SUCCESS;
}

} // namespace

NTSTATUS owner_table_init()
{
        if (g_initialised) return STATUS_SUCCESS;
        KeInitializeSpinLock(&g_lock);
        InitializeListHead(&g_list);
        g_initialised = true;
        return STATUS_SUCCESS;
}

void owner_table_cleanup()
{
        if (!g_initialised) return;

        KIRQL old_irql;
        KeAcquireSpinLock(&g_lock, &old_irql);
        while (!IsListEmpty(&g_list)) {
                auto p = RemoveHeadList(&g_list);
                auto e = CONTAINING_RECORD(p, Entry, link);
                free_entry(e);
        }
        KeReleaseSpinLock(&g_lock, old_irql);

        g_initialised = false;
}

NTSTATUS owner_table_set(_In_ PCWSTR instance_id,
                         _In_ PSID sid,
                         _In_ ULONG sid_size,
                         _In_ ULONG session_id)
{
        if (!instance_id || !sid || !sid_size) {
                return STATUS_INVALID_PARAMETER;
        }
        if (!RtlValidSid(sid)) {
                return STATUS_INVALID_SID;
        }
        if (RtlLengthSid(sid) != sid_size) {
                return STATUS_INVALID_SID;
        }

        auto id_chars = static_cast<ULONG>(wcslen(instance_id)) + 1;
        auto id_bytes = id_chars * sizeof(WCHAR);

        auto e = static_cast<Entry*>(ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(Entry), pooltag));
        if (!e) return STATUS_INSUFFICIENT_RESOURCES;

        e->instance_id_chars = id_chars;
        e->session_id = session_id;
        e->sid_size = sid_size;

        e->instance_id = static_cast<PWSTR>(ExAllocatePoolWithTag(PagedPool, id_bytes, pooltag));
        e->sid         = static_cast<PSID>(ExAllocatePoolWithTag(PagedPool, sid_size, pooltag));
        if (!e->instance_id || !e->sid) {
                free_entry(e);
                return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlCopyMemory(e->instance_id, instance_id, id_bytes);
        if (auto st = RtlCopySid(sid_size, e->sid, sid); !NT_SUCCESS(st)) {
                free_entry(e);
                return st;
        }

        KIRQL old_irql;
        KeAcquireSpinLock(&g_lock, &old_irql);
        if (auto existing = find_locked(instance_id)) {
                RemoveEntryList(&existing->link);
                KeReleaseSpinLock(&g_lock, old_irql);
                free_entry(existing);
                KeAcquireSpinLock(&g_lock, &old_irql);
        }
        InsertTailList(&g_list, &e->link);
        KeReleaseSpinLock(&g_lock, old_irql);

        Trace(TRACE_LEVEL_INFORMATION, "owner_table set '%ws' size=%lu", instance_id, sid_size);
        return STATUS_SUCCESS;
}

NTSTATUS owner_table_clear(_In_ PCWSTR instance_id)
{
        if (!instance_id) return STATUS_INVALID_PARAMETER;

        Entry *removed = nullptr;

        KIRQL old_irql;
        KeAcquireSpinLock(&g_lock, &old_irql);
        if (auto e = find_locked(instance_id)) {
                RemoveEntryList(&e->link);
                removed = e;
        }
        KeReleaseSpinLock(&g_lock, old_irql);

        if (removed) {
                Trace(TRACE_LEVEL_INFORMATION, "owner_table cleared '%ws'", instance_id);
                free_entry(removed);
        }
        return STATUS_SUCCESS;
}

void owner_table_clear_all()
{
        if (!g_initialised) return;

        LIST_ENTRY tmp;
        InitializeListHead(&tmp);

        KIRQL old_irql;
        KeAcquireSpinLock(&g_lock, &old_irql);
        while (!IsListEmpty(&g_list)) {
                auto p = RemoveHeadList(&g_list);
                InsertTailList(&tmp, p);
        }
        KeReleaseSpinLock(&g_lock, old_irql);

        while (!IsListEmpty(&tmp)) {
                auto p = RemoveHeadList(&tmp);
                free_entry(CONTAINING_RECORD(p, Entry, link));
        }
}

NTSTATUS owner_table_lookup_pdo(_In_ DEVICE_OBJECT *pdo,
                                _Out_ PSID *out_sid_copy,
                                _Out_ ULONG *out_sid_size,
                                _Out_opt_ ULONG *out_session_id)
{
        *out_sid_copy = nullptr;
        *out_sid_size = 0;
        if (out_session_id) *out_session_id = 0;

        if (!pdo) return STATUS_INVALID_PARAMETER;

        PWSTR instance_id = nullptr;
        if (auto st = get_pdo_instance_id(pdo, &instance_id, nullptr); !NT_SUCCESS(st)) {
                return st;
        }

        PSID sid_buf  = nullptr;
        ULONG sid_sz  = 0;
        ULONG sess    = 0;

        KIRQL old_irql;
        KeAcquireSpinLock(&g_lock, &old_irql);
        if (auto e = find_locked(instance_id)) {
                sid_sz = e->sid_size;
                sid_buf = ExAllocatePoolWithTag(NonPagedPoolNx, sid_sz, pooltag);
                if (sid_buf) {
                        RtlCopyMemory(sid_buf, e->sid, sid_sz);
                        sess = e->session_id;
                }
        }
        KeReleaseSpinLock(&g_lock, old_irql);

        ExFreePoolWithTag(instance_id, pooltag);

        if (!sid_sz) {
                return STATUS_NOT_FOUND;
        }
        if (!sid_buf) {
                return STATUS_INSUFFICIENT_RESOURCES;
        }

        *out_sid_copy  = sid_buf;
        *out_sid_size  = sid_sz;
        if (out_session_id) *out_session_id = sess;
        return STATUS_SUCCESS;
}

} // namespace usbip::filter
