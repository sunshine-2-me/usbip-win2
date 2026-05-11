/*
 * Copyright (c) 2026
 */

#include "process_elevation.h"

#include <Windows.h>

namespace usbip
{

bool process_is_elevated_admin() noexcept
{
        HANDLE tok = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
                return false;
        }
        TOKEN_ELEVATION elev{};
        DWORD ret = 0;
        const BOOL ok = GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &ret);
        CloseHandle(tok);
        return ok && ret == sizeof(elev) && elev.TokenIsElevated != 0;
}

} // namespace usbip
