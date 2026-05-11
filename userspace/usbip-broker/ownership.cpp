/*
 * Copyright (c) 2026
 */
#include "ownership.h"
#include "mini_json.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <windows.h>
#include <shlobj.h>

#include <spdlog/spdlog.h>

namespace usbip::broker
{

namespace
{

std::string wstring_to_utf8(std::wstring_view s)
{
        if (s.empty()) {
                return {};
        }
        auto n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                     nullptr, 0, nullptr, nullptr);
        if (n <= 0) return {};
        std::string out(static_cast<std::size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                            out.data(), n, nullptr, nullptr);
        return out;
}

} // namespace

std::string OwnershipStore::lowercase_ascii(std::string_view s)
{
        std::string out(s);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
        });
        return out;
}

std::string OwnershipStore::make_key(std::string_view host,
                                     std::string_view service,
                                     std::string_view busid)
{
        auto h = lowercase_ascii(host);
        auto s = lowercase_ascii(service);
        auto b = lowercase_ascii(busid);
        std::string key;
        key.reserve(h.size() + s.size() + b.size() + 2);
        key += h;
        key.push_back('|');
        key += s;
        key.push_back('|');
        key += b;
        return key;
}

std::uint64_t OwnershipStore::now_unix_seconds()
{
        auto now = std::chrono::system_clock::now().time_since_epoch();
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(now).count());
}

std::string OwnershipStore::escape_json(std::string_view s)
{
        std::string out;
        out.reserve(s.size() + 8);
        for (auto c : s) {
                switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                        if (static_cast<unsigned char>(c) < 0x20) {
                                char tmp[8];
                                std::snprintf(tmp, sizeof(tmp), "\\u%04x", static_cast<unsigned char>(c));
                                out += tmp;
                        } else {
                                out.push_back(c);
                        }
                }
        }
        return out;
}

std::wstring OwnershipStore::owners_path()
{
        wchar_t path[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr,
                                    SHGFP_TYPE_CURRENT, path))) {
                return L"C:\\ProgramData\\USBip\\owners.json";
        }
        std::wstring p = path;
        p += L"\\USBip\\owners.json";
        return p;
}

bool OwnershipStore::load_from_text_locked(std::string_view text) noexcept
{
        mini_json::ParseError err;
        auto root = mini_json::parse(text, &err);
        if (!err.message.empty()) {
                spdlog::error("owners parse error at offset {}: {}", err.offset, err.message);
                return false;
        }

        auto owners = root.at("owners").as_array();
        if (!owners) {
                spdlog::error("owners: missing or non-array 'owners'");
                return false;
        }

        std::unordered_map<std::string, Entry> by_key;
        std::unordered_map<int, std::string> by_port;

        for (auto &v : *owners) {
                Entry e;
                e.host = std::string(v.at("host").as_string());
                e.service = std::string(v.at("service").as_string());
                e.busid = std::string(v.at("busid").as_string());
                e.sid_string = std::string(v.at("sid").as_string());
                e.session_id = static_cast<DWORD>(v.at("session_id").as_int(0));
                e.port = static_cast<int>(v.at("port").as_int(0));
                e.updated_at = static_cast<std::uint64_t>(v.at("updated_at").as_int(0));
                if (e.updated_at == 0) {
                        e.updated_at = now_unix_seconds();
                }

                if (e.host.empty() || e.service.empty() || e.busid.empty() || e.sid_string.empty()) {
                        continue;
                }

                auto key = make_key(e.host, e.service, e.busid);
                by_key.insert_or_assign(key, e);
                if (e.port > 0) {
                        by_port[e.port] = key;
                }
        }

        by_key_ = std::move(by_key);
        by_port_ = std::move(by_port);
        return true;
}

bool OwnershipStore::load() noexcept
{
        try {
                auto path = owners_path();
                std::ifstream ifs(path);
                if (!ifs) {
                        // Missing owners file is expected on first run.
                        std::lock_guard lk(m_);
                        by_key_.clear();
                        by_port_.clear();
                        return true;
                }

                std::stringstream ss;
                ss << ifs.rdbuf();

                std::lock_guard lk(m_);
                return load_from_text_locked(ss.str());
        } catch (const std::exception &ex) {
                spdlog::error("owners load exception: {}", ex.what());
                return false;
        }
}

bool OwnershipStore::save_locked() const noexcept
{
        try {
                std::string json;
                json.reserve(by_key_.size() * 160 + 64);
                json += "{\n  \"owners\": [\n";
                bool first = true;
                for (const auto &[key, e] : by_key_) {
                        (void)key;
                        if (!first) {
                                json += ",\n";
                        }
                        first = false;
                        json += "    {\"host\":\"";
                        json += escape_json(e.host);
                        json += "\",\"service\":\"";
                        json += escape_json(e.service);
                        json += "\",\"busid\":\"";
                        json += escape_json(e.busid);
                        json += "\",\"sid\":\"";
                        json += escape_json(e.sid_string);
                        json += "\",\"session_id\":";
                        json += std::to_string(e.session_id);
                        json += ",\"port\":";
                        json += std::to_string(e.port);
                        json += ",\"updated_at\":";
                        json += std::to_string(e.updated_at);
                        json += "}";
                }
                json += "\n  ]\n}\n";

                auto path = owners_path();
                auto tmp = path + L".tmp";

                {
                        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
                        if (!ofs) {
                                spdlog::error("owners save: cannot open temp file {}", wstring_to_utf8(tmp));
                                return false;
                        }
                        ofs.write(json.data(), static_cast<std::streamsize>(json.size()));
                        if (!ofs) {
                                spdlog::error("owners save: short write {}", wstring_to_utf8(tmp));
                                return false;
                        }
                        ofs.flush();
                }

                if (!MoveFileExW(tmp.c_str(), path.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                        auto err = GetLastError();
                        spdlog::error("owners save: MoveFileExW failed err={} path={}",
                                      err, wstring_to_utf8(path));
                        DeleteFileW(tmp.c_str());
                        return false;
                }
                return true;
        } catch (const std::exception &ex) {
                spdlog::error("owners save exception: {}", ex.what());
                return false;
        }
}

ClaimResult OwnershipStore::try_claim(std::string_view host,
                                      std::string_view service,
                                      std::string_view busid,
                                      std::string_view sid_string,
                                      DWORD session_id,
                                      int port,
                                      OwnershipInfo *other_owner)
{
        std::lock_guard lk(m_);
        auto key = make_key(host, service, busid);
        auto it = by_key_.find(key);
        if (it == by_key_.end()) {
                Entry e;
                e.host = std::string(host);
                e.service = std::string(service);
                e.busid = std::string(busid);
                e.sid_string = std::string(sid_string);
                e.session_id = session_id;
                e.port = port;
                e.updated_at = now_unix_seconds();
                by_key_[key] = std::move(e);
                if (port > 0) {
                        by_port_[port] = key;
                }
                save_locked();
                return ClaimResult::claimed;
        }

        auto &e = it->second;
        if (e.sid_string != sid_string) {
                if (other_owner) {
                        other_owner->sid_string = e.sid_string;
                        other_owner->session_id = e.session_id;
                        other_owner->port = e.port;
                }
                return ClaimResult::owned_by_other;
        }

        e.session_id = session_id;
        if (port > 0) {
                if (e.port > 0 && e.port != port) {
                        by_port_.erase(e.port);
                }
                e.port = port;
                by_port_[port] = key;
        }
        e.updated_at = now_unix_seconds();
        save_locked();
        return ClaimResult::already_owned_by_caller;
}

bool OwnershipStore::release_if_owner(std::string_view host,
                                      std::string_view service,
                                      std::string_view busid,
                                      std::string_view sid_string)
{
        std::lock_guard lk(m_);
        auto key = make_key(host, service, busid);
        auto it = by_key_.find(key);
        if (it == by_key_.end() || it->second.sid_string != sid_string) {
                return false;
        }
        if (it->second.port > 0) {
                by_port_.erase(it->second.port);
        }
        by_key_.erase(it);
        save_locked();
        return true;
}

bool OwnershipStore::release_if_owner_by_port(int port, std::string_view sid_string)
{
        if (port <= 0) {
                return false;
        }

        std::lock_guard lk(m_);
        auto pit = by_port_.find(port);
        if (pit == by_port_.end()) {
                return false;
        }
        auto it = by_key_.find(pit->second);
        if (it == by_key_.end() || it->second.sid_string != sid_string) {
                return false;
        }
        by_port_.erase(pit);
        by_key_.erase(it);
        save_locked();
        return true;
}

std::size_t OwnershipStore::release_by_session(DWORD session_id)
{
        std::lock_guard lk(m_);
        std::size_t removed = 0;
        for (auto it = by_key_.begin(); it != by_key_.end();) {
                if (it->second.session_id == session_id) {
                        if (it->second.port > 0) {
                                by_port_.erase(it->second.port);
                        }
                        it = by_key_.erase(it);
                        ++removed;
                } else {
                        ++it;
                }
        }
        if (removed) {
                save_locked();
        }
        return removed;
}

std::optional<OwnershipInfo> OwnershipStore::get_owner_by_port(int port) const
{
        if (port <= 0) return std::nullopt;
        std::lock_guard lk(m_);
        auto pit = by_port_.find(port);
        if (pit == by_port_.end()) {
                return std::nullopt;
        }
        auto it = by_key_.find(pit->second);
        if (it == by_key_.end()) {
                return std::nullopt;
        }
        OwnershipInfo out;
        out.sid_string = it->second.sid_string;
        out.session_id = it->second.session_id;
        out.port = it->second.port;
        return out;
}

} // namespace usbip::broker
