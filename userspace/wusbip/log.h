/*
 * Copyright (c) 2023-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#pragma once

#include <wx/chartype.h>
#include <wx/log.h>
#include <wx/event.h>

class wxMenuItem;

enum { DEFAULT_LOGLEVEL = wxLOG_Status, VERBOSE_LOGLEVEL };

/*
 * Do not show dialog box for wxLOG_Info aka Verbose.
 */
class LogWindow : public wxEvtHandler, public wxLogWindow
{
public:
	LogWindow(
		_In_ wxWindow *parent, 
		_In_ const wxMenuItem *log_toogle,
		_In_ const wxMenuItem *font_inc,
		_In_ const wxMenuItem *font_decr,
		_In_ const wxMenuItem *font_dflt);

        int get_font_size() const;
        bool set_font_size(_In_ int pt);

private:
        wxTextCtrl *m_ctrl = do_get_control();

        wxTextCtrl *do_get_control();
        wxFont get_font() const;
        void change_font_size(_In_ int dir);

        void DoLogRecord(_In_ wxLogLevel level, _In_ const wxString &msg, _In_ const wxLogRecordInfo &info) override;

        void on_font_increase(_In_ wxCommandEvent &event);
	void on_font_decrease(_In_ wxCommandEvent &event);
	void on_font_default(_In_ wxCommandEvent &event);
	void on_mouse_wheel(_In_ wxMouseEvent &event);

	void set_accelerators(
		_In_ const wxMenuItem *log_toogle, 
		_In_ const wxMenuItem *font_incr, 
		_In_ const wxMenuItem *font_decr, 
		_In_ const wxMenuItem *font_dflt);
};

namespace usbip
{

/** spdlog (MSVC + default dated file under C:\\temp\\usbip\\logs) and libusbip::set_debug_output. Call before usbip::init. */
void init_wusbip_host_logging(int argc, wxChar **argv);

/** Routes wxLogError / wxLogMessage / etc. to spdlog (no wx log window). Call after init_wusbip_host_logging. */
void install_wx_log_for_spdlog();

void shutdown_wusbip_host_logging();

} // namespace usbip
