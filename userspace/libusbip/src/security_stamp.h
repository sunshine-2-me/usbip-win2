/*
 * Copyright (c) 2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#pragma once

#include "../vhci.h"

#include <string>

namespace usbip::vhci
{

bool stamp_imported_device_devnode_security(
        _In_ int port,
        _In_ const std::wstring &sddl,
        _In_ const imported_device &dev,
        _Inout_ std::wstring &error);

} // namespace usbip::vhci
