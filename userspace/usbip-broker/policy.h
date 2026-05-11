/*
 * Copyright (c) 2026
 *
 * Per-user policy: which (host, service, busid) tuples each SID may attach,
 * and which device classes are denied.
 */
#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace usbip::broker
{

struct AllowedDevice
{
        std::string host;
        std::string service;
        std::string busid;
        std::unordered_set<std::string> deny_classes; // upper-case class names: "HID", "MASS"
};

struct UserRules
{
        std::vector<AllowedDevice> devices;

        // True if (host, service, busid) appears in `devices`.
        const AllowedDevice* match(std::string_view host,
                                   std::string_view service,
                                   std::string_view busid) const noexcept;
};

/*
 * Thread-safe policy snapshot loaded from %ProgramData%\USBip\policy.json.
 * Reload() is called from main on SIGHUP-equivalent (a control-event from SCM)
 * and from a directory-change watcher.
 */
class Policy
{
public:
        // Load (or reload) from policy.json. Returns true on success.
        // If the file is missing or malformed, the current snapshot is left intact
        // (a previous good snapshot keeps working) and false is returned.
        bool reload() noexcept;

        // Lookup rules for a SID string ("S-1-5-21-...").
        // Returns an empty UserRules if no rules apply (caller -> deny).
        UserRules lookup(std::string_view sid_string) const;

        // Path resolution helper. Always returns the same absolute path.
        static std::wstring policy_path();

        /** Normalized (host, service, busid): returns owning SID if any, else nullopt. */
        std::optional<std::string> find_tuple_owner_sid(std::string_view host,
                                                        std::string_view service,
                                                        std::string_view busid) const;

        /**
         * If the normalized tuple is bound to another SID, returns false.
         * If unbound, appends the tuple for caller_sid, persists policy.json, returns true.
         * If already bound to caller_sid, returns true (no duplicate row).
         */
        bool grant_tuple_for_sid(std::string_view caller_sid,
                                 std::string_view host,
                                 std::string_view service,
                                 std::string_view busid) noexcept;

        Policy();
        ~Policy() = default;
        Policy(const Policy &) = delete;
        Policy &operator=(const Policy &) = delete;

private:
        struct Impl;
        std::shared_ptr<Impl> impl_;
        mutable std::mutex    mutex_;

        bool save_to_path_locked(const std::wstring &path) noexcept;
};

} // namespace usbip::broker
