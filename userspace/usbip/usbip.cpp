/*
 * Copyright (c) 2021-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "usbip.h"
#include "resource.h"

#include <libusbip\output.h>
#include <libusbip\win_handle.h>
#include <libusbip\win_socket.h>
#include <libusbip\format_message.h>
#include <libusbip\vhci.h>

#include <libusbip\src\usb_ids.h>
#include <libusbip\src\strconv.h>
#include <libusbip\src\file_ver.h>

#include <resources\messages.h>

#include <common/log_format.h>
#include <common/log_paths.h>
#include <common/process_elevation.h>

#include <spdlog\spdlog.h>
#include <spdlog\sinks\basic_file_sink.h>
#include <spdlog\sinks\dist_sink.h>
#include <spdlog\sinks\stdout_color_sinks.h>

#include <CLI11\CLI11.hpp>

#include <filesystem>
#include <optional>

#include <Windows.h>

namespace
{

using namespace usbip;

bool env_allows_direct_vhci() noexcept
{
	wchar_t buf[16];
	const DWORD n = GetEnvironmentVariableW(L"USBIP_ALLOW_DIRECT_VHCI", buf, static_cast<DWORD>(std::size(buf)));
	if (n == 0 || n >= std::size(buf)) {
		return false;
	}
	return _wcsicmp(buf, L"1") == 0 || _wcsicmp(buf, L"true") == 0 || _wcsicmp(buf, L"yes") == 0;
}

const auto MAX_HUB_PORTS = 255; // @see drivers/usbip_ude/vhci.cpp, set_usb_ports_cnt

auto get_ids_data()
{
	win::Resource r(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDR_USB_IDS), RT_RCDATA);
	assert(r);
	return r.str();
}

auto get_version()
{
        win::FileVersion fv;
        auto ver = fv.GetFileVersion();
        return wchar_to_utf8_or_errmsg(ver);
}

auto pack(command_t cmd, void *p) 
{
	return [cmd, p] { 
		if (!cmd(p)) {
			exit(EXIT_FAILURE); // throw CLI::RuntimeError(EXIT_FAILURE);
		}
	};
}

void add_cmd_attach(CLI::App &app)
{
	static attach_args r;

	auto cmd = app.add_subcommand("attach", "Attach remote/persistent USB device(s)")
		->callback(pack(cmd_attach, &r))
		->require_option(1);

	auto rem = cmd->add_option_group("Remote", "Attach remote USB device");

	rem->add_option("-r,--remote", r.remote, "Hostname/IP of a USB/IP server with exported USB devices")
		->required();	

	rem->add_option("-b,--bus-id", r.busid, "Bus Id of the USB device on a server")
		->required();	

	rem->add_flag("-t,--terse", r.terse, "Show port number as a result");

        auto stop = rem->add_flag("-x,--stop", r.stop, "Stop attach attempts to this device");
        rem->add_flag("--once", r.once, "Do not run automatic attach attempts if the command fails")->excludes(stop);

	cmd->add_option_group("Stop")
		->add_flag("-X,--stop-all", r.stop_all, "Stop all active attach attempts");

        cmd->add_option_group("Persistent", "Attach persistent USB device(s)")
                ->add_flag("-s,--stashed,--persistent", r.persistent, "Attach persistent device(s) stashed by 'port --stash'");
}

void add_cmd_detach(CLI::App &app)
{
	static detach_args r;

	auto cmd = app.add_subcommand("detach", "Detach a remote USB device")
		->callback(pack(cmd_detach, &r))
		->require_option(1);

	cmd->add_option("-p,--port", r.port, "Hub port number the device is plugged in")
		->check(CLI::Range(1, MAX_HUB_PORTS));

	cmd->add_flag("-a,--all", [&port = r.port] (auto) { port = -1; }, "Detach all devices");
}

void add_cmd_list(CLI::App &app)
{
	static list_args r;

	auto cmd = app.add_subcommand("list", "List exportable/persistent USB devices")
		->callback(pack(cmd_list, &r))
		->require_option(1);

	cmd->add_option_group("Remote", "List exportable USB devices")
		->add_option("-r,--remote", r.remote, "List exportable devices on a remote")
		->required();

	cmd->add_option_group("Persistent", "List persistent USB devices")
		->add_flag("-s,--stashed,--persistent", r.persistent, "List persistent devices stashed by 'port --stash'");
}

void add_cmd_port(CLI::App &app)
{
	static port_args r;

	auto cmd = app.add_subcommand("port", "Show/stash imported USB devices")
		->callback(pack(cmd_port, &r));

	cmd->add_flag("-s,--stash,--persistent", r.persistent,
		      "Devices listed by the command will be attached every time the driver is loaded (aka persistent devices)");
	
	cmd->add_option("number", r.ports, "Hub port number")
		->check(CLI::Range(1, MAX_HUB_PORTS))
		->expected(1, MAX_HUB_PORTS);
}

auto &msgtable_dll = L"resources"; // resource-only DLL that contains RT_MESSAGETABLE

auto& get_resource_module() noexcept
{
	static HModule mod(LoadLibraryEx(msgtable_dll, nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR));
	return mod;
}

std::optional<std::filesystem::path> parse_libusbip_log_path_arg(int argc, wchar_t **argv)
{
	for (int i = 1; i < argc; ++i) {
		if (std::wcscmp(argv[i], L"--libusbip-log") != 0) {
			continue;
		}
		if (i + 1 < argc && argv[i + 1][0] != L'\0' && argv[i + 1][0] != L'-') {
			return std::filesystem::path(argv[i + 1]);
		}
	}
	return std::nullopt;
}

void init_spdlog(int argc, wchar_t **argv)
{
	std::filesystem::path file_path;
	if (const auto o = parse_libusbip_log_path_arg(argc, argv)) {
		file_path = *o;
	} else {
		file_path = usbip::default_usbip_log_file("usbip");
	}

	auto dist = std::make_shared<spdlog::sinks::dist_sink_mt>();
	dist->add_sink(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());

	bool file_sink_added = false;
	if (!file_path.empty()) {
		try {
			dist->add_sink(std::make_shared<spdlog::sinks::basic_file_sink_mt>(file_path.string(), false));
			file_sink_added = true;
		} catch (...) {
			// best-effort: stderr only
		}
	}

	auto logger = std::make_shared<spdlog::logger>("usbip", dist);
	logger->set_pattern(usbip::log_format_spdlog_pattern(true));
	spdlog::set_default_logger(std::move(logger));

	if (file_sink_added) {
		spdlog::debug("usbip file log: {}", file_path.string());
	} else if (!file_path.empty()) {
		spdlog::debug("usbip file log unavailable (could not open: {})", file_path.string());
	} else {
		spdlog::debug("usbip file log disabled (empty path)");
	}

	libusbip::set_debug_output([](std::string msg) {
		if (auto lg = spdlog::default_logger()) {
			lg->log(spdlog::level::debug, "{}", msg);
		}
	});
}

void init(CLI::App &app)
{
	app.option_defaults()->always_capture_default();
	app.set_version_flag("-V,--version", get_version());

	app.add_flag("-d,--debug", 
		[] (auto) { spdlog::set_level(spdlog::level::debug); }, "Debug output");

	app.add_option("-t,--tcp-port", global_args.tcp_port, "TCP/IP port number of USB/IP server")
		->check(CLI::Range(1024, USHRT_MAX));

	add_cmd_attach(app);
	add_cmd_detach(app);
	add_cmd_list(app);
	add_cmd_port(app);

	app.require_subcommand(1);
}

auto run(int argc, wchar_t *argv[])
{
	init_spdlog(argc, argv);

	if (!env_allows_direct_vhci() && !usbip::process_is_elevated_admin()) {
		usbip::vhci::set_require_broker_only(true);
	}

	if (!get_resource_module()) {
		auto err = GetLastError();
		spdlog::critical(L"can't load '{}.dll', error {:#x} {}", msgtable_dll, err, wformat_message(err));
		return EXIT_FAILURE;
	}

	InitWinSock2 ws2;
	if (!ws2) {
		spdlog::critical("can't initialize Windows Sockets 2, {}", GetLastErrorMsg());
		return EXIT_FAILURE;
	}

	CLI::App app(
		"USB/IP client. Global: -d/--debug. By default opens VHCI only via usbip-broker "
		"(per-user isolation). Elevated processes skip that (direct VHCI). Set "
		"USBIP_ALLOW_DIRECT_VHCI=1 to allow direct VHCI when not elevated. libusbip lines share "
		"spdlog; default log "
		"file is C:\\temp\\usbip\\logs\\usbip-yyyymmdd.log. Use --libusbip-log <path> before the "
		"subcommand for a different file.");
	init(app);

	spdlog::debug("usbip parsing arguments");

	try {                                                                                                              
		app.parse(argc, argv);
	} catch (CLI::ParseError &e) {
		return app.exit(e);
	}

	if (const auto subs = app.get_subcommands(); !subs.empty()) {
		spdlog::debug("usbip finished subcommand '{}'", subs.front()->get_name());
	}

	return EXIT_SUCCESS;
}

} // namespace


std::string usbip::GetLastErrorMsg(unsigned long msg_id)
{
	static_assert(sizeof(msg_id) == sizeof(UINT32));
	static_assert(std::is_same_v<decltype(msg_id), DWORD>);

	if (msg_id == ~0UL) {
		msg_id = GetLastError();
	}

	auto &mod = get_resource_module();
	return format_message(mod.get(), msg_id);
}

const UsbIds& usbip::get_ids()
{
	static UsbIds ids(get_ids_data());
	assert(ids);
	return ids;
}

int wmain(int argc, wchar_t *argv[])
{
	auto ret = EXIT_FAILURE;

	try {
		ret = run(argc, argv);
	} catch (std::exception &e) {
		printf("exception: %s\n", e.what());
	}

	return ret;
}
