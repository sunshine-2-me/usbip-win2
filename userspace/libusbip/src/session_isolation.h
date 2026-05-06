#pragma once

#include <windows.h>

namespace usbip::session_isolation
{

bool isolate_attached_usb_device_to_current_session(
        _In_ HANDLE vhci,
        _In_ int port,
        _In_ DWORD timeout_ms = 5000) noexcept;

} // namespace usbip::session_isolation
