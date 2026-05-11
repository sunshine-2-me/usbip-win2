/*
 * Copyright (c) 2026
 *
 * Best-effort post-attach enumeration under the VHCI devnode to find the new
 * USB PDO instance id and register it with usbip2_filter (SET_OWNER) without
 * waiting for a volume interface (covers HID and similar).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <windows.h>

namespace usbip::broker
{

/** Several short retries while PnP creates the child stack. */
void try_register_filter_via_vhci_enumeration(int port,
                                              std::string_view busid,
                                              std::span<const std::uint8_t> sid_bytes,
                                              DWORD session_id);

} // namespace usbip::broker
