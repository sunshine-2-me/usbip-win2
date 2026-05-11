/*
 * Copyright (c) 2026
 *
 * usbip-broker entry point.
 *
 * Usage:
 *   usbip-broker.exe                  -- runs as a Windows service (SCM-launched)
 *   usbip-broker.exe --console [--debug] -- console mode; --debug or USBIP_BROKER_DEBUG=1 enables spdlog::debug
 *   usbip-broker.exe --install        -- installs and starts the service
 *   usbip-broker.exe --uninstall      -- stops and removes the service
 */
#include "service.h"

#include <cwchar>
#include <cstdio>

#include <windows.h>

int wmain(int argc, wchar_t **argv)
{
        for (int i = 1; i < argc; ++i) {
                if (std::wcscmp(argv[i], L"--install") == 0) {
                        return ::usbip::broker::install_service();
                }
                if (std::wcscmp(argv[i], L"--uninstall") == 0) {
                        return ::usbip::broker::uninstall_service();
                }
                if (std::wcscmp(argv[i], L"--console") == 0) {
                        return ::usbip::broker::run_console(argc, argv);
                }
                if (std::wcscmp(argv[i], L"-h") == 0 ||
                    std::wcscmp(argv[i], L"--help") == 0) {
                        std::fputws(
                                L"usbip-broker.exe [--install|--uninstall|--console [--debug]]\n",
                                stderr);
                        return 0;
                }
        }
        return ::usbip::broker::run_service(argc, argv);
}
