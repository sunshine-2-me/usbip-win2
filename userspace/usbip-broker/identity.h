/*
 * Copyright (c) 2026
 *
 * Helpers for capturing the calling user's SID and session through a
 * connected named pipe, and RAII for impersonation.
 */
#pragma once

#include <string>
#include <vector>

#include <windows.h>

namespace usbip::broker
{

struct CallerIdentity
{
        std::string sid_string{};                       // textual "S-1-5-...", UTF-8
        std::vector<unsigned char> sid{};               // raw self-relative SID bytes
        DWORD session_id = 0;
        DWORD win32_error = 0;                          // 0 on success
};

/*
 * Pull the SID and TS session from the currently impersonated thread token.
 * Caller must already be impersonating (e.g. via ImpersonateNamedPipeClient).
 */
CallerIdentity capture_caller_identity_impersonating();

/*
 * RAII: ImpersonateNamedPipeClient on construction, RevertToSelf on destruction.
 * Use sparingly: hold for the minimum time needed to perform a kernel-side IOCTL.
 */
class ImpersonationScope
{
public:
        explicit ImpersonationScope(HANDLE pipe);
        ~ImpersonationScope();

        ImpersonationScope(const ImpersonationScope&) = delete;
        ImpersonationScope& operator=(const ImpersonationScope&) = delete;

        bool ok() const noexcept { return ok_; }
        DWORD error() const noexcept { return last_error_; }

private:
        bool  ok_ = false;
        DWORD last_error_ = 0;
};

} // namespace usbip::broker
