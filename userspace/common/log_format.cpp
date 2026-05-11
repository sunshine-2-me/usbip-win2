/*
 * Copyright (c) 2026
 */
#include "log_format.h"

#include <string>

#include <windows.h>

namespace usbip
{

namespace
{

void sanitize_username_for_spdlog_pattern(std::string &s)
{
        for (char &c : s) {
                if (c == '%' || c == '[' || c == ']') {
                        c = '_';
                }
        }
}

} // namespace

std::string log_format_username_utf8()
{
        // UNLEN+1 per GetUserNameW; avoid <lmcons.h> here (PASCAL clashes with win headers in some SDK orders).
        wchar_t wbuf[257]{};
        DWORD wlen = static_cast<DWORD>(std::size(wbuf));
        if (!GetUserNameW(wbuf, &wlen) || !wbuf[0]) {
                return "unknown";
        }
        const int need = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, nullptr, 0, nullptr, nullptr);
        if (need <= 1) {
                return "unknown";
        }
        std::string out(static_cast<std::size_t>(need - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out.data(), need, nullptr, nullptr);
        return out;
}

std::string log_format_spdlog_pattern(bool color_level)
{
        auto u = log_format_username_utf8();
        sanitize_username_for_spdlog_pattern(u);

        std::string p;
        p.reserve(u.size() + 64);
        p.push_back('[');
        p += u;
        p += "] [%Y-%m-%d %H:%M:%S.%e] [";
        if (color_level) {
                p += "^";
        }
        p += "%l";
        if (color_level) {
                p += "$";
        }
        p += "] %v";
        return p;
}

} // namespace usbip
