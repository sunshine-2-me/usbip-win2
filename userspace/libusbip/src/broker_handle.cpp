/*
 * Copyright (c) 2026
 */
#include "broker_handle.h"
#include "output.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace usbip::broker_client
{

namespace
{

constexpr DWORD kMaxMessageBytes = 64 * 1024;

bool write_msg(HANDLE pipe, std::string_view msg)
{
        if (msg.size() > kMaxMessageBytes) {
                libusbip::output("broker: write_msg oversize {}", msg.size());
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
                return false;
        }
        DWORD written = 0;
        if (!WriteFile(pipe, msg.data(), DWORD(msg.size()), &written, nullptr)) {
                auto err = GetLastError();
                libusbip::output("broker: WriteFile failed #{} ({} / {} bytes)", err, written,
                                 msg.size());
                SetLastError(err);
                return false;
        }
        if (written != msg.size()) {
                SetLastError(ERROR_IO_INCOMPLETE);
                libusbip::output("broker: WriteFile partial write ({} / {} bytes)", written,
                                 msg.size());
                return false;
        }
        return true;
}

std::string read_msg(HANDLE pipe)
{
        std::string buf;
        buf.resize(8192);
        for (;;) {
                DWORD got = 0;
                if (ReadFile(pipe, buf.data(), DWORD(buf.size()), &got, nullptr)) {
                        buf.resize(got);
                        return buf;
                }
                auto err = GetLastError();
                if (err != ERROR_MORE_DATA) {
                        libusbip::output("broker: ReadFile failed #{}", err);
                        buf.clear();
                        return buf;
                }
                DWORD avail = 0;
                DWORD left  = 0;
                DWORD peek  = 0;
                PeekNamedPipe(pipe, nullptr, 0, &peek, &avail, &left);
                buf.resize(buf.size() + std::max<DWORD>(left, 1024));
                DWORD more = 0;
                if (!ReadFile(pipe, buf.data() + got,
                              DWORD(buf.size() - got), &more, nullptr)) {
                        libusbip::output("broker: ReadFile (MORE_DATA) failed #{}", GetLastError());
                        buf.clear();
                        return buf;
                }
                buf.resize(got + more);
                return buf;
        }
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
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                        out.push_back(c);
                }
        }
        out.push_back('"');
        return out;
}

// Tiny scanner to extract a string or integer field from a flat JSON object.
// Sufficient because the broker emits a known, simple schema. Returns false
// if the field is missing.
bool find_field(std::string_view json, std::string_view name, std::string_view *out_str = nullptr)
{
        std::string needle;
        needle.reserve(name.size() + 4);
        needle.push_back('"');
        needle += name;
        needle.push_back('"');

        auto p = json.find(needle);
        if (p == std::string_view::npos) return false;
        p = json.find(':', p + needle.size());
        if (p == std::string_view::npos) return false;
        ++p;
        while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
        if (p >= json.size()) return false;

        if (out_str) {
                if (json[p] == '"') {
                        ++p;
                        auto start = p;
                        while (p < json.size() && json[p] != '"') {
                                if (json[p] == '\\' && p + 1 < json.size()) ++p;
                                ++p;
                        }
                        *out_str = json.substr(start, p - start);
                } else {
                        auto start = p;
                        while (p < json.size() &&
                               json[p] != ',' && json[p] != '}' && json[p] != ']') {
                                ++p;
                        }
                        *out_str = json.substr(start, p - start);
                        // strip trailing whitespace
                        while (!out_str->empty() && (out_str->back() == ' ' || out_str->back() == '\t')) {
                                out_str->remove_suffix(1);
                        }
                }
        }
        return true;
}

bool is_ok(std::string_view resp)
{
        std::string_view ok;
        if (!find_field(resp, "ok", &ok)) return false;
        return ok == "true";
}

DWORD map_error_token(std::string_view tok)
{
        if (tok == "access_denied")        return ERROR_ACCESS_DENIED;
        if (tok == "invalid_request")      return ERROR_INVALID_PARAMETER;
        if (tok == "vhci_open_failed")     return ERROR_FILE_NOT_FOUND;
        if (tok == "ioctl_failed")         return ERROR_GEN_FAILURE;
        if (tok == "impersonation_failed") return ERROR_ACCESS_DENIED;
        if (tok == "identity_capture_failed") return ERROR_ACCESS_DENIED;
        if (tok == "unknown_cmd")          return ERROR_NOT_SUPPORTED;
        if (tok == "bad_json")             return ERROR_INVALID_DATA;
        if (tok == "oversize")             return ERROR_INSUFFICIENT_BUFFER;
        return ERROR_GEN_FAILURE;
}

void set_error_from_response(std::string_view resp)
{
        std::string_view err;
        if (find_field(resp, "error", &err)) {
                SetLastError(map_error_token(err));
        } else {
                SetLastError(ERROR_GEN_FAILURE);
        }
}

} // namespace

HANDLE open_pipe()
{
        for (;;) {
                auto h = CreateFileW(
                        kPipeName,
                        GENERIC_READ | GENERIC_WRITE,
                        0,
                        nullptr,
                        OPEN_EXISTING,
                        // SECURITY_IDENTIFICATION lets the broker read our SID.
                        // SECURITY_SQOS_PRESENT must accompany impersonation flags.
                        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                        nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                        DWORD mode = PIPE_READMODE_MESSAGE;
                        SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
                        libusbip::output("broker: named pipe opened (message mode)");
                        return h;
                }
                auto err = GetLastError();
                if (err != ERROR_PIPE_BUSY) {
                        libusbip::output("broker: CreateFile pipe failed #{}", err);
                        SetLastError(err);
                        return INVALID_HANDLE_VALUE;
                }
                if (!WaitNamedPipeW(kPipeName, 5000)) {
                        err = GetLastError();
                        libusbip::output("broker: WaitNamedPipe failed #{}", err);
                        SetLastError(err);
                        return INVALID_HANDLE_VALUE;
                }
        }
}

int attach(_In_ HANDLE pipe, _In_ const device_location &location, _In_ unsigned long options)
{
        std::string req = "{\"cmd\":\"attach\",\"host\":";
        req += json_escape(location.hostname);
        req += ",\"service\":";
        req += json_escape(location.service);
        req += ",\"busid\":";
        req += json_escape(location.busid);
        req += ",\"once\":";
        req += (options & vhci::ATTACH_ONCE) ? "true" : "false";
        req += "}";

        libusbip::output("broker: attach {}:{}/{}, once={}", location.hostname, location.service,
                         location.busid, (options & vhci::ATTACH_ONCE) != 0);

        if (!write_msg(pipe, req)) {
                return 0;
        }
        auto resp = read_msg(pipe);
        if (resp.empty()) {
                libusbip::output("broker: attach empty read (broken pipe)");
                SetLastError(ERROR_BROKEN_PIPE);
                return 0;
        }
        if (!is_ok(resp)) {
                std::string_view err{};
                find_field(resp, "error", &err);
                libusbip::output("broker: attach failed error='{}'",
                                 err.empty() ? std::string_view("?") : err);
                set_error_from_response(resp);
                return 0;
        }
        std::string_view port_sv;
        if (!find_field(resp, "port", &port_sv)) {
                libusbip::output("broker: attach ok but missing \"port\" in response");
                SetLastError(ERROR_BAD_FORMAT);
                return 0;
        }
        std::string port_str(port_sv);
        auto port = std::atoi(port_str.c_str());
        if (port <= 0) {
                libusbip::output("broker: attach invalid or zero port value '{}'", port_str);
                SetLastError(ERROR_BAD_FORMAT);
                return 0;
        }
        libusbip::output("broker: attach ok port={}", port);
        return port;
}

bool detach(_In_ HANDLE pipe, _In_ int port)
{
        char buf[64];
        std::snprintf(buf, sizeof(buf), "{\"cmd\":\"detach\",\"port\":%d}", port);

        libusbip::output("broker: detach port={}", port);

        if (!write_msg(pipe, buf)) {
                return false;
        }
        auto resp = read_msg(pipe);
        if (resp.empty()) {
                libusbip::output("broker: detach empty read (broken pipe)");
                SetLastError(ERROR_BROKEN_PIPE);
                return false;
        }
        if (!is_ok(resp)) {
                std::string_view err{};
                find_field(resp, "error", &err);
                libusbip::output("broker: detach failed port={} error='{}'",
                                 port, err.empty() ? std::string_view("?") : err);
                set_error_from_response(resp);
                return false;
        }
        libusbip::output("broker: detach ok port={}", port);
        return true;
}

namespace
{

// Extract an integer value at the field name from the current JSON object scope.
long long parse_int_field(std::string_view obj, std::string_view name, long long def = 0)
{
        std::string_view s;
        if (!find_field(obj, name, &s)) return def;
        std::string str(s);
        return std::atoll(str.c_str());
}

std::string parse_str_field(std::string_view obj, std::string_view name)
{
        std::string_view s;
        if (!find_field(obj, name, &s)) return {};
        return std::string(s);
}

} // namespace

std::optional<std::vector<imported_device>> get_imported_devices(_In_ HANDLE pipe)
{
        std::optional<std::vector<imported_device>> result;

        libusbip::output("broker: list imported devices");

        if (!write_msg(pipe, std::string_view("{\"cmd\":\"list\"}"))) {
                return result;
        }
        auto resp = read_msg(pipe);
        if (resp.empty()) {
                libusbip::output("broker: list empty read (broken pipe)");
                SetLastError(ERROR_BROKEN_PIPE);
                return result;
        }
        if (!is_ok(resp)) {
                std::string_view err{};
                find_field(resp, "error", &err);
                libusbip::output("broker: list failed error='{}'",
                                 err.empty() ? std::string_view("?") : err);
                set_error_from_response(resp);
                return result;
        }

        // Walk the "devices" array by hand; objects are flat.
        auto p = resp.find("\"devices\"");
        if (p == std::string::npos) {
                result.emplace();
                return result;
        }
        p = resp.find('[', p);
        if (p == std::string::npos) {
                result.emplace();
                return result;
        }
        ++p;

        std::vector<imported_device> devs;

        while (p < resp.size()) {
                while (p < resp.size() && (resp[p] == ' ' || resp[p] == ',' || resp[p] == '\t' || resp[p] == '\n')) {
                        ++p;
                }
                if (p >= resp.size() || resp[p] == ']') break;
                if (resp[p] != '{') break;

                int depth = 0;
                auto start = p;
                while (p < resp.size()) {
                        if (resp[p] == '{') ++depth;
                        else if (resp[p] == '}') {
                                if (--depth == 0) { ++p; break; }
                        }
                        ++p;
                }
                std::string_view obj(resp.data() + start, p - start);

                imported_device d;
                d.location.hostname = parse_str_field(obj, "host");
                d.location.service  = parse_str_field(obj, "service");
                d.location.busid    = parse_str_field(obj, "busid");
                d.port    = static_cast<int>(parse_int_field(obj, "port"));
                d.devid   = static_cast<UINT32>(parse_int_field(obj, "devid"));
                d.vendor  = static_cast<UINT16>(parse_int_field(obj, "vid"));
                d.product = static_cast<UINT16>(parse_int_field(obj, "pid"));
                devs.push_back(std::move(d));
        }

        result = std::move(devs);
        libusbip::output("broker: list ok count={}", result->size());
        return result;
}

} // namespace usbip::broker_client
