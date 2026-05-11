/*
 * Copyright (c) 2026
 */
#include "service.h"
#include "attach_state.h"
#include "pipe_server.h"
#include "ownership.h"
#include "policy.h"
#include "volume_watcher.h"

#include <atomic>
#include <cwchar>

#include <cstdlib>

#include <windows.h>
#include <wtsapi32.h>

#include <common/log_format.h>
#include <common/log_paths.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/msvc_sink.h>

namespace usbip::broker
{

void log_rds_volume_automount_reminder()
{
        spdlog::info(
                "RDS/multi-session hosts: use installer task \"Disable global volume automount "
                "(mountvol /N)\" or run `mountvol /N` as Administrator so Windows does not assign "
                "the same removable volume in every user session; see README "
                "\"RDS / Explorer volume isolation\".");
}

namespace
{

SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS        g_status{};
HANDLE                g_stop_event = nullptr;
std::unique_ptr<Policy> g_policy;
std::unique_ptr<OwnershipStore> g_ownership;

void set_status(DWORD state, DWORD wait_hint = 0)
{
        g_status.dwCurrentState = state;
        g_status.dwWaitHint     = wait_hint;
        g_status.dwCheckPoint   = (state == SERVICE_RUNNING || state == SERVICE_STOPPED)
                ? 0 : g_status.dwCheckPoint + 1;
        if (state == SERVICE_START_PENDING) {
                g_status.dwControlsAccepted = 0;
        } else {
                g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
                                              | SERVICE_ACCEPT_PARAMCHANGE
                                              | SERVICE_ACCEPT_SESSIONCHANGE;
        }
        if (g_status_handle) {
                SetServiceStatus(g_status_handle, &g_status);
        }
}

DWORD WINAPI control_handler(DWORD ctrl, DWORD evt, LPVOID data, LPVOID /*ctx*/)
{
        switch (ctrl) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
                set_status(SERVICE_STOP_PENDING, 5000);
                if (g_stop_event) SetEvent(g_stop_event);
                return NO_ERROR;
        case SERVICE_CONTROL_PARAMCHANGE:
                spdlog::debug("SCM PARAMCHANGE: reloading policy");
                if (g_policy) g_policy->reload();
                return NO_ERROR;
        case SERVICE_CONTROL_SESSIONCHANGE:
                if (g_ownership && data &&
                    (evt == WTS_SESSION_LOGOFF || evt == WTS_REMOTE_DISCONNECT)) {
                        auto *notif = static_cast<const WTSSESSION_NOTIFICATION*>(data);
                        const DWORD ts_session = notif->dwSessionId;
                        for (const auto &[port, rec] :
                             AttachState::instance().list_by_session(ts_session)) {
                                (void)port;
                                if (rec.session_drive_letter != 0 && rec.session != 0) {
                                        unmount_session_drive_letter(rec.session,
                                                                    rec.session_drive_letter);
                                }
                        }
                        auto removed = g_ownership->release_by_session(ts_session);
                        auto attach_removed =
                                AttachState::instance().erase_by_session(ts_session);
                        if (removed) {
                                spdlog::info("released {} ownership record(s) for session {}",
                                             removed, ts_session);
                        }
                        if (attach_removed) {
                                spdlog::info("released {} attach-state record(s) for session {}",
                                             attach_removed, ts_session);
                        }
                }
                return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
                return NO_ERROR;
        }
        return ERROR_CALL_NOT_IMPLEMENTED;
}

bool env_requests_debug_logging()
{
        wchar_t *buf = nullptr;
        size_t n = 0;
        if (_wdupenv_s(&buf, &n, L"USBIP_BROKER_DEBUG") != 0) {
                return false;
        }
        if (!buf || !*buf) {
                std::free(buf);
                return false;
        }
        const auto ok =
                _wcsicmp(buf, L"1") == 0 || _wcsicmp(buf, L"true") == 0 || _wcsicmp(buf, L"yes") == 0;
        std::free(buf);
        return ok;
}

void install_logger(bool debug)
{
        try {
                auto dist = std::make_shared<spdlog::sinks::dist_sink_mt>();
                dist->add_sink(std::make_shared<spdlog::sinks::msvc_sink_mt>());
                const auto file_path = usbip::default_usbip_log_file("usbip-broker");
                if (!file_path.empty()) {
                        try {
                                dist->add_sink(std::make_shared<spdlog::sinks::basic_file_sink_mt>(file_path.string(), false));
                        } catch (...) {
                        }
                }
                auto logger = std::make_shared<spdlog::logger>("usbip-broker", dist);
                logger->set_pattern(usbip::log_format_spdlog_pattern(false));
                spdlog::set_default_logger(std::move(logger));
                auto level = debug ? spdlog::level::debug : spdlog::level::info;
                spdlog::set_level(level);
                spdlog::flush_on(level);
        } catch (...) {
                // logging is best-effort
        }
}

bool argv_requests_debug_logging(int argc, wchar_t **argv)
{
        for (int i = 1; i < argc; ++i) {
                if (std::wcscmp(argv[i], L"--debug") == 0 || std::wcscmp(argv[i], L"-d") == 0) {
                        return true;
                }
        }
        return false;
}

DWORD service_body()
{
        install_logger(env_requests_debug_logging());
        spdlog::info("usbip-broker starting");
        log_rds_volume_automount_reminder();
        if (spdlog::should_log(spdlog::level::debug)) {
                spdlog::debug(
                        "debug logging enabled (USBIP_BROKER_DEBUG=1/true/yes)");
        }

        g_policy = std::make_unique<Policy>();
        if (!g_policy->reload()) {
                spdlog::warn("policy not loaded; broker will deny all requests until reload");
        }
        g_ownership = std::make_unique<OwnershipStore>();
        if (!g_ownership->load()) {
                spdlog::warn("ownership state not loaded; starting with empty ownership map");
        }

        g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!g_stop_event) {
                return GetLastError();
        }

        VolumeWatcher vw;
        if (!vw.start()) {
                spdlog::warn("VolumeWatcher failed to start; per-session mount disabled");
        }

        PipeServer server(*g_policy, *g_ownership);
        auto rc = server.run(g_stop_event);

        vw.stop();

        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
        g_ownership.reset();
        g_policy.reset();

        spdlog::info("usbip-broker stopped rc={}", rc);
        spdlog::shutdown();
        return rc;
}

void WINAPI service_main(DWORD /*argc*/, wchar_t** /*argv*/)
{
        g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        g_status_handle = RegisterServiceCtrlHandlerExW(kServiceName, control_handler, nullptr);
        if (!g_status_handle) return;

        set_status(SERVICE_START_PENDING, 3000);
        set_status(SERVICE_RUNNING);

        auto rc = service_body();

        g_status.dwWin32ExitCode = rc;
        set_status(SERVICE_STOPPED);
}

} // namespace

int run_service(int /*argc*/, wchar_t** /*argv*/)
{
        SERVICE_TABLE_ENTRYW table[] = {
                { const_cast<LPWSTR>(kServiceName), service_main },
                { nullptr, nullptr }
        };
        if (!StartServiceCtrlDispatcherW(table)) {
                return static_cast<int>(GetLastError());
        }
        return 0;
}

int run_console(int argc, wchar_t **argv)
{
        const bool dbg =
                argv_requests_debug_logging(argc, argv) || env_requests_debug_logging();
        install_logger(dbg);
        spdlog::info("usbip-broker (console mode) starting");
        log_rds_volume_automount_reminder();
        if (dbg) {
                spdlog::info("debug logging enabled (--debug / USBIP_BROKER_DEBUG)");
        }

        Policy policy;
        if (!policy.reload()) {
                spdlog::warn("policy not loaded; broker will deny all requests until reload");
        }
        OwnershipStore ownership;
        if (!ownership.load()) {
                spdlog::warn("ownership state not loaded; starting with empty ownership map");
        }

        auto stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        SetConsoleCtrlHandler([](DWORD) -> BOOL {
                spdlog::info("console: stop requested");
                if (g_stop_event) SetEvent(g_stop_event);
                return TRUE;
        }, TRUE);
        g_stop_event = stop;

        VolumeWatcher vw;
        vw.start();

        PipeServer server(policy, ownership);
        auto rc = server.run(stop);

        vw.stop();
        CloseHandle(stop);
        g_stop_event = nullptr;

        spdlog::shutdown();
        return static_cast<int>(rc);
}

int install_service()
{
        wchar_t bin[MAX_PATH]{};
        GetModuleFileNameW(nullptr, bin, MAX_PATH);

        std::wstring quoted = L"\"";
        quoted += bin;
        quoted += L"\"";

        auto scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
        if (!scm) {
                std::fwprintf(stderr, L"OpenSCManager err=%lu\n", GetLastError());
                return 1;
        }

        auto svc = CreateServiceW(scm,
                                  kServiceName,
                                  kServiceDisplay,
                                  SERVICE_ALL_ACCESS,
                                  SERVICE_WIN32_OWN_PROCESS,
                                  SERVICE_AUTO_START,
                                  SERVICE_ERROR_NORMAL,
                                  quoted.c_str(),
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  L"LocalSystem",
                                  nullptr);
        if (!svc && GetLastError() == ERROR_SERVICE_EXISTS) {
                svc = OpenServiceW(scm, kServiceName,
                                   SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_QUERY_STATUS);
                if (svc && !ChangeServiceConfigW(svc,
                                                SERVICE_NO_CHANGE,
                                                SERVICE_NO_CHANGE,
                                                SERVICE_NO_CHANGE,
                                                quoted.c_str(),
                                                nullptr,
                                                nullptr,
                                                nullptr,
                                                nullptr,
                                                nullptr,
                                                nullptr)) {
                        std::fwprintf(stderr, L"ChangeServiceConfig err=%lu\n", GetLastError());
                        CloseServiceHandle(svc);
                        svc = nullptr;
                }
        }
        if (!svc) {
                auto err = GetLastError();
                std::fwprintf(stderr, L"CreateService err=%lu\n", err);
                CloseServiceHandle(scm);
                return 1;
        }

        SERVICE_DESCRIPTIONW desc{};
        desc.lpDescription = const_cast<LPWSTR>(kServiceDesc);
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

        StartServiceW(svc, 0, nullptr);

        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return 0;
}

int uninstall_service()
{
        auto scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
        if (!scm) {
                return 1;
        }
        auto svc = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
        if (!svc) {
                CloseServiceHandle(scm);
                return 1;
        }

        SERVICE_STATUS s{};
        ControlService(svc, SERVICE_CONTROL_STOP, &s);

        DeleteService(svc);
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return 0;
}

} // namespace usbip::broker
