/*
 * Copyright (c) 2026
 */
#include "policy.h"
#include "mini_json.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <filesystem>
#include <optional>

#include <windows.h>
#include <shlobj.h>

#include <spdlog/spdlog.h>

namespace usbip::broker
{

namespace
{

std::string toupper_ascii(std::string_view s)
{
        std::string out(s);
        std::transform(out.begin(), out.end(), out.begin(),
                [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return out;
}

std::string lowercase_ascii(std::string_view s)
{
        std::string out(s);
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
}

UserRules parse_user_rules(const mini_json::Value &arr)
{
        UserRules r;
        auto a = arr.as_array();
        if (!a) return r;

        r.devices.reserve(a->size());
        for (auto &entry : *a) {
                AllowedDevice dev;
                dev.host    = lowercase_ascii(entry.at("host").as_string());
                dev.service = lowercase_ascii(entry.at("service").as_string());
                dev.busid   = lowercase_ascii(entry.at("busid").as_string());

                if (auto cls = entry.at("deny_classes").as_array()) {
                        for (auto &c : *cls) {
                                if (c.is_string()) {
                                        dev.deny_classes.insert(toupper_ascii(c.as_string()));
                                }
                        }
                }
                if (!dev.host.empty() && !dev.service.empty() && !dev.busid.empty()) {
                        r.devices.push_back(std::move(dev));
                }
        }
        return r;
}

} // namespace

const AllowedDevice* UserRules::match(std::string_view host,
                                      std::string_view service,
                                      std::string_view busid) const noexcept
{
        for (auto &d : devices) {
                if (d.host == host && d.service == service && d.busid == busid) {
                        return &d;
                }
        }
        return nullptr;
}

struct Policy::Impl
{
        std::unordered_map<std::string, UserRules> rules; // SID string -> rules
};

std::wstring Policy::policy_path()
{
        wchar_t path[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr,
                                    SHGFP_TYPE_CURRENT, path))) {
                return L"C:\\ProgramData\\USBip\\policy.json";
        }
        std::wstring p = path;
        p += L"\\USBip\\policy.json";
        return p;
}

bool Policy::reload() noexcept
{
        try {
                auto path = policy_path();
                std::ifstream ifs(path);
                if (!ifs) {
                        std::string path_utf8;
                        if (!path.empty()) {
                                auto n = WideCharToMultiByte(CP_UTF8, 0, path.c_str(),
                                                             -1, nullptr, 0, nullptr, nullptr);
                                if (n > 1) {
                                        path_utf8.resize(static_cast<size_t>(n) - 1);
                                        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1,
                                                            path_utf8.data(), n, nullptr,
                                                            nullptr);
                                }
                        }
                        spdlog::warn("policy reload: cannot open {}", path_utf8);
                        return false;
                }
                std::stringstream buf;
                buf << ifs.rdbuf();

                mini_json::ParseError err;
                auto root = mini_json::parse(buf.str(), &err);
                if (!err.message.empty()) {
                        spdlog::error("policy parse error at offset {}: {}", err.offset, err.message);
                        return false;
                }

                auto users = root.at("users").as_object();
                if (!users) {
                        spdlog::error("policy: missing or non-object 'users'");
                        return false;
                }

                auto impl = std::make_shared<Impl>();
                for (auto &[sid, list] : *users) {
                        impl->rules[sid] = parse_user_rules(list);
                }

                {
                        std::lock_guard lock(mutex_);
                        impl_ = std::move(impl);
                }
                spdlog::info("policy reloaded: {} user(s)", impl_->rules.size());
                return true;
        } catch (const std::exception &ex) {
                spdlog::error("policy reload exception: {}", ex.what());
                return false;
        }
}

UserRules Policy::lookup(std::string_view sid_string) const
{
        std::shared_ptr<Impl> snap;
        {
                std::lock_guard lock(mutex_);
                snap = impl_;
        }
        if (!snap) return {};
        auto it = snap->rules.find(std::string(sid_string));
        if (it == snap->rules.end()) return {};
        return it->second;
}

Policy::Policy() : impl_(std::make_shared<Impl>()) {}

std::optional<std::string> Policy::find_tuple_owner_sid(std::string_view host,
                                                      std::string_view service,
                                                      std::string_view busid) const
{
        const auto h = lowercase_ascii(host);
        const auto s = lowercase_ascii(service);
        const auto b = lowercase_ascii(busid);

        std::shared_ptr<Impl> snap;
        {
                std::lock_guard lock(mutex_);
                snap = impl_;
        }
        if (!snap) {
                return std::nullopt;
        }
        for (const auto &[sid, rules] : snap->rules) {
                if (rules.match(h, s, b)) {
                        return sid;
                }
        }
        return std::nullopt;
}

bool Policy::grant_tuple_for_sid(std::string_view caller_sid,
                                 std::string_view host,
                                 std::string_view service,
                                 std::string_view busid) noexcept
{
        try {
                const std::string caller(caller_sid);
                const auto h = lowercase_ascii(host);
                const auto s = lowercase_ascii(service);
                const auto b = lowercase_ascii(busid);

                std::lock_guard lock(mutex_);
                if (!impl_) {
                        impl_ = std::make_shared<Impl>();
                }

                for (const auto &[sid, rules] : impl_->rules) {
                        if (!rules.match(h, s, b)) {
                                continue;
                        }
                        if (sid != caller) {
                                spdlog::info(
                                        "policy: attach denied tuple {}/{}/{} owned by sid {}",
                                        h, s, b, sid);
                                return false;
                        }
                        return true;
                }

                auto path = policy_path();
                auto &rules = impl_->rules[caller];
                AllowedDevice dev;
                dev.host    = h;
                dev.service = s;
                dev.busid   = b;
                rules.devices.push_back(std::move(dev));

                if (!save_to_path_locked(path)) {
                        rules.devices.pop_back();
                        if (rules.devices.empty()) {
                                impl_->rules.erase(caller);
                        }
                        spdlog::error("policy: failed to persist tuple {}/{}/{} for sid {}", h, s,
                                      b, caller);
                        return false;
                }
                spdlog::info("policy: auto-granted tuple {}/{}/{} for sid {}", h, s, b, caller);
                return true;
        } catch (const std::exception &ex) {
                spdlog::error("policy: grant_tuple_for_sid exception: {}", ex.what());
                return false;
        } catch (...) {
                return false;
        }
}

bool Policy::save_to_path_locked(const std::wstring &path) noexcept
{
        try {
                if (!impl_) {
                        return false;
                }

                std::vector<std::pair<std::string, const UserRules *>> ordered;
                ordered.reserve(impl_->rules.size());
                for (const auto &pr : impl_->rules) {
                        ordered.emplace_back(pr.first, &pr.second);
                }
                std::sort(ordered.begin(), ordered.end(),
                          [](const auto &a, const auto &b) { return a.first < b.first; });

                mini_json::ObjectMap users_obj;
                for (const auto &[sid, rules_ptr] : ordered) {
                        mini_json::ArrayVec arr;
                        for (const auto &d : rules_ptr->devices) {
                                mini_json::ObjectMap dev_obj;
                                dev_obj.emplace("host", mini_json::Value(std::string(d.host)));
                                dev_obj.emplace("service", mini_json::Value(std::string(d.service)));
                                dev_obj.emplace("busid", mini_json::Value(std::string(d.busid)));
                                std::vector<std::string> denies(d.deny_classes.begin(),
                                                                 d.deny_classes.end());
                                std::sort(denies.begin(), denies.end());
                                mini_json::ArrayVec deny_arr;
                                for (const auto &c : denies) {
                                        deny_arr.emplace_back(mini_json::Value(c));
                                }
                                dev_obj.emplace("deny_classes", mini_json::Value(std::move(deny_arr)));
                                arr.emplace_back(mini_json::Value(std::move(dev_obj)));
                        }
                        users_obj.emplace(sid, mini_json::Value(std::move(arr)));
                }
                mini_json::ObjectMap root_obj;
                root_obj.emplace("users", mini_json::Value(std::move(users_obj)));
                const mini_json::Value doc(std::move(root_obj));
                const std::string utf8 = mini_json::serialize(doc);

                const std::filesystem::path fs_path(path);
                std::error_code ec;
                std::filesystem::create_directories(fs_path.parent_path(), ec);
                (void)ec;

                const std::wstring tmp = path + L".tmp";
                const HANDLE fh = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                               nullptr);
                if (fh == INVALID_HANDLE_VALUE) {
                        spdlog::error("policy save: CreateFileW tmp err={}", GetLastError());
                        return false;
                }

                const char *p   = utf8.data();
                std::size_t left = utf8.size();
                while (left > 0) {
                        const DWORD chunk =
                                left > static_cast<std::size_t>(1 << 20)
                                        ? static_cast<DWORD>(1 << 20)
                                        : static_cast<DWORD>(left);
                        DWORD written = 0;
                        if (!WriteFile(fh, p, chunk, &written, nullptr) || written != chunk) {
                                spdlog::error("policy save: WriteFile err={}", GetLastError());
                                CloseHandle(fh);
                                DeleteFileW(tmp.c_str());
                                return false;
                        }
                        p += chunk;
                        left -= chunk;
                }
                FlushFileBuffers(fh);
                CloseHandle(fh);

                if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                        spdlog::error("policy save: MoveFileExW err={}", GetLastError());
                        DeleteFileW(tmp.c_str());
                        return false;
                }
                return true;
        } catch (const std::exception &ex) {
                spdlog::error("policy save exception: {}", ex.what());
                return false;
        } catch (...) {
                return false;
        }
}

} // namespace usbip::broker
