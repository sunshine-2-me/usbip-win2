/*
 * Copyright (c) 2026
 *
 * libusbip backend that proxies attach/detach/list calls to usbip-broker
 * over a named pipe. Used as a fallback when CreateFile on the VHCI device
 * fails with ERROR_ACCESS_DENIED, which is the normal path for non-admin
 * RDP users after Phase 0 tightens the VHCI ACL.
 *
 * Detection at runtime is done with GetFileType: a normal driver handle is
 * FILE_TYPE_CHAR, the broker fallback is FILE_TYPE_PIPE.
 */
#pragma once

#include <vector>
#include <optional>

#include <windows.h>

#include "..\vhci.h"

namespace usbip::broker_client
{

inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\usbip-broker";

// Open a connection to the broker pipe, returning a HANDLE compatible with
// CloseHandle. Returns INVALID_HANDLE_VALUE on failure (preserves GetLastError).
HANDLE open_pipe();

// Attach over the broker. Returns hub port number, or 0 on failure.
int attach(_In_ HANDLE pipe, _In_ const device_location &location, _In_ unsigned long options);

// Detach over the broker. Returns true on success.
bool detach(_In_ HANDLE pipe, _In_ int port);

// List devices visible to the calling user.
std::optional<std::vector<imported_device>> get_imported_devices(_In_ HANDLE pipe);

} // namespace usbip::broker_client
