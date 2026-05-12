/*
 * Copyright (c) 2023-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#pragma once

namespace usbip
{

/** Install wxLogChain(wxLogSpdlog) so wxLog* also goes to spdlog (file sink). */
void install_wx_spdlog_chain();

} // namespace usbip
