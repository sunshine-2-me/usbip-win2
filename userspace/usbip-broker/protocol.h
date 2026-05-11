/*
 * Copyright (c) 2026
 *
 * usbip-broker wire protocol.
 *
 * Each request and response is a single JSON object delivered as one message
 * over a PIPE_TYPE_MESSAGE named pipe. Strings are UTF-8.
 *
 * Request schema:
 *   { "cmd": "attach"|"detach"|"list"|"ping",
 *     "host": "...", "service": "...", "busid": "...",
 *     "port": 3,
 *     "once": true }
 *
 * Response schema:
 *   { "ok": true,  "port": 3, "devices": [ ... ] }
 *   { "ok": false, "error": "access_denied"|"not_found"|... , "detail": "..." }
 */
#pragma once

#include <cstddef>

namespace usbip::broker
{

inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\usbip-broker";

// Maximum pipe message size. Generous for the worst-case "list" response.
inline constexpr std::size_t kMaxMessageBytes = 64 * 1024;

// Pipe SDDL: Authenticated Users may read/write/connect; BUILTIN\Administrators
// and LocalSystem have full control. Allows non-admin RDP users to talk to the
// broker, but does not let them change DACL.
inline constexpr wchar_t kPipeSddl[] =
        L"D:(A;;0x12019b;;;AU)(A;;FA;;;BA)(A;;FA;;;SY)";

} // namespace usbip::broker
