/*
 * Copyright (c) 2026
 */
#include "vhci_access.h"

#include <cstddef>
#include <vector>

#include <initguid.h>
#include <cfgmgr32.h>
#include <usbip/vhci.h>

#pragma comment(lib, "CfgMgr32.lib")

namespace usbip::broker::vhci_access
{

namespace
{

std::wstring device_path_impl()
{
        auto guid = const_cast<GUID *>(&::usbip::vhci::GUID_DEVINTERFACE_USB_HOST_CONTROLLER);
        std::wstring multi_sz;

        for (;;) {
                ULONG cch = 0;
                if (CM_Get_Device_Interface_List_Size(&cch, guid, nullptr,
                                                      CM_GET_DEVICE_INTERFACE_LIST_PRESENT)
                    != CR_SUCCESS) {
                        return {};
                }
                multi_sz.assign(cch, L'\0');
                auto cr = CM_Get_Device_Interface_List(guid, nullptr, multi_sz.data(), cch,
                                                       CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
                if (cr == CR_SUCCESS) {
                        break;
                }
                if (cr != CR_BUFFER_SMALL) {
                        return {};
                }
        }

        if (multi_sz.empty() || multi_sz[0] == L'\0') {
                return {};
        }
        return std::wstring(multi_sz.c_str());
}

} // namespace

std::wstring device_path()
{
        return device_path_impl();
}

HANDLE open()
{
        auto path = device_path_impl();
        if (path.empty()) {
                return INVALID_HANDLE_VALUE;
        }
        return CreateFileW(path.c_str(),
                             GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);
}

std::optional<std::pair<std::uint16_t, std::uint16_t>> imported_vid_pid_for_port(HANDLE vhci,
                                                                                int port)
{
        if (!vhci || vhci == INVALID_HANDLE_VALUE || port < 1) {
                return std::nullopt;
        }

        std::vector<char> buf;
        ::usbip::vhci::ioctl::get_imported_devices *r = nullptr;
        DWORD bytes = 0;
        BOOL ok = FALSE;
        DWORD ioctl_err = 0;

        for (auto cnt = 4u;; cnt <<= 1) {
                buf.resize(::usbip::vhci::ioctl::get_imported_devices_size(cnt));
                r = reinterpret_cast<::usbip::vhci::ioctl::get_imported_devices *>(buf.data());
                r->size = sizeof(*r);

                ok = DeviceIoControl(vhci,
                                     ::usbip::vhci::ioctl::GET_IMPORTED_DEVICES,
                                     r, sizeof(r->size),
                                     buf.data(), DWORD(buf.size()),
                                     &bytes, nullptr);
                if (ok) {
                        break;
                }
                ioctl_err = GetLastError();
                if (ioctl_err != ERROR_INSUFFICIENT_BUFFER || cnt > 1024u) {
                        return std::nullopt;
                }
        }

        constexpr auto offset = offsetof(::usbip::vhci::ioctl::get_imported_devices, devices);
        if (!ok || bytes < offset) {
                return std::nullopt;
        }

        const auto count = (bytes - offset) / sizeof(r->devices[0]);
        for (size_t i = 0; i < count; ++i) {
                if (r->devices[i].port == port) {
                        return std::pair<std::uint16_t, std::uint16_t>{r->devices[i].vendor,
                                                                      r->devices[i].product};
                }
        }

        return std::nullopt;
}

} // namespace usbip::broker::vhci_access
