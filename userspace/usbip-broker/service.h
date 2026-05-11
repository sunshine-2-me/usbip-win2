/*
 * Copyright (c) 2026
 */
#pragma once

#include <string>

namespace usbip::broker
{

inline constexpr wchar_t kServiceName[]    = L"usbip-broker";
inline constexpr wchar_t kServiceDisplay[] = L"USB/IP Per-User Broker";
inline constexpr wchar_t kServiceDesc[]    =
        L"Brokers per-user USB/IP attach/detach requests against the VHCI driver.";

// Run as a Windows service: hooks SCM, starts the pipe server, blocks until stop.
int run_service(int argc, wchar_t **argv);

// Run interactively in the console (for debugging). Same as service body without SCM.
// Pass argc/argv so --debug can enable spdlog::level::debug (MSVC Output window).
int run_console(int argc, wchar_t **argv);

// Service installation helpers, invoked from main on --install / --uninstall.
int install_service();
int uninstall_service();

} // namespace usbip::broker
