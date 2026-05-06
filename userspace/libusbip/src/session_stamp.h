/*
 * Copyright (c) 2026
 *
 * Walk PnP devnodes under the VHCI and set DEVPKEY_Device_SessionId for an attached device.
 * @see tmp/session_based_isolation_solution.md
 */
#pragma once

#include <windows.h>

#include <string>

namespace usbip
{

bool stamp_session_property(
        _In_ HANDLE vhci_dev,
        _In_ int port,
        _In_ unsigned long session_id,
        _Inout_ std::wstring &error);

} // namespace usbip
