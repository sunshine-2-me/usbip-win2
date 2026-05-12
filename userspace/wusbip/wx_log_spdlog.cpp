/*
 * Copyright (c) 2023-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "wx_log_spdlog.h"

#include <spdlog\spdlog.h>

#include <wx\log.h>

#include <string_view>

namespace usbip
{

namespace
{

class wxLogSpdlog final : public wxLog
{
protected:
	void DoLogRecord(wxLogLevel level, const wxString &msg, const wxLogRecordInfo &) override;
};

void wxLogSpdlog::DoLogRecord(wxLogLevel level, const wxString &msg, const wxLogRecordInfo &)
{
	const wxScopedCharBuffer buf = msg.ToUTF8();
	const char *const data = buf.data();
	const auto len = static_cast<size_t>(buf.length());
	const std::string_view text(data ? data : "", len);

	spdlog::level::level_enum lv = spdlog::level::info;
	switch (level) {
	case wxLOG_FatalError:
	case wxLOG_Error:
		lv = spdlog::level::err;
		break;
	case wxLOG_Warning:
		lv = spdlog::level::warn;
		break;
	case wxLOG_Message:
	case wxLOG_Status:
		lv = spdlog::level::info;
		break;
	case wxLOG_Info:
		lv = spdlog::level::debug;
		break;
	case wxLOG_Debug:
	case wxLOG_Trace:
		lv = spdlog::level::debug;
		break;
	default:
		break;
	}

	spdlog::log(lv, "{}", text);
}

} // namespace

void install_wx_spdlog_chain()
{
	wxLog::SetActiveTarget(new wxLogChain(new wxLogSpdlog()));
}

} // namespace usbip
