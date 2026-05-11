/*
 * Copyright (c) 2026
 */
#include "handler.h"
#include "attach_state.h"
#include "class_check.h"
#include "filter_client.h"
#include "identity.h"
#include "mini_json.h"
#include "ownership.h"
#include "pdo_post_attach.h"
#include "policy.h"
#include "protocol.h"
#include "vhci_access.h"
#include "volume_watcher.h"

#include <cstring>
#include <optional>
#include <vector>

#include <windows.h>
#include <cfgmgr32.h>

#include <spdlog/spdlog.h>

#include <libusbip/remote.h>
#include <initguid.h>
#include <usbip/vhci.h>

namespace usbip::broker
{

namespace
{

std::string read_message(HANDLE pipe)
{
        std::string buf;
        buf.resize(8192);
        DWORD bytes = 0;
        while (true) {
                if (ReadFile(pipe, buf.data(), DWORD(buf.size()), &bytes, nullptr)) {
                        buf.resize(bytes);
                        return buf;
                }
                auto err = GetLastError();
                if (err == ERROR_MORE_DATA) {
                        DWORD avail = 0;
                        DWORD left  = 0;
                        DWORD got   = 0;
                        PeekNamedPipe(pipe, nullptr, 0, &got, &avail, &left);
                        buf.resize(buf.size() + std::max<DWORD>(left, 1024));
                        DWORD more = 0;
                        if (!ReadFile(pipe,
                                      buf.data() + bytes,
                                      DWORD(buf.size() - bytes),
                                      &more, nullptr)) {
                                spdlog::debug("read_message: ReadFile (MORE_DATA) err={}",
                                              GetLastError());
                                buf.clear();
                                return buf;
                        }
                        bytes += more;
                        buf.resize(bytes);
                        return buf;
                }
                buf.clear();
                spdlog::debug("read_message: ReadFile err={}, empty message", err);
                return buf;
        }
}

bool write_message(HANDLE pipe, std::string_view msg)
{
        DWORD written = 0;
        if (msg.size() > std::numeric_limits<DWORD>::max()) {
                return false;
        }
        return WriteFile(pipe, msg.data(), DWORD(msg.size()), &written, nullptr) != 0
                && written == msg.size();
}

std::string json_escape(std::string_view s)
{
        std::string out;
        out.reserve(s.size() + 2);
        out.push_back('"');
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
                                std::snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                                out += tmp;
                        } else {
                                out.push_back(c);
                        }
                }
        }
        out.push_back('"');
        return out;
}

std::string error_response(std::string_view code, std::string_view detail = {})
{
        std::string out = "{\"ok\":false,\"error\":";
        out += json_escape(code);
        if (!detail.empty()) {
                out += ",\"detail\":";
                out += json_escape(detail);
        }
        out += "}";
        return out;
}

bool fill_location(::usbip::vhci::imported_device_location &loc,
                   std::string_view host,
                   std::string_view service,
                   std::string_view busid)
{
        if (host.size() >= sizeof(loc.host) ||
            service.size() >= sizeof(loc.service) ||
            busid.size() >= sizeof(loc.busid)) {
                return false;
        }
        std::memset(&loc, 0, sizeof(loc));
        std::memcpy(loc.host,    host.data(),    host.size());
        std::memcpy(loc.service, service.data(), service.size());
        std::memcpy(loc.busid,   busid.data(),   busid.size());
        return true;
}

std::string handle_attach(HANDLE pipe,
                          const mini_json::Value &req,
                          Policy &policy,
                          OwnershipStore &ownership)
{
        // Capture identity under impersonation.
        CallerIdentity id;
        {
                ImpersonationScope imp(pipe);
                if (!imp.ok()) {
                        return error_response("impersonation_failed",
                                              std::to_string(imp.error()));
                }
                id = capture_caller_identity_impersonating();
        }
        if (id.win32_error || id.sid_string.empty()) {
                return error_response("identity_capture_failed",
                                      std::to_string(id.win32_error));
        }

        spdlog::debug("attach: caller session={} sid={}", id.session_id, id.sid_string);

        auto host    = std::string(req.at("host").as_string());
        auto service = std::string(req.at("service").as_string());
        auto busid   = std::string(req.at("busid").as_string());
        bool once    = req.at("once").as_bool(true);

        if (host.empty() || service.empty() || busid.empty()) {
                return error_response("invalid_request");
        }

        const auto nhost    = OwnershipStore::lowercase_ascii(host);
        const auto nservice = OwnershipStore::lowercase_ascii(service);
        const auto nbusid   = OwnershipStore::lowercase_ascii(busid);

        if (auto bound_sid = policy.find_tuple_owner_sid(nhost, nservice, nbusid)) {
                if (*bound_sid != id.sid_string) {
                        spdlog::info("attach denied: tuple {}:{}/{} owned by policy sid {}",
                                     nhost, nservice, nbusid, *bound_sid);
                        return error_response("access_denied");
                }
        } else {
                if (!policy.grant_tuple_for_sid(id.sid_string, nhost, nservice, nbusid)) {
                        if (auto raced = policy.find_tuple_owner_sid(nhost, nservice, nbusid)) {
                                if (*raced != id.sid_string) {
                                        spdlog::info(
                                                "attach denied: tuple {}:{}/{} owned by policy sid {}",
                                                nhost, nservice, nbusid, *raced);
                                        return error_response("access_denied");
                                }
                        }
                        return error_response("policy_persist_failed");
                }
        }

        auto rules = policy.lookup(id.sid_string);
        const auto *allow = rules.match(nhost, nservice, nbusid);
        if (!allow) {
                spdlog::error("attach policy inconsistent for sid {} tuple {}:{}/{}",
                              id.sid_string, nhost, nservice, nbusid);
                return error_response("policy_inconsistent");
        }

        OwnershipInfo conflicting_owner;
        auto claim = ownership.try_claim(host, service, busid,
                                         id.sid_string, id.session_id, 0,
                                         &conflicting_owner);
        if (claim == ClaimResult::owned_by_other) {
                spdlog::info("attach rejected: {}:{} / {} owned by sid {}",
                             host, busid, service, conflicting_owner.sid_string);
                return error_response("device_owned_by_another_user");
        }

        // Phase 9: USB HID (class 0x03) — query remote devlist before attach.
        auto cls = probe_usb_classes_for_busid(host, service, busid);
        if (cls.enum_ok) {
                if (cls.has_hid) {
                        if (allow->deny_classes.count("HID")) {
                                spdlog::info("HID attach denied by policy for {} {}:{}/{}",
                                               id.sid_string, host, service, busid);
                                return error_response("hid_denied_by_policy");
                        }
                        spdlog::warn(
                                "HID device {}:{}/{} — input may not stay in this RDP session; "
                                "policy allows HID for {}",
                                host, service, busid, id.sid_string);
                }
        } else {
                spdlog::warn(
                        "USB class probe failed for {}:{}/{}; skipping HID deny_classes check",
                        host, service, busid);
        }

        // Open VHCI as LocalSystem (we are not impersonating here).
        auto vhci = vhci_access::open();
        if (vhci == INVALID_HANDLE_VALUE) {
                return error_response("vhci_open_failed",
                                      std::to_string(GetLastError()));
        }

        ::usbip::vhci::ioctl::plugin_hardware r{};
        r.size = sizeof(r);
        if (!fill_location(r, host, service, busid)) {
                CloseHandle(vhci);
                return error_response("invalid_request", "string too long");
        }

        // Impersonate while issuing IOCTL so the kernel captures the user's SID.
        DWORD bytes = 0;
        BOOL ok = FALSE;
        DWORD ioctl_err = 0;
        {
                ImpersonationScope imp(pipe);
                if (!imp.ok()) {
                        CloseHandle(vhci);
                        return error_response("impersonation_failed",
                                              std::to_string(imp.error()));
                }
                auto code = once
                        ? ::usbip::vhci::ioctl::PLUGIN_HARDWARE_ONCE
                        : ::usbip::vhci::ioctl::PLUGIN_HARDWARE;
                ok = DeviceIoControl(vhci, code, &r, sizeof(r), &r, sizeof(r), &bytes, nullptr);
                if (!ok) {
                        ioctl_err = GetLastError();
                }
        }
        CloseHandle(vhci);

        if (!ok) {
                spdlog::debug("attach: PLUGIN_HARDWARE ioctl err={} once={}", ioctl_err, once);
                // Roll back an optimistic claim on failed attach.
                ownership.release_if_owner(host, service, busid, id.sid_string);
                return error_response("ioctl_failed", std::to_string(ioctl_err));
        }

        // Phase 5/8: stash the attach in AttachState so the volume watcher and
        // the filter SET_OWNER pass can resolve owner from a future volume PDO.
        if (r.port > 0 && !id.sid.empty()) {
                AttachRecord rec;
                rec.port = r.port;
                rec.host = host;
                rec.service = service;
                rec.busid = busid;
                rec.session = id.session_id;
                rec.sid_bytes.assign(id.sid.begin(), id.sid.end());

                // Convert sid_string (UTF-8) to wstring for AttachState.sid.
                rec.sid.reserve(id.sid_string.size());
                for (auto c : id.sid_string) {
                        rec.sid.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
                }

                const auto sid_bytes_for_filter = rec.sid_bytes;
                const DWORD session_for_filter  = id.session_id;
                AttachState::instance().set(r.port, std::move(rec));
                try_register_filter_via_vhci_enumeration(r.port, busid, sid_bytes_for_filter,
                                                         session_for_filter);
                spdlog::debug(
                        "attach: post-register volume catch-up port={} session={} pdo_id_len={}",
                        r.port, session_for_filter,
                        AttachState::instance().get(r.port)
                                .transform([](const AttachRecord &a) {
                                        return static_cast<int>(a.pdo_instance_id.size());
                                })
                                .value_or(-1));
                try_mount_existing_volumes_for_port(r.port);
                if (auto ar = AttachState::instance().get(r.port)) {
                        if (ar->session_drive_letter == 0 && ar->session != 0
                            && !ar->pdo_instance_id.empty()) {
                                spdlog::debug(
                                        "attach: no session letter yet; retry volume catch-up "
                                        "after 300ms port={}",
                                        r.port);
                                Sleep(300);
                                try_mount_existing_volumes_for_port(r.port);
                        }
                        if (auto ar2 = AttachState::instance().get(r.port)) {
                                const unsigned letter_u = ar2->session_drive_letter != 0
                                                                  ? static_cast<unsigned>(
                                                                            ar2->session_drive_letter)
                                                                  : 0u;
                                spdlog::debug("attach: volume catch-up done port={} "
                                              "session_drive_letter_u={} (0=unset)",
                                              r.port, letter_u);
                        }
                }
        }
        ownership.try_claim(host, service, busid, id.sid_string, id.session_id, r.port);

        std::string out = "{\"ok\":true,\"port\":";
        out += std::to_string(r.port);
        out += "}";
        spdlog::info("attached for {} -> port {}", id.sid_string, r.port);
        return out;
}

std::string handle_detach(HANDLE pipe, const mini_json::Value &req, Policy &/*policy*/, OwnershipStore &ownership)
{
        CallerIdentity id;
        {
                ImpersonationScope imp(pipe);
                if (!imp.ok()) {
                        return error_response("impersonation_failed");
                }
                id = capture_caller_identity_impersonating();
        }
        if (id.sid_string.empty()) {
                return error_response("identity_capture_failed");
        }

        spdlog::debug("detach: caller sid={}", id.sid_string);

        auto port = req.at("port").as_int(-1);
        if (port == -1) {
                return error_response("invalid_request");
        }

        std::optional<AttachRecord> attach_snapshot;
        std::wstring filter_instance_id;
        if (port > 0) {
                if (auto ar = AttachState::instance().get(static_cast<int>(port))) {
                        attach_snapshot = *ar;
                        if (!ar->pdo_instance_id.empty()) {
                                filter_instance_id = ar->pdo_instance_id;
                        }
                }
        }

        if (auto owner = ownership.get_owner_by_port(static_cast<int>(port))) {
                if (owner->sid_string != id.sid_string) {
                        return error_response("access_denied");
                }
        }

        if (attach_snapshot && attach_snapshot->session_drive_letter != 0
            && attach_snapshot->session != 0) {
                unmount_session_drive_letter(attach_snapshot->session,
                                            attach_snapshot->session_drive_letter);
        }

        auto vhci = vhci_access::open();
        if (vhci == INVALID_HANDLE_VALUE) {
                return error_response("vhci_open_failed",
                                      std::to_string(GetLastError()));
        }

        ::usbip::vhci::ioctl::plugout_hardware r{};
        r.size = sizeof(r);
        r.port = static_cast<int>(port);

        DWORD bytes = 0;
        BOOL ok = FALSE;
        DWORD ioctl_err = 0;
        {
                ImpersonationScope imp(pipe);
                if (!imp.ok()) {
                        CloseHandle(vhci);
                        return error_response("impersonation_failed");
                }
                ok = DeviceIoControl(vhci, ::usbip::vhci::ioctl::PLUGOUT_HARDWARE,
                                     &r, sizeof(r), nullptr, 0, &bytes, nullptr);
                if (!ok) {
                        ioctl_err = GetLastError();
                }
        }
        CloseHandle(vhci);

        if (!ok) {
                spdlog::debug("detach: PLUGOUT_HARDWARE ioctl err={} port={}", ioctl_err, port);
                return error_response("ioctl_failed", std::to_string(ioctl_err));
        }
        if (port > 0) {
                if (!ownership.release_if_owner_by_port(static_cast<int>(port), id.sid_string)) {
                        spdlog::warn("detach succeeded for port {} but ownership entry was absent/mismatched", port);
                }
                AttachState::instance().erase(static_cast<int>(port));
                if (!filter_instance_id.empty()) {
                        FilterClient::unregister_owner(filter_instance_id);
                }
        }
        return "{\"ok\":true}";
}

// Cosmetic list. The kernel already filters per caller (Phase 6); we issue
// the IOCTL while impersonating so non-owners only see their own slots.
std::string handle_list(HANDLE pipe, const mini_json::Value &/*req*/, Policy &/*policy*/)
{
        CallerIdentity id;
        {
                ImpersonationScope imp(pipe);
                if (!imp.ok()) {
                        return error_response("impersonation_failed");
                }
                id = capture_caller_identity_impersonating();
        }
        if (id.sid_string.empty()) {
                return error_response("identity_capture_failed");
        }

        auto vhci = vhci_access::open();
        if (vhci == INVALID_HANDLE_VALUE) {
                return error_response("vhci_open_failed");
        }

        std::vector<char> buf;
        ::usbip::vhci::ioctl::get_imported_devices *r = nullptr;
        DWORD bytes = 0;
        BOOL ok = FALSE;
        DWORD ioctl_err = 0;

        for (auto cnt = 4u;; cnt <<= 1) {
                buf.resize(::usbip::vhci::ioctl::get_imported_devices_size(cnt));
                r = reinterpret_cast<::usbip::vhci::ioctl::get_imported_devices*>(buf.data());
                r->size = sizeof(*r);

                ImpersonationScope imp(pipe);
                if (!imp.ok()) {
                        CloseHandle(vhci);
                        return error_response("impersonation_failed");
                }

                ok = DeviceIoControl(vhci,
                                     ::usbip::vhci::ioctl::GET_IMPORTED_DEVICES,
                                     r, sizeof(r->size),
                                     buf.data(), DWORD(buf.size()),
                                     &bytes, nullptr);
                if (ok) {
                        break;
                }
                ioctl_err = GetLastError();
                if (ioctl_err != ERROR_INSUFFICIENT_BUFFER || cnt > 1024) {
                        break;
                }
        }
        CloseHandle(vhci);

        if (!ok) {
                spdlog::debug("list: GET_IMPORTED_DEVICES ioctl err={}", ioctl_err);
                return error_response("ioctl_failed", std::to_string(ioctl_err));
        }

        constexpr auto offset = offsetof(::usbip::vhci::ioctl::get_imported_devices, devices);
        if (bytes < offset) {
                return error_response("driver_response");
        }
        auto count = (bytes - offset) / sizeof(*r->devices);

        spdlog::debug("list: sid={} returning {} device slot(s)", id.sid_string, count);

        std::string out = "{\"ok\":true,\"devices\":[";
        for (size_t i = 0; i < count; ++i) {
                if (i) out.push_back(',');
                auto &d = r->devices[i];
                out += "{\"port\":" + std::to_string(d.port);
                out += ",\"host\":"    + json_escape(d.host);
                out += ",\"service\":" + json_escape(d.service);
                out += ",\"busid\":"   + json_escape(d.busid);
                out += ",\"vid\":" + std::to_string(d.vendor);
                out += ",\"pid\":" + std::to_string(d.product);
                out += ",\"devid\":" + std::to_string(d.devid);
                out += "}";
        }
        out += "]}";
        return out;
}

} // namespace

bool RequestHandler::serve_one(HANDLE pipe)
{
        auto msg = read_message(pipe);
        if (msg.empty()) {
                spdlog::debug("serve_one: eof or read error, ending connection");
                return false;
        }
        if (msg.size() > kMaxMessageBytes) {
                spdlog::debug("serve_one: rejecting oversize message ({})", msg.size());
                write_message(pipe, error_response("oversize"));
                return false;
        }

        auto resp = handle(pipe, msg);
        if (!write_message(pipe, resp)) {
                spdlog::warn("WriteFile pipe failed err={}", GetLastError());
                return false;
        }
        return true;
}

std::string RequestHandler::handle(HANDLE pipe, std::string_view request_json)
{
        mini_json::ParseError perr;
        auto root = mini_json::parse(request_json, &perr);
        if (!perr.message.empty()) {
                spdlog::warn("bad json from pipe: {}", perr.message);
                return error_response("bad_json", perr.message);
        }

        auto cmd = root.at("cmd").as_string();
        spdlog::debug("cmd={}", cmd);

        if (cmd == "ping") {
                return "{\"ok\":true,\"pong\":true}";
        }
        if (cmd == "attach") {
                return handle_attach(pipe, root, policy_, ownership_);
        }
        if (cmd == "detach") {
                return handle_detach(pipe, root, policy_, ownership_);
        }
        if (cmd == "list") {
                return handle_list(pipe, root, policy_);
        }
        return error_response("unknown_cmd", std::string(cmd));
}

} // namespace usbip::broker
