/*
 * Copyright (c) 2021-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "logging.h"

#include <libusbip\output.h>
#include <libusbip\src\strconv.h>

#include <spdlog\common.h>
/* This TU must emit info-level startup lines (command line) and debug (log path, libusbip hook);
 * project tweakme may set SPDLOG_ACTIVE_LEVEL above INFO and compile out spdlog::info. */
#ifdef SPDLOG_ACTIVE_LEVEL
#undef SPDLOG_ACTIVE_LEVEL
#endif
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG

#include <spdlog\spdlog.h>
#include <spdlog\sinks\basic_file_sink.h>
#include <spdlog\sinks\stdout_color_sinks.h>

#include <Windows.h>
#include <Lmcons.h>

#include <filesystem>
#include <vector>

namespace
{

void sanitize_for_spdlog_pattern(std::string &s)
{
	for (auto &c : s) {
		if (c == '%') {
			c = '_';
		}
	}
}

} // namespace

void libusbip::init_logging(const std::wstring_view log_file_stem, const bool include_stderr_sink)
{
	std::vector<spdlog::sink_ptr> sinks;
	bool file_sink_ok{};

	constexpr auto log_dir = LR"(C:\usbip\logs)";
	std::error_code fs_err;
	std::filesystem::create_directories(log_dir, fs_err);

	std::wstring stem(log_file_stem);
	if (stem.empty()) {
		wchar_t modpath[MAX_PATH]{};
		GetModuleFileNameW(nullptr, modpath, static_cast<DWORD>(std::size(modpath)));
		stem = std::filesystem::path{ modpath }.stem().wstring();
	}
	if (stem.empty()) {
		stem = L"usbip";
	}

	SYSTEMTIME st{};
	GetLocalTime(&st);
	/* Log file date stamp: yyyymmdd (local); same-day runs append to one file. */
	wchar_t ts[16]{};
	swprintf_s(ts, std::size(ts), L"%04u%02u%02u", st.wYear, st.wMonth, st.wDay);

	std::filesystem::path log_full;
	if (!fs_err) {
		try {
			log_full = std::filesystem::path{ log_dir } / (stem + L"_" + std::wstring(ts) + L".log");
#if defined(SPDLOG_WCHAR_FILENAMES)
			const spdlog::filename_t log_file = log_full.wstring();
#else
			const spdlog::filename_t log_file = usbip::wchar_to_utf8_or_errmsg(log_full.wstring());
#endif
			auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file, false);

			std::string user_utf8{ "unknown" };
			wchar_t userbuf[UNLEN + 1]{};
			DWORD user_len = UNLEN + 1;
			if (GetUserNameW(userbuf, &user_len) && user_len > 0) {
				/* API returns length including null terminator; exclude it from the pattern prefix. */
				user_utf8 = usbip::wchar_to_utf8_or_errmsg(std::wstring_view{ userbuf, user_len - 1 });
			}
			sanitize_for_spdlog_pattern(user_utf8);

			std::string file_pattern = user_utf8;
			file_pattern += " %Y-%m-%d %H:%M:%S.%e %l %v";
			file_sink->set_pattern(file_pattern);
			sinks.push_back(std::move(file_sink));
			file_sink_ok = true;
		} catch (...) {
		}
	}

	if (include_stderr_sink) {
		auto stderr_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
		stderr_sink->set_pattern("%^%l%$: %v");
		sinks.push_back(std::move(stderr_sink));
	}

	if (sinks.empty()) {
		auto stderr_fallback = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
		stderr_fallback->set_pattern("%^%l%$: %v");
		sinks.push_back(std::move(stderr_fallback));
	}

	auto logger = std::make_shared<spdlog::logger>(usbip::wchar_to_utf8_or_errmsg(stem), sinks.begin(), sinks.end());
	logger->set_level(spdlog::level::info);
	spdlog::set_default_logger(std::move(logger));

	if (const auto *cmdw = GetCommandLineW()) {
		spdlog::info("command line: {}", usbip::wchar_to_utf8_or_errmsg(std::wstring_view{ cmdw }));
	} else {
		spdlog::info("command line: <unavailable>");
	}
	spdlog::default_logger()->flush();

	if (file_sink_ok) {
		spdlog::debug("log file {} (append same calendar day)", usbip::wchar_to_utf8_or_errmsg(log_full.wstring()));
	}

	if (fs_err) {
		spdlog::error("could not create log directory {}: {}", usbip::wchar_to_utf8_or_errmsg(log_dir),
			       fs_err.message());
	} else if (!file_sink_ok) {
		spdlog::error("could not open a log file under {}", usbip::wchar_to_utf8_or_errmsg(log_dir));
	}

	spdlog::flush_on(spdlog::level::warn);

	using fn = void(const std::string &);
	fn &f = spdlog::debug;
	libusbip::set_debug_output(f);
}
