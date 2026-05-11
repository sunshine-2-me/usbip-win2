/*
 * Copyright (c) 2026
 */
#include "attach_state.h"

#include <algorithm>
#include <cwctype>

#include <spdlog/spdlog.h>

namespace usbip::broker
{

namespace
{

bool iequals_prefix(std::wstring_view haystack, std::wstring_view prefix)
{
        if (prefix.empty() || prefix.size() > haystack.size()) {
                return false;
        }
        for (std::size_t i = 0; i < prefix.size(); ++i) {
                if (std::towlower(haystack[i]) != std::towlower(prefix[i])) {
                        return false;
                }
        }
        return true;
}

bool wicontains(std::wstring_view haystack, std::wstring_view needle)
{
        if (needle.empty() || needle.size() > haystack.size()) {
                return false;
        }
        for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
                bool ok = true;
                for (std::size_t j = 0; j < needle.size(); ++j) {
                        if (std::towlower(haystack[i + j]) != std::towlower(needle[j])) {
                                ok = false;
                                break;
                        }
                }
                if (ok) {
                        return true;
                }
        }
        return false;
}

std::wstring busid_ascii_to_lower_wide(std::string_view busid)
{
        std::wstring w;
        w.reserve(busid.size());
        for (unsigned char c : busid) {
                if (c <= 127) {
                        w.push_back(static_cast<wchar_t>(std::tolower(static_cast<int>(c))));
                }
        }
        return w;
}

} // namespace

AttachState &AttachState::instance()
{
        static AttachState s;
        return s;
}

void AttachState::set(int port, AttachRecord rec)
{
        const auto sess = rec.session;
        const auto host = rec.host;
        const auto service = rec.service;
        const auto busid = rec.busid;
        std::lock_guard lk(m_);
        rec.port = port;
        by_port_[port] = std::move(rec);
        spdlog::debug("attach_state: set port={} session={} {}:{}/{}", port, sess, host, service,
                      busid);
}

void AttachState::erase(int port)
{
        std::lock_guard lk(m_);
        by_port_.erase(port);
        spdlog::debug("attach_state: erase port={}", port);
}

std::optional<AttachRecord> AttachState::get(int port) const
{
        std::lock_guard lk(m_);
        auto it = by_port_.find(port);
        if (it == by_port_.end()) return std::nullopt;
        return it->second;
}

std::optional<AttachRecord> AttachState::find_by_instance_prefix(std::wstring_view instance_id) const
{
        std::lock_guard lk(m_);
        for (auto &[port, rec] : by_port_) {
                if (!rec.pdo_instance_id.empty() &&
                    iequals_prefix(instance_id, rec.pdo_instance_id)) {
                        return rec;
                }
        }
        return std::nullopt;
}

std::optional<AttachRecord> AttachState::find_pending_for_usb_instance(
        std::wstring_view full_usb_instance_id) const
{
        std::lock_guard lk(m_);
        const AttachRecord *match = nullptr;
        int hits = 0;
        for (const auto &[port, rec] : by_port_) {
                if (!rec.pdo_instance_id.empty()) {
                        continue;
                }
                const auto busid_w = busid_ascii_to_lower_wide(rec.busid);
                if (busid_w.empty()) {
                        continue;
                }
                if (!wicontains(full_usb_instance_id, busid_w)) {
                        continue;
                }
                ++hits;
                match = &rec;
        }
        if (hits != 1) {
                if (hits > 1) {
                        spdlog::warn(
                                "attach_state: ambiguous pending match for usb instance (hits={})",
                                hits);
                }
                return std::nullopt;
        }
        return *match;
}

void AttachState::set_pdo_instance(int port, std::wstring instance_id)
{
        std::lock_guard lk(m_);
        if (auto it = by_port_.find(port); it != by_port_.end()) {
                it->second.pdo_instance_id = std::move(instance_id);
                spdlog::debug("attach_state: port={} PDO instance id set (len={})",
                              port, it->second.pdo_instance_id.size());
        }
}

void AttachState::set_session_drive_letter(int port, wchar_t letter)
{
        std::lock_guard lk(m_);
        if (auto it = by_port_.find(port); it != by_port_.end()) {
                it->second.session_drive_letter = letter;
                spdlog::debug("attach_state: port={} session drive letter U+{:04X}", port,
                              static_cast<unsigned>(letter));
        }
}

std::vector<std::pair<int, AttachRecord>> AttachState::list_by_session(DWORD session_id) const
{
        std::lock_guard lk(m_);
        std::vector<std::pair<int, AttachRecord>> out;
        for (const auto &[port, rec] : by_port_) {
                if (rec.session == session_id) {
                        out.emplace_back(port, rec);
                }
        }
        return out;
}

std::size_t AttachState::erase_by_session(DWORD session_id)
{
        std::lock_guard lk(m_);
        std::size_t removed = 0;
        for (auto it = by_port_.begin(); it != by_port_.end();) {
                if (it->second.session == session_id) {
                        it = by_port_.erase(it);
                        ++removed;
                } else {
                        ++it;
                }
        }
        if (removed) {
                spdlog::debug("attach_state: erase_by_session session={} removed={}", session_id,
                              removed);
        }
        return removed;
}

} // namespace usbip::broker
