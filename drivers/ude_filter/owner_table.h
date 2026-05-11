/*
 * Copyright (c) 2026
 *
 * Per-PDO owner SID table for usbip2_filter.
 *
 * The broker and the persistent-reattach path in usbip2_ude register entries
 * by device-instance-id. CreateFile dispatch then looks up the entry for the
 * arriving PDO and refuses callers whose SID does not match.
 */
#pragma once

#include <wdm.h>

namespace usbip::filter
{

/*
 * Initialise the global owner table. Call from DriverEntry. Idempotent.
 * Returns STATUS_SUCCESS or an error.
 */
NTSTATUS owner_table_init();

/*
 * Tear down the global owner table. Call from DriverUnload. Idempotent.
 */
void owner_table_cleanup();

/*
 * Add or replace an entry for (instance_id, sid). instance_id is a
 * NUL-terminated UTF-16 string. sid is a self-relative SID buffer; it is
 * deep-copied. session_id is informational only.
 *
 * Returns STATUS_INVALID_PARAMETER, STATUS_INSUFFICIENT_RESOURCES, or
 * STATUS_SUCCESS.
 */
NTSTATUS owner_table_set(_In_ PCWSTR instance_id,
                         _In_ PSID sid,
                         _In_ ULONG sid_size,
                         _In_ ULONG session_id);

/*
 * Remove the entry for instance_id, if any. STATUS_SUCCESS if removed or absent.
 */
NTSTATUS owner_table_clear(_In_ PCWSTR instance_id);

/*
 * Remove all entries.
 */
void owner_table_clear_all();

/*
 * Lookup the owner SID for a PDO. Reads the PDO's device-instance-id and
 * matches against the table.
 *
 * On success, *out_sid is a pointer into the table-owned buffer (valid only
 * while the caller holds no spinlock and the table entry is not concurrently
 * removed). For our use, the filter copies the SID into filter_ext::device.
 *
 * Returns:
 *   STATUS_SUCCESS         - an entry was found, *out_sid_size is set, *out_sid_copy is allocated and copied (caller frees with ExFreePoolWithTag).
 *   STATUS_NOT_FOUND       - no policy registered for this PDO (allow CreateFile).
 *   other errors           - lookup failed.
 *
 * out_session_id is informational and may be 0.
 */
NTSTATUS owner_table_lookup_pdo(_In_ DEVICE_OBJECT *pdo,
                                _Out_ PSID *out_sid_copy,
                                _Out_ ULONG *out_sid_size,
                                _Out_opt_ ULONG *out_session_id);

} // namespace usbip::filter
