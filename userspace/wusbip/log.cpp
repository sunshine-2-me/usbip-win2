/*
 * Copyright (c) 2023-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "log.h"
#include "font.h"

#include <common/log_format.h>
#include <common/log_paths.h>

#include <libusbip/output.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/msvc_sink.h>

#include <filesystem>
#include <optional>
#include <string>

#include <wx/frame.h>
#include <wx/log.h>
#include <wx/settings.h>
#include <wx/persist/toplevel.h>
#include <wx/menuitem.h>
#include <wx/textctrl.h>
#include <wx/persist/toplevel.h>

namespace
{

void log_record_to_spdlog(wxLogLevel level, const wxString &msg)
{
        const std::string u8(msg.utf8_string());
        switch (level) {
        case wxLOG_FatalError:
        case wxLOG_Error:
                spdlog::error("{}", u8);
                break;
        case wxLOG_Warning:
                spdlog::warn("{}", u8);
                break;
        case wxLOG_Info:
        case wxLOG_Message:
        case wxLOG_Progress:
        case wxLOG_Status:
                spdlog::info("{}", u8);
                break;
        default:
                spdlog::debug("{}", u8);
                break;
        }
}

class wxLogSpdlog final : public wxLog
{
public:
        void DoLogRecord(wxLogLevel level, const wxString &msg, const wxLogRecordInfo &) override
        {
                log_record_to_spdlog(level, msg);
        }
};

} // namespace

LogWindow::LogWindow(
        _In_ wxWindow *parent, 
        _In_ const wxMenuItem *log_toggle,
        _In_ const wxMenuItem *font_incr,
        _In_ const wxMenuItem *font_decr,
        _In_ const wxMenuItem *font_dflt
) : 
        wxLogWindow(parent, _("Log records"), false)
{
        wxASSERT(log_toggle);
        wxASSERT(font_incr);
        wxASSERT(font_decr);
        wxASSERT(font_dflt);

        set_accelerators(log_toggle, font_incr, font_decr, font_dflt);
        auto wnd = GetFrame();

        wnd->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(LogWindow::on_font_increase), this, font_incr->GetId());
        wnd->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(LogWindow::on_font_decrease), this, font_decr->GetId());
        wnd->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(LogWindow::on_font_default), this, font_dflt->GetId());
        m_ctrl->Bind(wxEVT_MOUSEWHEEL, wxMouseEventHandler(LogWindow::on_mouse_wheel), this);

        wxPersistentRegisterAndRestore(wnd, wxString::FromAscii(__func__));

        if (wxSystemSettings::GetAppearance().IsDark()) {
                m_ctrl->SetDefaultStyle(*wxWHITE);
        }
}

wxTextCtrl* LogWindow::do_get_control()
{
        for (auto fr = GetFrame(); auto child: fr->GetChildren()) {
                if (auto ctrl = wxDynamicCast(child, wxTextCtrl)) {
                        return ctrl;
                }
        }

        wxASSERT(!"wxTextCtrl not found");
        return nullptr;
}

wxFont LogWindow::get_font() const
{
        auto &attr = m_ctrl->GetDefaultStyle();
        auto font = attr.GetFont();

        if (!font.IsOk()) { // assertion failure in wxWidgets during closing the app
                font = *wxNORMAL_FONT;
        }

        return font;
}

int LogWindow::get_font_size() const
{
        return get_font().GetPointSize();
}

bool LogWindow::set_font_size(_In_ int pt)
{
        wxTextAttr attr;
        attr.SetFontPointSize(pt);

        auto lines = m_ctrl->GetNumberOfLines();
        auto end = m_ctrl->XYToPosition(0, lines - 1);

        return m_ctrl->SetStyle(0, end, attr) && // for existing lines
               m_ctrl->SetDefaultStyle(attr); // for new lines
}

void LogWindow::set_accelerators(
        _In_ const wxMenuItem *log_toggle,
        _In_ const wxMenuItem *font_incr,
        _In_ const wxMenuItem *font_decr,
        _In_ const wxMenuItem *font_dflt)
{
        std::unique_ptr<wxAcceleratorEntry> toggle(log_toggle->GetAccel());
        std::unique_ptr<wxAcceleratorEntry> incr(font_incr->GetAccel());
        std::unique_ptr<wxAcceleratorEntry> decr(font_decr->GetAccel());
        std::unique_ptr<wxAcceleratorEntry> dflt(font_dflt->GetAccel());

        wxAcceleratorEntry entries[] { 
                { toggle->GetFlags(), toggle->GetKeyCode(), wxID_CLOSE }, 
                { incr->GetFlags(), incr->GetKeyCode(), font_incr->GetId() }, 
                { decr->GetFlags(), decr->GetKeyCode(), font_decr->GetId() }, 
                { dflt->GetFlags(), dflt->GetKeyCode(), font_dflt->GetId() }, 
        };

        wxAcceleratorTable table(std::size(entries), entries);
        GetFrame()->SetAcceleratorTable(table);
}

void LogWindow::DoLogRecord(_In_ wxLogLevel level, _In_ const wxString &msg, _In_ const wxLogRecordInfo &info)
{
        // wxLogWindow UI disabled — wusbip uses spdlog file only.
        // bool pass{};
        // auto verbose = level == wxLOG_Info;
        //
        // if (verbose) {
        //         pass = IsPassingMessages();
        //         PassMessages(false);
        // }
        //
        // wxLogWindow::DoLogRecord(level, msg, info);
        //
        // if (verbose) {
        //         PassMessages(pass);
        // }

        (void)info;
        log_record_to_spdlog(level, msg);
}

void LogWindow::change_font_size(_In_ int dir)
{
        auto font = get_font();
        usbip::change_font_size(font, dir);
        set_font_size(font.GetPointSize());
}

void LogWindow::on_font_increase(_In_ wxCommandEvent&)
{
        change_font_size(1);
}

void LogWindow::on_font_decrease(_In_ wxCommandEvent&)
{
        change_font_size(-1);
}

void LogWindow::on_font_default(_In_ wxCommandEvent&)
{
        change_font_size(0);
}

void LogWindow::on_mouse_wheel(_In_ wxMouseEvent &event)
{
        wxASSERT(event.GetEventObject() == m_ctrl);

        if (event.GetModifiers() == wxMOD_CONTROL) { // only Ctrl is depressed
                auto dir = event.GetWheelRotation();
                change_font_size(dir);
        }
}

namespace
{

std::optional<std::filesystem::path> parse_libusbip_log_path_wx(int argc, wxChar **argv)
{
        for (int i = 1; i < argc; ++i) {
                if (wxStrcmp(argv[i], wxS("--libusbip-log")) != 0) {
                        continue;
                }
                if (i + 1 < argc && argv[i + 1][0] && argv[i + 1][0] != wxT('-')) {
                        return std::filesystem::path(std::wstring(static_cast<const wchar_t *>(argv[i + 1])));
                }
        }
        return std::nullopt;
}

} // namespace

void usbip::init_wusbip_host_logging(int argc, wxChar **argv)
{
        std::filesystem::path file_path;
        if (const auto o = parse_libusbip_log_path_wx(argc, argv)) {
                file_path = *o;
        } else {
                file_path = usbip::default_usbip_log_file("wusbip");
        }

        auto dist = std::make_shared<spdlog::sinks::dist_sink_mt>();
        dist->add_sink(std::make_shared<spdlog::sinks::msvc_sink_mt>());
        if (!file_path.empty()) {
                try {
                        dist->add_sink(std::make_shared<spdlog::sinks::basic_file_sink_mt>(file_path.string(), false));
                } catch (...) {
                }
        }

        auto logger = std::make_shared<spdlog::logger>("wusbip", dist);
        logger->set_pattern(usbip::log_format_spdlog_pattern(true));
        spdlog::set_default_logger(std::move(logger));
        spdlog::set_level(spdlog::level::info);

        libusbip::set_debug_output([](std::string msg) {
                if (auto lg = spdlog::default_logger()) {
                        lg->log(spdlog::level::info, "{}", msg);
                }
        });
}

void usbip::install_wx_log_for_spdlog()
{
        delete wxLog::SetActiveTarget(new wxLogSpdlog);
}

void usbip::shutdown_wusbip_host_logging()
{
        libusbip::set_debug_output({});
        spdlog::shutdown();
}
