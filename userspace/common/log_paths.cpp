/*
 * Copyright (c) 2026
 */

#include "log_paths.h"

#include <cstdio>
#include <string_view>

#include <windows.h>

namespace usbip
{

namespace
{
constexpr std::wstring_view k_default_log_dir = L"C:\\temp\\usbip\\logs";
}

std::filesystem::path default_usbip_log_directory()
{
        std::filesystem::path p(k_default_log_dir);
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p;
}

std::string default_usbip_log_date_yyyymmdd()
{
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char buf[16]{};
        std::snprintf(buf, sizeof buf, "%04u%02u%02u",
                      static_cast<unsigned>(st.wYear),
                      static_cast<unsigned>(st.wMonth),
                      static_cast<unsigned>(st.wDay));
        return buf;
}

std::filesystem::path default_usbip_log_file(std::string_view basename)
{
        const auto dir = default_usbip_log_directory();
        if (dir.empty()) {
                return {};
        }
        std::string name;
        name.reserve(basename.size() + 1 + 8 + 4);
        name.append(basename);
        name.push_back('-');
        name += default_usbip_log_date_yyyymmdd();
        name.append(".log");
        return dir / name;
}

} // namespace usbip
