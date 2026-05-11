/*
 * Copyright (c) 2023-2025 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "app.h"
#include "log.h"
#include "utils.h"
#include "wusbip.h"

#include <wx/config.h>
#include <wx/utils.h>

#include <libusbip/src/file_ver.h>
#include <libusbip/vhci.h>

#include <common/process_elevation.h>

namespace
{

using namespace usbip;
        
auto read_appearance(_In_ const wchar_t *key)
{
        using enum wxApp::Appearance;
        enum { Sys = static_cast<long>(System) };

        auto &cfg = *wxConfig::Get();
        auto val = cfg.ReadLong(key, Sys);

        if (!(val >= Sys && val <= static_cast<long>(Dark))) {
                wxLogDebug(_("Invalid value %s=%d"), key, val);
                val = Sys;
        }

        return static_cast<wxApp::Appearance>(val);
}

auto init_mainframe(_In_ wxApp::Appearance app)
{
        wxString err;

        if (wxConfigBase *cfg = wxConfig::Get(false)) {
                cfg->SetStyle(wxCONFIG_USE_LOCAL_FILE); // matches MainFrame::init before reading prefs
        }

        if (auto read = usbip::init(err) ? vhci::open() : Handle()) {
                if (auto &frame = *new MainFrame(std::move(read), static_cast<int>(app)); frame.start_in_tray()) {
                        frame.iconize_to_tray();
                } else {
                        frame.Show();
                }
                return true;
        }

        if (err.empty()) {
                err = GetLastErrorMsg();
        }

        wxSafeShowMessage(_("Fatal error"), err);
        return false;
}

} // namespace


App::App()
{
        Bind(wxEVT_END_SESSION, &App::on_end_session, this);
}

bool App::OnInit()
{
        if (!wxApp::OnInit()) {
                return false;
        }

        usbip::init_wusbip_host_logging(argc, argv);
        usbip::install_wx_log_for_spdlog();

        {
                bool env_allow = false;
                wxString v;
                if (wxGetEnv(wxS("USBIP_ALLOW_DIRECT_VHCI"), &v)) {
                        v.Trim(true).Trim(false).MakeLower();
                        env_allow = (v == wxS("1") || v == wxS("true") || v == wxS("yes"));
                }
                if (!env_allow && !usbip::process_is_elevated_admin()) {
                        usbip::vhci::set_require_broker_only(true);
                }
        }

        set_names();

        auto app = set_appearance();
        return init_mainframe(app);
}

int App::OnExit()
{
        usbip::shutdown_wusbip_host_logging();
        return wxApp::OnExit();
}

void App::set_names()
{
        auto &v = win::get_file_version();

        SetAppName(wx_string(v.GetProductName()));
        SetVendorName(wx_string(v.GetCompanyName()));
}

auto App::set_appearance() -> Appearance
{
        auto app = read_appearance(m_appearance);

        if (auto res = SetAppearance(app); res != AppearanceResult::Ok) {
                wxLogError(_("SetAppearance(%d) error %d"), app, res);
        }

        return app;
}

void App::write_appearance(_In_ int val)
{
        if (auto &cfg = *wxConfig::Get(); !cfg.Write(m_appearance, val)) {
                wxLogError(_("Cannot write %s=%d"), m_appearance, val);
        }
}

/*
 * wxEVT_CLOSE_WINDOW will not be sent to MainFrame, but m_read_thread must be joined.
 * @see MainFrame::on_close
 */
void App::on_end_session(_In_ wxCloseEvent&)
{
        if (auto wnd = GetMainTopWindow()) {
                wnd->Close(true);
        }
}

wxIMPLEMENT_APP(App);
