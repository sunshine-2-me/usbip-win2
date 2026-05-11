/*
 * Copyright (c) 2026
 */
#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace usbip::broker::vhci_access
{

std::wstring device_path();
HANDLE open();

/*
 * Read GET_IMPORTED_DEVICES and return vendor/product for the given hub port.
 * std::nullopt = IOCTL failed or buffer error.
 * {0,0} is not returned as success; if the port is missing, returns nullopt.
 */
std::optional<std::pair<std::uint16_t, std::uint16_t>> imported_vid_pid_for_port(HANDLE vhci,
                                                                                  int port);

} // namespace usbip::broker::vhci_access
