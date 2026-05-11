/*
 * Copyright (c) 2026
 */
#include "volume_watcher.h"

#include "attach_state.h"
#include "filter_client.h"

#include <process.h>

#include <array>
#include <cwctype>
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

namespace
{

constexpr wchar_t kWindowClass[] = L"usbip_broker_volume_watcher";

GUID kVolumeGuid = { 0x53F5630D, 0xB6BF, 0x11D0,
                    { 0x94, 0xF2, 0x00, 0xA0, 0xC9, 0x1E, 0xFB, 0x8B } };

// Resolve the device-instance-id of @p devinst.
std::wstring instance_id_of(DEVINST devinst)
{
        ULONG cch = 0;
        if (CM_Get_Device_ID_Size(&cch, devinst, 0) != CR_SUCCESS) {
                return {};
        }
        std::wstring s(cch + 1, L'\0');
        if (CM_Get_Device_IDW(devinst, s.data(),
                              static_cast<ULONG>(s.size()), 0) != CR_SUCCESS) {
                return {};
        }
        s.resize(wcslen(s.c_str()));
        return s;
}

// Walk up from @p start until we reach a node whose instance id starts with
// "USB\\" (heuristic: matches the bus-emulated child PDOs created by
// usbip2_ude / udecx). Returns empty if no such ancestor is found.
std::wstring find_usb_ancestor(DEVINST start)
{
        DEVINST cur = start;
        for (int depth = 0; depth < 16; ++depth) {
                auto id = instance_id_of(cur);
                if (id.empty()) break;

                if (id.size() >= 4 &&
                    (id[0] == L'U' || id[0] == L'u') &&
                    (id[1] == L'S' || id[1] == L's') &&
                    (id[2] == L'B' || id[2] == L'b') &&
                    id[3] == L'\\') {
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

// Resolve a DEV_BROADCAST_DEVICEINTERFACE notification to the volume's
// devnode (DEVINST). Returns 0 on failure.
DEVINST devinst_from_volume_path(std::wstring_view path)
{
        // Strip "\\?\" prefix if present so we end up with a device id we can
        // pass to CM_Locate_DevNode.
        std::wstring p(path);
        // Some volume paths end with a backslash, trim it.
        while (!p.empty() && p.back() == L'\\') {
                p.pop_back();
        }

        // Walk all volume devnodes (small set) and match by interface path.
        ULONG list_chars = 0;
        if (CM_Get_Device_Interface_List_SizeW(&list_chars, &kVolumeGuid, nullptr,
                                               CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
                return 0;
        }
        std::wstring buf(list_chars, L'\0');
        if (CM_Get_Device_Interface_ListW(&kVolumeGuid, nullptr, buf.data(), list_chars,
                                          CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
                return 0;
        }

        for (auto *cur = buf.c_str(); *cur; cur += wcslen(cur) + 1) {
                if (_wcsicmp(cur, p.c_str()) != 0) {
                        continue;
                }

                DEVPROPTYPE pt{};
                ULONG bytes = 0;
                CM_Get_Device_Interface_PropertyW(cur, &DEVPKEY_Device_InstanceId,
                                                  &pt, nullptr, &bytes, 0);
                if (bytes) {
                        std::wstring id(bytes / sizeof(wchar_t) + 1, L'\0');
                        if (CM_Get_Device_Interface_PropertyW(cur, &DEVPKEY_Device_InstanceId,
                                                             &pt,
                                                             reinterpret_cast<PBYTE>(id.data()),
                                                             &bytes, 0)
                            == CR_SUCCESS) {
                                id.resize(bytes / sizeof(wchar_t));
                                DEVINST devinst = 0;
                                if (CM_Locate_DevNodeW(&devinst, id.data(),
                                                       CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
                                        return devinst;
                                }
                        }
                }
        }

        (void)list_chars;
        return 0;
}

bool spawn_in_session(DWORD session_id, std::wstring_view cmdline)
{
        HANDLE user_token = nullptr;
        if (!WTSQueryUserToken(session_id, &user_token)) {
                spdlog::warn("WTSQueryUserToken({}) err={}", session_id, GetLastError());
                return false;
        }

        LPVOID env = nullptr;
        CreateEnvironmentBlock(&env, user_token, FALSE);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

        PROCESS_INFORMATION pi{};
        std::wstring mut(cmdline);

        BOOL ok = CreateProcessAsUserW(user_token,
                                       nullptr,
                                       mut.data(),
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
                return false;
        }

        WaitForSingleObject(pi.hProcess, 30 * 1000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
}

void on_volume_arrival(std::wstring_view path)
{
        auto devinst = devinst_from_volume_path(path);
        if (!devinst) {
                spdlog::debug("volume arrival: cannot resolve devinst for '{}' (truncated)",
                              "[wide]");
                return;
        }

        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, devinst, 0) != CR_SUCCESS || !parent) {
                return;
        }

        auto usb_id = find_usb_ancestor(parent);
        if (usb_id.empty()) {
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

        // Get a free drive letter inside the user's session.
        // Strategy: ask `mountvol <letter>: <volume_guid>\` to attach the
        // letter. We probe Z..D for the first that is free in the user
        // session (we run the probe inside the session via cmd.exe).
        std::wstring volume_guid(path);
        if (!volume_guid.empty() && volume_guid.back() != L'\\') {
                volume_guid.push_back(L'\\');
        }

        // Compose a command that finds a free letter and mounts.
        // for %L in (Z Y X W V U T S R Q P O N M L K J I H G F E D) do
        //   if not exist %L:\ ( mountvol %L: <volume> & exit /b )
        std::wstring cmd = L"cmd.exe /c \"";
        cmd += L"for %L in (Z Y X W V U T S R Q P O N M L K J I H G F E D) do "
               L"if not exist %L:\\ ( mountvol %L: \"";
        cmd += volume_guid;
        cmd += L"\" && exit /b 0 )\"";

        spdlog::info("mounting volume in session {} for owner SID len {}",
                     rec->session, rec->sid_bytes.size());

        spawn_in_session(rec->session, cmd);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
        if (msg == WM_DEVICECHANGE) {
                if (wparam == DBT_DEVICEARRIVAL) {
                        auto *hdr = reinterpret_cast<DEV_BROADCAST_HDR*>(lparam);
                        if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                                auto *iface = reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE_W*>(hdr);
                                std::wstring_view path(iface->dbcc_name);
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
        filter.dbcc_classguid  = kVolumeGuid;

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
