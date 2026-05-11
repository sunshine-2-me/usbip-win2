/*
 * Copyright (c) 2026
 */
#include "volume_watcher.h"

#include "attach_state.h"
#include "filter_client.h"

#include <common/log_paths.h>

#include <process.h>

#include <array>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <initguid.h>
#include <windows.h>
#include <devpkey.h>
#include <dbt.h>
#include <cfgmgr32.h>
#include <wtsapi32.h>
#include <userenv.h>

#include <spdlog/spdlog.h>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "cfgmgr32.lib")

namespace usbip::broker
{

namespace detail
{

struct SpawnOutcome
{
        bool process_created = false;
        DWORD exit_code = STILL_ACTIVE;
};

SpawnOutcome spawn_in_session(DWORD session_id, std::wstring &cmdline_mut)
{
        SpawnOutcome out;
        HANDLE user_token = nullptr;
        if (!WTSQueryUserToken(session_id, &user_token)) {
                spdlog::warn("WTSQueryUserToken({}) err={}", session_id, GetLastError());
                return out;
        }

        LPVOID env = nullptr;
        CreateEnvironmentBlock(&env, user_token, FALSE);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

        PROCESS_INFORMATION pi{};

        BOOL ok = CreateProcessAsUserW(user_token,
                                       nullptr,
                                       cmdline_mut.data(),
                                       nullptr,
                                       nullptr,
                                       FALSE,
                                       CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                                       env,
                                       nullptr,
                                       &si,
                                       &pi);

        if (env) DestroyEnvironmentBlock(env);
        CloseHandle(user_token);

        if (!ok) {
                spdlog::warn("CreateProcessAsUser session={} err={}",
                             session_id, GetLastError());
                return out;
        }

        out.process_created = true;
        WaitForSingleObject(pi.hProcess, 30 * 1000);
        if (!GetExitCodeProcess(pi.hProcess, &out.exit_code)) {
                spdlog::warn("GetExitCodeProcess session={} err={}", session_id, GetLastError());
                out.exit_code = static_cast<DWORD>(-1);
        }
        spdlog::debug("spawn_in_session: session={} exit_code={}", session_id, out.exit_code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return out;
}

std::wstring read_first_letter_line(const std::wstring &path)
{
        // cmd.exe `echo X>` typically writes OEM/ANSI; read first byte/letter.
        std::ifstream in(std::filesystem::path(path), std::ios::binary);
        char ch = 0;
        if (!in.get(ch)) {
                return {};
        }
        const auto c = static_cast<unsigned char>(ch);
        if (c >= 'a' && c <= 'z') {
                return std::wstring(1, static_cast<wchar_t>(c - 'a' + L'A'));
        }
        if (c >= 'A' && c <= 'Z') {
                return std::wstring(1, static_cast<wchar_t>(c));
        }
        return {};
}

bool write_utf16_le_file(const std::wstring &path, std::wstring_view text)
{
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
                return false;
        }
        constexpr unsigned char bom[] = {0xFF, 0xFE};
        DWORD w = 0;
        if (!WriteFile(h, bom, sizeof(bom), &w, nullptr) || w != sizeof(bom)) {
                CloseHandle(h);
                return false;
        }
        for (wchar_t ch : text) {
                if (!WriteFile(h, &ch, sizeof(ch), &w, nullptr) || w != sizeof(ch)) {
                        CloseHandle(h);
                        return false;
                }
        }
        CloseHandle(h);
        return true;
}

// GUID_DEVINTERFACE_VOLUME. Must be mutable: CM_* APIs take LPGUID (non-const).
GUID kVolumeDeviceInterfaceGuid = {0x53F5630D, 0xB6BF, 0x11D0,
                                   {0x94, 0xF2, 0x00, 0xA0, 0xC9, 0x1E, 0xFB, 0x8B}};

std::wstring instance_id_of(DEVINST devinst)
{
        ULONG cch = 0;
        if (CM_Get_Device_ID_Size(&cch, devinst, 0) != CR_SUCCESS) {
                return {};
        }
        std::wstring s(cch + 1, L'\0');
        if (CM_Get_Device_IDW(devinst, s.data(), static_cast<ULONG>(s.size()), 0) != CR_SUCCESS) {
                return {};
        }
        s.resize(wcslen(s.c_str()));
        return s;
}

std::wstring find_usb_ancestor(DEVINST start)
{
        DEVINST cur = start;
        for (int depth = 0; depth < 16; ++depth) {
                auto id = instance_id_of(cur);
                if (id.empty()) {
                        break;
                }

                if (id.size() >= 4 && (id[0] == L'U' || id[0] == L'u')
                    && (id[1] == L'S' || id[1] == L's') && (id[2] == L'B' || id[2] == L'b')
                    && id[3] == L'\\') {
                        return id;
                }

                DEVINST parent = 0;
                if (CM_Get_Parent(&parent, cur, 0) != CR_SUCCESS || !parent) {
                        break;
                }
                cur = parent;
        }
        return {};
}

std::vector<std::wstring> list_volume_device_interface_paths()
{
        ULONG list_chars = 0;
        if (CM_Get_Device_Interface_List_SizeW(&list_chars, &kVolumeDeviceInterfaceGuid, nullptr,
                                               CM_GET_DEVICE_INTERFACE_LIST_PRESENT)
            != CR_SUCCESS) {
                return {};
        }
        std::wstring buf(list_chars, L'\0');
        if (CM_Get_Device_Interface_ListW(&kVolumeDeviceInterfaceGuid, nullptr, buf.data(),
                                          list_chars, CM_GET_DEVICE_INTERFACE_LIST_PRESENT)
            != CR_SUCCESS) {
                return {};
        }
        std::vector<std::wstring> out;
        for (auto *cur = buf.c_str(); *cur; cur += wcslen(cur) + 1) {
                out.emplace_back(cur);
        }
        return out;
}

DEVINST devinst_from_volume_path(std::wstring_view path)
{
        std::wstring p(path);
        while (!p.empty() && p.back() == L'\\') {
                p.pop_back();
        }

        for (const auto &iface : list_volume_device_interface_paths()) {
                if (_wcsicmp(iface.c_str(), p.c_str()) != 0) {
                        continue;
                }

                DEVPROPTYPE pt{};
                ULONG bytes = 0;
                CM_Get_Device_Interface_PropertyW(iface.c_str(), &DEVPKEY_Device_InstanceId, &pt,
                                                  nullptr, &bytes, 0);
                if (!bytes) {
                        continue;
                }
                std::wstring id(bytes / sizeof(wchar_t) + 1, L'\0');
                if (CM_Get_Device_Interface_PropertyW(iface.c_str(), &DEVPKEY_Device_InstanceId,
                                                       &pt, reinterpret_cast<PBYTE>(id.data()),
                                                       &bytes, 0)
                    != CR_SUCCESS) {
                        continue;
                }
                id.resize(bytes / sizeof(wchar_t));
                DEVINST devinst = 0;
                if (CM_Locate_DevNodeW(&devinst, id.data(), CM_LOCATE_DEVNODE_NORMAL)
                    == CR_SUCCESS) {
                        return devinst;
                }
        }
        return 0;
}

void mount_volume_in_owner_session(const AttachRecord &rec, std::wstring_view path)
{
        if (rec.session == 0) {
                return;
        }
        if (auto cur = AttachState::instance().get(rec.port)) {
                if (cur->session_drive_letter != 0) {
                        spdlog::debug("volume mount: skip port={} (session_drive_letter already "
                                      "set)",
                                      rec.port);
                        return;
                }
        }

        std::wstring volume_guid(path);
        if (!volume_guid.empty() && volume_guid.back() != L'\\') {
                volume_guid.push_back(L'\\');
        }
        std::wstring vol_mount_arg = volume_guid;
        while (!vol_mount_arg.empty()
               && (vol_mount_arg.back() == L'\\' || vol_mount_arg.back() == L'/')) {
                vol_mount_arg.pop_back();
        }

        const auto logdir = usbip::default_usbip_log_directory().wstring();
        const std::wstring letter_path =
                logdir + L"\\usbip_broker_mountletter_" + std::to_wstring(rec.port) + L".txt";
        const std::wstring err_path =
                logdir + L"\\usbip_broker_mounterr_" + std::to_wstring(rec.port) + L".txt";
        const std::wstring script_path =
                logdir + L"\\usbip_broker_domount_" + std::to_wstring(rec.port) + L".cmd";
        DeleteFileW(letter_path.c_str());
        DeleteFileW(err_path.c_str());
        DeleteFileW(script_path.c_str());

        std::wstring script;
        script.reserve(1024);
        script += L"@echo off\r\n";
        script += L"for %%L in (Z Y X W V U T S R Q P O N M L K J I H G F E D) do ";
        script += L"if not exist %%L:\\ (\r\n";
        script += L"  mountvol %%L: \"";
        script += vol_mount_arg;
        script += L"\"\r\n";
        script += L"  if errorlevel 1 goto :eof\r\n";
        script += L"  (echo %%L)>\"";
        script += letter_path;
        script += L"\"\r\n";
        script += L"  exit /b 0\r\n";
        script += L")\r\n";
        if (!write_utf16_le_file(script_path, script)) {
                spdlog::warn("volume mount: cannot write domount script port={}", rec.port);
                return;
        }

        if (auto cur2 = AttachState::instance().get(rec.port)) {
                if (cur2->session_drive_letter != 0) {
                        spdlog::debug("volume mount: skip spawn port={} (letter set after script "
                                      "write, likely concurrent arrival)",
                                      rec.port);
                        return;
                }
        }

        std::wstring cmd = L"cmd.exe /c \"";
        cmd += script_path;
        cmd += L"\" 2>\"";
        cmd += err_path;
        cmd += L"\"";

        spdlog::info("mounting volume in session {} port={}", rec.session, rec.port);

        auto sp = spawn_in_session(rec.session, cmd);
        if (!sp.process_created) {
                spdlog::warn("volume mount: spawn failed session={} port={}", rec.session,
                             rec.port);
                return;
        }
        if (sp.exit_code != 0) {
                spdlog::warn(
                        "volume mount: cmd exit={} session={} port={} (see mounterr file for port)",
                        sp.exit_code, rec.session, rec.port);
        } else {
                spdlog::info("volume mount: cmd exit=0 session={} port={}", rec.session, rec.port);
        }

        const auto letter_sv = read_first_letter_line(letter_path);
        if (letter_sv.size() == 1) {
                AttachState::instance().set_session_drive_letter(rec.port, letter_sv[0]);
        } else if (sp.exit_code == 0) {
                spdlog::warn(
                        "volume mount: success exit but could not read drive letter file port={}",
                        rec.port);
        }
}

} // namespace detail

bool unmount_session_drive_letter(DWORD session_id, wchar_t drive_letter)
{
        const wchar_t u = static_cast<wchar_t>(std::towupper(static_cast<wint_t>(drive_letter)));
        if (u < L'A' || u > L'Z') {
                return false;
        }
        std::wstring cmd =
                std::wstring(L"cmd.exe /c \"mountvol ") + u + L": /D\"";
        auto sp = detail::spawn_in_session(session_id, cmd);
        if (!sp.process_created) {
                return false;
        }
        if (sp.exit_code != 0) {
                spdlog::warn("unmount_session_drive_letter: mountvol /D exit={} session={} letter U+{:04X}",
                             sp.exit_code, session_id, static_cast<unsigned>(u));
                return false;
        }
        spdlog::info("unmount_session_drive_letter: session={} letter U+{:04X} ok", session_id,
                     static_cast<unsigned>(u));
        return true;
}

void try_mount_existing_volumes_for_port(int port)
{
        auto rec = AttachState::instance().get(port);
        if (!rec) {
                spdlog::debug("try_mount_existing_volumes_for_port: no attach record port={}",
                              port);
                return;
        }
        if (rec->session == 0) {
                spdlog::debug("try_mount_existing_volumes_for_port: session=0 skip port={}", port);
                return;
        }
        if (rec->session_drive_letter != 0) {
                spdlog::debug("try_mount_existing_volumes_for_port: already mounted port={} "
                              "letter_u={}",
                              port, static_cast<unsigned>(rec->session_drive_letter));
                return;
        }
        if (rec->pdo_instance_id.empty()) {
                spdlog::debug("try_mount_existing_volumes_for_port: no PDO yet port={}", port);
                return;
        }
        const auto paths = detail::list_volume_device_interface_paths();
        spdlog::debug("try_mount_existing_volumes_for_port: scanning {} volume path(s) port={}",
                      paths.size(), port);
        for (const auto &path : paths) {
                auto devinst = detail::devinst_from_volume_path(path);
                if (!devinst) {
                        continue;
                }
                DEVINST parent = 0;
                if (CM_Get_Parent(&parent, devinst, 0) != CR_SUCCESS || !parent) {
                        continue;
                }
                auto usb_id = detail::find_usb_ancestor(parent);
                if (usb_id.empty()) {
                        continue;
                }
                auto match = AttachState::instance().find_by_instance_prefix(usb_id);
                if (!match || match->port != port) {
                        continue;
                }
                spdlog::info("try_mount_existing_volumes_for_port: matched existing volume port={}",
                             port);
                detail::mount_volume_in_owner_session(*match, path);
                return;
        }
        spdlog::debug("try_mount_existing_volumes_for_port: no matching volume interface yet port={}",
                      port);
}

namespace
{

constexpr wchar_t kWindowClass[] = L"usbip_broker_volume_watcher";

void on_volume_arrival(std::wstring_view path)
{
        auto devinst = detail::devinst_from_volume_path(path);
        if (!devinst) {
                spdlog::debug("volume arrival: cannot resolve devinst for '{}' (truncated)",
                              "[wide]");
                return;
        }

        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, devinst, 0) != CR_SUCCESS || !parent) {
                spdlog::debug("volume arrival: CM_Get_Parent failed or empty");
                return;
        }

        auto usb_id = detail::find_usb_ancestor(parent);
        if (usb_id.empty()) {
                spdlog::debug("volume arrival: no USB\\ ancestor under volume parent");
                return;
        }

        auto rec = AttachState::instance().find_by_instance_prefix(usb_id);
        if (!rec) {
                rec = AttachState::instance().find_pending_for_usb_instance(usb_id);
        }
        if (!rec) {
                spdlog::debug("volume arrival: no attach record matches usb ancestor");
                return;
        }

        AttachState::instance().set_pdo_instance(rec->port, std::wstring(usb_id));
        FilterClient::register_owner(usb_id, rec->sid_bytes, rec->session);

        if (rec->session == 0) {
                spdlog::debug("volume arrival: owner has no session, skipping mount");
                return;
        }

        detail::mount_volume_in_owner_session(*rec, path);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
        if (msg == WM_DEVICECHANGE) {
                if (wparam == DBT_DEVICEARRIVAL) {
                        auto *hdr = reinterpret_cast<DEV_BROADCAST_HDR*>(lparam);
                        if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                                auto *iface = reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE_W*>(hdr);
                                std::wstring_view path(iface->dbcc_name);
                                spdlog::debug(
                                        "VolumeWatcher: DBT_DEVICEARRIVAL volume interface chars={}",
                                        path.size());
                                on_volume_arrival(path);
                        }
                }
                return TRUE;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

VolumeWatcher::VolumeWatcher() = default;

VolumeWatcher::~VolumeWatcher()
{
        stop();
}

bool VolumeWatcher::start()
{
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
                return true;
        }
        thread_ = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, &thread_main,
                                                         this, 0, &thread_id_));
        if (!thread_) {
                running_ = false;
                return false;
        }
        return true;
}

void VolumeWatcher::stop()
{
        if (!running_.exchange(false)) return;
        if (worker_tid_) {
                PostThreadMessageW(worker_tid_, WM_QUIT, 0, 0);
        }
        if (thread_) {
                WaitForSingleObject(thread_, 5000);
                CloseHandle(thread_);
                thread_ = nullptr;
        }
}

unsigned __stdcall VolumeWatcher::thread_main(void *self)
{
        static_cast<VolumeWatcher*>(self)->run();
        return 0;
}

void VolumeWatcher::run()
{
        worker_tid_ = GetCurrentThreadId();

        WNDCLASSW wc{};
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance   = GetModuleHandleW(nullptr);
        wc.lpszClassName = kWindowClass;
        RegisterClassW(&wc);

        hwnd_ = CreateWindowExW(0, kWindowClass, kWindowClass, 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (!hwnd_) {
                spdlog::warn("VolumeWatcher: CreateWindow failed err={}", GetLastError());
                return;
        }

        DEV_BROADCAST_DEVICEINTERFACE_W filter{};
        filter.dbcc_size       = sizeof(filter);
        filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        filter.dbcc_classguid  = detail::kVolumeDeviceInterfaceGuid;

        notify_ = RegisterDeviceNotificationW(hwnd_, &filter,
                                              DEVICE_NOTIFY_WINDOW_HANDLE);
        if (!notify_) {
                spdlog::warn("VolumeWatcher: RegisterDeviceNotification err={}", GetLastError());
                DestroyWindow(hwnd_);
                hwnd_ = nullptr;
                return;
        }

        spdlog::info("VolumeWatcher: running");

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
        }

        if (notify_) {
                UnregisterDeviceNotification(notify_);
                notify_ = nullptr;
        }
        if (hwnd_) {
                DestroyWindow(hwnd_);
                hwnd_ = nullptr;
        }
        UnregisterClassW(kWindowClass, GetModuleHandleW(nullptr));

        spdlog::info("VolumeWatcher: stopped");
}

} // namespace usbip::broker
