/*
 * Copyright (c) 2026
 *
 * Dynamic ownership lock for (host,service,busid) tuples.
 * This is broker-managed runtime state persisted to %ProgramData%\USBip\owners.json.
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <windows.h>

namespace usbip::broker
{

struct OwnershipInfo
{
        std::string sid_string;
        DWORD session_id = 0;
        int   port = 0;
};

enum class ClaimResult
{
        claimed,
        already_owned_by_caller,
        owned_by_other,
};

class OwnershipStore
{
public:
        // Best-effort load of persisted state. Missing file is not an error.
        bool load() noexcept;

        // Path resolution helper.
        static std::wstring owners_path();

        /** Lower-case ASCII normalization for host/service/busid (policy + ownership keys). */
        static std::string lowercase_ascii(std::string_view s);

        // Claim (host,service,busid) for sid/session/port.
        ClaimResult try_claim(std::string_view host,
                              std::string_view service,
                              std::string_view busid,
                              std::string_view sid_string,
                              DWORD session_id,
                              int port,
                              OwnershipInfo *other_owner = nullptr);

        // Release tuple if owner matches the caller sid.
        bool release_if_owner(std::string_view host,
                              std::string_view service,
                              std::string_view busid,
                              std::string_view sid_string);

        // Release entry by port if sid is owner.
        bool release_if_owner_by_port(int port, std::string_view sid_string);

        // Release all ownership records mapped to a disconnected/logged-off session.
        std::size_t release_by_session(DWORD session_id);

        // Lookup ownership by port.
        std::optional<OwnershipInfo> get_owner_by_port(int port) const;

private:
        struct Entry
        {
                std::string host;
                std::string service;
                std::string busid;
                std::string sid_string;
                DWORD session_id = 0;
                int port = 0;
                std::uint64_t updated_at = 0;
        };

        static std::string make_key(std::string_view host,
                                    std::string_view service,
                                    std::string_view busid);
        static std::uint64_t now_unix_seconds();

        bool save_locked() const noexcept;
        bool load_from_text_locked(std::string_view json) noexcept;
        static std::string escape_json(std::string_view s);

        // key(host|service|busid) -> entry
        std::unordered_map<std::string, Entry> by_key_;
        // port -> key
        std::unordered_map<int, std::string> by_port_;
        mutable std::mutex m_;
};

} // namespace usbip::broker
