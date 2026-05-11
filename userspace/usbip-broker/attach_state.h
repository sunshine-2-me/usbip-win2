/*
 * Copyright (c) 2026
 *
 * Process-wide map maintained by the broker that records, for every successful
 * attach, the owning user's SID, RDP session id and the resolved hub port.
 *
 * Phase 8 uses this map in two ways:
 *   1. After PLUGIN_HARDWARE succeeds, register the new USB device with the
 *      filter (SET_OWNER) so usbip2_filter can reject foreign CreateFile.
 *   2. When a new volume PDO arrives, resolve it back to the owning session so
 *      we can mount its drive letter in that session and nowhere else.
 */
#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

namespace usbip::broker
{

struct AttachRecord
{
        int port = 0;
        std::string host;
        std::string service;
        std::string busid;
        std::wstring sid;       // wide string form of the SID, "S-1-5-21-..."
        DWORD session = 0;
        std::vector<uint8_t> sid_bytes; // self-relative SID buffer for IOCTLs

        // PDO device instance ID (cached after PnP enumeration finishes).
        // Empty until resolve_pdo_instance has been called.
        std::wstring pdo_instance_id;

        // Drive letter mounted in the owner's session by VolumeWatcher (e.g. L'Z').
        // 0 if not yet mounted or unknown. Used to run mountvol /D on detach/logoff.
        wchar_t session_drive_letter = 0;
};

class AttachState
{
public:
        // Insert/replace the record for a given hub port.
        void set(int port, AttachRecord rec);

        // Forget the record (e.g. on plugout).
        void erase(int port);

        // Look up by hub port (returns a copy to avoid lock contention).
        std::optional<AttachRecord> get(int port) const;

        // Find the record whose pdo_instance_id is a case-insensitive prefix
        // of @p instance_id. Used by the volume watcher to walk up from a
        // volume PDO to the owning USB device.
        std::optional<AttachRecord> find_by_instance_prefix(std::wstring_view instance_id) const;

        /**
         * Match a pending attach (empty pdo_instance_id) whose busid appears as
         * a case-insensitive substring of @p full_usb_instance_id (e.g. CM device
         * id for the USB PDO). Returns nullopt if none or ambiguous.
         */
        std::optional<AttachRecord> find_pending_for_usb_instance(
                std::wstring_view full_usb_instance_id) const;

        // Update the cached pdo_instance_id for a port (called from the volume
        // watcher / attach finalizer once PnP enumeration has produced the PDO).
        void set_pdo_instance(int port, std::wstring instance_id);

        // Record the drive letter assigned in the owner's session (VolumeWatcher).
        void set_session_drive_letter(int port, wchar_t letter);

        // Copy all attach records for a TS session (for unmount before erase).
        std::vector<std::pair<int, AttachRecord>> list_by_session(DWORD session_id) const;

        // Remove all records owned by a session (disconnect/logoff cleanup).
        std::size_t erase_by_session(DWORD session_id);

        static AttachState &instance();

private:
        mutable std::mutex m_;
        std::unordered_map<int, AttachRecord> by_port_;
};

} // namespace usbip::broker
