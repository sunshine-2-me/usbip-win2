/*
 * Copyright (c) 2026
 *
 * Default host log directory C:\temp\usbip\logs and dated log filenames.
 */
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace usbip
{

/** e.g. C:\temp\usbip\logs (created if missing). Empty on failure. */
std::filesystem::path default_usbip_log_directory();

/** Local calendar date as yyyymmdd for log filename suffix. */
std::string default_usbip_log_date_yyyymmdd();

/** Full path: {default_usbip_log_directory()}\{basename}-yyyymmdd.log */
std::filesystem::path default_usbip_log_file(std::string_view basename);

} // namespace usbip
