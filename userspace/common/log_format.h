/*
 * Copyright (c) 2026
 *
 * Shared spdlog line prefix: [user] [timestamp] [level] message
 */
#pragma once

#include <string>

namespace usbip
{

/** Current process user name (GetUserNameW), UTF-8. */
std::string log_format_username_utf8();

/**
 * spdlog pattern string:
 *   [username] [%Y-%m-%d %H:%M:%S.%e] [%l] %v
 * or with ANSI colors on the level token only (stderr):
 *   [username] [%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v
 */
std::string log_format_spdlog_pattern(bool color_level);

} // namespace usbip
