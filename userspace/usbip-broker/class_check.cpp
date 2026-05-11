/*
 * Copyright (c) 2026
 */
#include "class_check.h"

#include <libusbip/remote.h>
#include <libusbip/win_socket.h>

#include <spdlog/spdlog.h>

namespace usbip::broker
{

namespace
{

constexpr UINT8 USB_CLASS_HID = 3;

} // namespace

UsbClassProbeResult probe_usb_classes_for_busid(std::string_view hostname,
                                                std::string_view service,
                                                std::string_view busid)
{
        UsbClassProbeResult out{};

        if (hostname.empty() || service.empty() || busid.empty()) {
                return out;
        }

        usbip::InitWinSock2 wsa;
        if (!wsa) {
                spdlog::debug("class_probe: WSAStartup failed {}:{}/{}", hostname, service, busid);
                return out;
        }

        std::string host(hostname);
        std::string srv(service);

        auto sock = usbip::connect(host.c_str(), srv.c_str());
        if (!sock) {
                spdlog::debug("class_probe: connect failed {}:{}/{}", hostname, service, busid);
                return out;
        }

        const std::string busid_str(busid);

        auto on_dev = [&](int /*idx*/, const usbip::usb_device &d)
        {
                if (d.busid != busid_str) {
                        return;
                }
                out.busid_found = true;
                if (d.bDeviceClass == USB_CLASS_HID) {
                        out.has_hid = true;
                }
        };

        auto on_intf = [&](int /*dev_idx*/,
                           const usbip::usb_device &d,
                           int /*intf_idx*/,
                           const usbip::usb_interface &intf)
        {
                if (d.busid != busid_str) {
                        return;
                }
                out.busid_found = true;
                if (intf.bInterfaceClass == USB_CLASS_HID) {
                        out.has_hid = true;
                }
        };

        out.enum_ok = usbip::enum_exportable_devices(sock.get(), on_dev, on_intf);
        spdlog::debug(
                "class_probe: {}:{}/{} enum_ok={} busid_found={} has_hid={}",
                hostname, service, busid, out.enum_ok, out.busid_found, out.has_hid);
        return out;
}

} // namespace usbip::broker
