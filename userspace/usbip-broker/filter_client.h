/*
 * Copyright (c) 2026
 *
 * Thin C++ wrapper around the usbip2_filter control device. The broker uses
 * this immediately after a successful PLUGIN_HARDWARE to register the new
 * USB PDO's owner with the filter so its IRP_MJ_CREATE gate becomes active.
 */
#pragma once

#include <span>
#include <string>
#include <vector>

#include <windows.h>

namespace usbip::broker
{

class FilterClient
{
public:
        // Open the filter's control symbolic link. Returns INVALID_HANDLE_VALUE
        // on failure; call GetLastError().
        static HANDLE open();

        // SET_OWNER with the given device instance id and self-relative SID.
        static bool set_owner(HANDLE h,
                              std::wstring_view instance_id,
                              std::span<const uint8_t> sid_bytes,
                              DWORD session_id);

        // CLEAR_OWNER for an instance id.
        static bool clear_owner(HANDLE h, std::wstring_view instance_id);

        // Convenience: open + set + close. Best-effort, swallows errors and
        // logs them. Used after a successful attach so the broker doesn't
        // have to keep the handle around.
        static void register_owner(std::wstring_view instance_id,
                                   std::span<const uint8_t> sid_bytes,
                                   DWORD session_id);

        /** Open + CLEAR_OWNER + close (best-effort). */
        static void unregister_owner(std::wstring_view instance_id);
};

} // namespace usbip::broker
