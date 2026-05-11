/*
 * Probe exportable-device list from a USB/IP server to detect HID (class 0x03)
 * before issuing PLUGIN_HARDWARE — Phase 9 of per-user isolation.
 */
#pragma once

#include <string_view>

namespace usbip::broker
{

struct UsbClassProbeResult
{
        bool enum_ok{};       // usbip OP_REQ_DEVLIST succeeded
        bool busid_found{};   // matching busid appeared in list
        bool has_hid{};       // device or any interface uses USB HID class (0x03)
};

UsbClassProbeResult probe_usb_classes_for_busid(std::string_view hostname,
                                                  std::string_view service,
                                                  std::string_view busid);

} // namespace usbip::broker
