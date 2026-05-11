/*
 * Copyright (c) 2026
 */
#include "identity.h"

#include <vector>

#include <windows.h>
#include <sddl.h>

#include <spdlog/spdlog.h>

namespace usbip::broker
{

ImpersonationScope::ImpersonationScope(HANDLE pipe)
{
        if (ImpersonateNamedPipeClient(pipe)) {
                ok_ = true;
        } else {
                last_error_ = GetLastError();
                spdlog::warn("ImpersonateNamedPipeClient failed err={}", last_error_);
        }
}

ImpersonationScope::~ImpersonationScope()
{
        if (ok_) {
                if (!RevertToSelf()) {
                        spdlog::error("RevertToSelf failed err={}", GetLastError());
                }
        }
}

CallerIdentity capture_caller_identity_impersonating()
{
        CallerIdentity id;

        HANDLE token = nullptr;
        if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
                id.win32_error = GetLastError();
                spdlog::debug("identity: OpenThreadToken err={}", id.win32_error);
                return id;
        }

        DWORD needed = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
        std::vector<unsigned char> tu(needed);
        if (!GetTokenInformation(token, TokenUser, tu.data(), needed, &needed)) {
                id.win32_error = GetLastError();
                spdlog::debug("identity: GetTokenInformation(TokenUser) err={}", id.win32_error);
                CloseHandle(token);
                return id;
        }

        auto user = reinterpret_cast<const TOKEN_USER*>(tu.data());

        if (auto len = GetLengthSid(user->User.Sid)) {
                id.sid.resize(static_cast<size_t>(len));
                if (!CopySid(len, reinterpret_cast<PSID>(id.sid.data()), user->User.Sid)) {
                        id.sid.clear();
                }
        }

        LPWSTR str_sid = nullptr;
        if (ConvertSidToStringSidW(user->User.Sid, &str_sid)) {
                int wlen = WideCharToMultiByte(CP_UTF8, 0, str_sid, -1, nullptr, 0, nullptr, nullptr);
                if (wlen > 0) {
                        std::string s(static_cast<size_t>(wlen) - 1, '\0');
                        WideCharToMultiByte(CP_UTF8, 0, str_sid, -1, s.data(), wlen, nullptr, nullptr);
                        id.sid_string = std::move(s);
                }
                LocalFree(str_sid);
        }

        DWORD session_id = 0;
        DWORD ret_len = 0;
        if (GetTokenInformation(token, TokenSessionId, &session_id, sizeof(session_id), &ret_len)) {
                id.session_id = session_id;
        }

        CloseHandle(token);
        return id;
}

} // namespace usbip::broker
