/*
 * Copyright (c) 2026
 *
 * Detect elevated process token (UAC elevation / "Run as administrator").
 */
#pragma once

namespace usbip
{

/** True if the current process token is elevated (TokenElevation). */
bool process_is_elevated_admin() noexcept;

} // namespace usbip
