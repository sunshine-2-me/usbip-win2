/*
 * Copyright (c) 2021-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#pragma once

#include <string_view>

namespace libusbip
{

/**
 * Initialize spdlog in the host process (usbip.exe / wusbip.exe). Must be linked into the
 * executable, not libusbip.dll, so exe and DLL share one spdlog registry and log file.
 *
 * Log file under C:\usbip\logs: {log_file_stem}_{yyyymmdd}.log (local date; stem from host exe if empty).
 * Multiple runs the same day append to the same file.
 * File line pattern: username %Y-%m-%d %H:%M:%S.%e %l %v
 * Logs the process command line (GetCommandLineW) at info as the first line after the logger is
 * installed, then flushes so it appears immediately; further messages follow (e.g. log path at debug).
 * Wires libusbip::set_debug_output to the same spdlog::debug as the rest of the process.
 *
 * @param log_file_stem optional stem for the log file; empty uses the host process image name
 * @param include_stderr_sink if true, mirror logs to colored stderr (console tools)
 */
void init_logging(std::wstring_view log_file_stem, bool include_stderr_sink);

} // namespace libusbip
