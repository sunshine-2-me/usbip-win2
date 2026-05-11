/*
 * Copyright (c) 2026
 */
#include "filter_client.h"

#include <cstring>

#include <spdlog/spdlog.h>

#include <usbip/filter.h>

namespace usbip::broker
{

namespace
{

constexpr wchar_t kSymlink[] = L"\\\\.\\usbip2_filter_ctl";

} // namespace

HANDLE FilterClient::open()
{
        return CreateFileW(kSymlink,
                           GENERIC_READ | GENERIC_WRITE,
                           0,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
}

bool FilterClient::set_owner(HANDLE h,
                             std::wstring_view instance_id,
                             std::span<const uint8_t> sid_bytes,
                             DWORD session_id)
{
        using ::usbip::filter::ioctl::set_owner;
        using ::usbip::filter::ioctl::SET_OWNER;
        using ::usbip::filter::ioctl::INSTANCE_ID_MAX_CHARS;
        using ::usbip::filter::ioctl::OWNER_SID_MAX_SIZE;

        if (instance_id.size() + 1 > INSTANCE_ID_MAX_CHARS) {
                SetLastError(ERROR_BUFFER_OVERFLOW);
                return false;
        }
        if (sid_bytes.size() > OWNER_SID_MAX_SIZE) {
                SetLastError(ERROR_BUFFER_OVERFLOW);
                return false;
        }

        set_owner req{};
        req.size = sizeof(req);
        req.session_id = session_id;
        req.sid_size = static_cast<ULONG>(sid_bytes.size());
        std::memcpy(req.sid, sid_bytes.data(), sid_bytes.size());
        req.instance_id_chars = static_cast<ULONG>(instance_id.size() + 1);
        std::memcpy(req.instance_id, instance_id.data(),
                    instance_id.size() * sizeof(wchar_t));
        req.instance_id[instance_id.size()] = L'\0';

        DWORD bytes = 0;
        return DeviceIoControl(h, SET_OWNER, &req, sizeof(req),
                               nullptr, 0, &bytes, nullptr) != 0;
}

bool FilterClient::clear_owner(HANDLE h, std::wstring_view instance_id)
{
        using ::usbip::filter::ioctl::clear_owner;
        using ::usbip::filter::ioctl::CLEAR_OWNER;
        using ::usbip::filter::ioctl::INSTANCE_ID_MAX_CHARS;

        if (instance_id.size() + 1 > INSTANCE_ID_MAX_CHARS) {
                SetLastError(ERROR_BUFFER_OVERFLOW);
                return false;
        }

        clear_owner req{};
        req.size = sizeof(req);
        req.instance_id_chars = static_cast<ULONG>(instance_id.size() + 1);
        std::memcpy(req.instance_id, instance_id.data(),
                    instance_id.size() * sizeof(wchar_t));
        req.instance_id[instance_id.size()] = L'\0';

        DWORD bytes = 0;
        return DeviceIoControl(h, CLEAR_OWNER, &req, sizeof(req),
                               nullptr, 0, &bytes, nullptr) != 0;
}

void FilterClient::register_owner(std::wstring_view instance_id,
                                  std::span<const uint8_t> sid_bytes,
                                  DWORD session_id)
{
        spdlog::debug("filter register_owner: session={} instance_id_len={} sid_bytes={}",
                      session_id, instance_id.size(), sid_bytes.size());
        auto h = open();
        if (h == INVALID_HANDLE_VALUE) {
                spdlog::warn("filter_client::open failed err={} (filter not installed?)",
                             GetLastError());
                return;
        }
        if (!set_owner(h, instance_id, sid_bytes, session_id)) {
                spdlog::warn("filter SET_OWNER for '{}' failed err={}",
                             "[wide]", GetLastError());
        } else {
                spdlog::info("filter SET_OWNER ok session={} sid_bytes={}",
                             session_id, sid_bytes.size());
        }
        CloseHandle(h);
}

void FilterClient::unregister_owner(std::wstring_view instance_id)
{
        spdlog::debug("filter unregister_owner: instance_id_len={}", instance_id.size());
        auto h = open();
        if (h == INVALID_HANDLE_VALUE) {
                spdlog::warn("filter_client::open failed err={} (filter not installed?)",
                             GetLastError());
                return;
        }
        if (!clear_owner(h, instance_id)) {
                spdlog::warn("filter CLEAR_OWNER failed err={}", GetLastError());
        } else {
                spdlog::info("filter CLEAR_OWNER ok");
        }
        CloseHandle(h);
}

} // namespace usbip::broker
