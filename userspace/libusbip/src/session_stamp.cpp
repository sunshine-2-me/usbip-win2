/*
 * Copyright (c) 2026
 */

#include "session_stamp.h"

#include "strconv.h"
#include "../vhci.h"
#include "output.h"

#include <usbip/vhci.h>

#include <initguid.h>
#include <devpkey.h>
#include <cfgmgr32.h>

#include <cwchar>
#include <optional>
#include <string>
#include <vector>

namespace
{

using namespace usbip;

constexpr int k_poll_attempts = 50;
constexpr DWORD k_poll_ms = 100;

std::wstring get_device_interface_path()
{
        auto guid = const_cast<GUID*>(&usbip::vhci::GUID_DEVINTERFACE_USB_HOST_CONTROLLER);
        std::wstring path;

        for (std::wstring multi_sz; true;) {

                ULONG cch{};
                if (auto err = CM_Get_Device_Interface_List_SizeW(&cch, guid, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT)) {
                        libusbip::output("CM_Get_Device_Interface_List_SizeW error #{}", err);
                        return path;
                }

                multi_sz.resize(cch);

                switch (auto err = CM_Get_Device_Interface_ListW(
                               guid, nullptr, multi_sz.data(), cch, CM_GET_DEVICE_INTERFACE_LIST_PRESENT)) {
                case CR_SUCCESS:
                        if (auto v = split_multi_sz(multi_sz); v.size() == 1) {
                                path = v.front();
                        } else {
                                libusbip::output("CM_Get_Device_Interface_List: {} paths", v.size());
                        }
                        return path;
                case CR_BUFFER_SMALL:
                        break;
                default:
                        libusbip::output("CM_Get_Device_Interface_List error #{}", err);
                        return path;
                }
        }
}

bool locate_controller_devinst(_Out_ DEVINST &out, _Inout_ std::wstring &error)
{
        auto path = get_device_interface_path();
        if (path.empty()) {
                error = L"VHCI interface path not found";
                return false;
        }

        DEVPROPTYPE ptype{};
        std::vector<wchar_t> id_buf(512);
        ULONG sz = ULONG(id_buf.size() * sizeof(wchar_t));

        auto cr = CM_Get_Device_Interface_PropertyW(
                path.c_str(),
                &DEVPKEY_Device_InstanceId,
                &ptype,
                reinterpret_cast<PBYTE>(id_buf.data()),
                &sz,
                0);

        if (cr == CR_BUFFER_SMALL) {
                id_buf.resize(sz / sizeof(wchar_t));
                sz = ULONG(id_buf.size() * sizeof(wchar_t));
                cr = CM_Get_Device_Interface_PropertyW(
                        path.c_str(),
                        &DEVPKEY_Device_InstanceId,
                        &ptype,
                        reinterpret_cast<PBYTE>(id_buf.data()),
                        &sz,
                        0);
        }

        if (cr != CR_SUCCESS || ptype != DEVPROP_TYPE_STRING) {
                error = L"CM_Get_Device_Interface_PropertyW(InstanceId) failed";
                return false;
        }

        if (sz < sizeof(wchar_t)) {
                error = L"empty instance id";
                return false;
        }

        id_buf.resize(sz / sizeof(wchar_t) - 1);

        DEVINST dn{};
        cr = CM_Locate_DevNodeW(&dn, id_buf.data(), CM_LOCATE_DEVNODE_NORMAL);
        if (cr != CR_SUCCESS) {
                error = L"CM_Locate_DevNodeW failed";
                return false;
        }

        out = dn;
        return true;
}

void enum_descendants(_In_ DEVINST root, _Inout_ std::vector<DEVINST> &out)
{
        out.push_back(root);

        DEVINST child{};
        if (CM_Get_Child(&child, root, 0) != CR_SUCCESS) {
                return;
        }

        for (;;) {
                enum_descendants(child, out);
                if (CM_Get_Sibling(&child, child, 0) != CR_SUCCESS) {
                        break;
                }
        }
}

std::vector<std::wstring> get_string_list_prop(_In_ DEVINST dn, _In_ const DEVPROPKEY &key)
{
        std::vector<std::wstring> result;
        DEVPROPTYPE type{};
        ULONG size = 0;

        auto cr = CM_Get_DevNode_PropertyW(dn, &key, &type, nullptr, &size, 0);
        if (cr != CR_BUFFER_SMALL && cr != CR_SUCCESS) {
                return result;
        }

        std::vector<BYTE> buf(size);
        cr = CM_Get_DevNode_PropertyW(dn, &key, &type, buf.data(), &size, 0);
        if (cr != CR_SUCCESS || (type != DEVPROP_TYPE_STRING_LIST && type != DEVPROP_TYPE_STRING)) {
                return result;
        }

        if (type == DEVPROP_TYPE_STRING) {
                if (size >= sizeof(wchar_t)) {
                        result.emplace_back(reinterpret_cast<wchar_t *>(buf.data()), size / sizeof(wchar_t) - 1);
                }
                return result;
        }

        for (const wchar_t *p = reinterpret_cast<const wchar_t *>(buf.data()); *p;) {
                result.emplace_back(p);
                p += result.back().size() + 1;
        }

        return result;
}

std::wstring get_string_prop(_In_ DEVINST dn, _In_ const DEVPROPKEY &key)
{
        auto v = get_string_list_prop(dn, key);
        return v.empty() ? std::wstring{} : std::move(v.front());
}

bool hardware_id_has_vid_pid(_In_ const std::wstring &s, _In_ unsigned vid, _In_ unsigned pid)
{
        wchar_t pat[32]{};
        swprintf_s(pat, L"VID_%04X&PID_%04X", vid, pid);
        return s.find(pat) != std::wstring::npos;
}

bool devnode_has_vid_pid(_In_ DEVINST dn, _In_ unsigned vid, _In_ unsigned pid)
{
        for (auto &hw : get_string_list_prop(dn, DEVPKEY_Device_HardwareIds)) {
                if (hardware_id_has_vid_pid(hw, vid, pid)) {
                        return true;
                }
        }
        return false;
}

std::optional<int> port_from_location(_In_ const std::wstring &loc)
{
        static constexpr wchar_t k_tag[] = L"Port_#";
        auto pos = loc.find(k_tag);
        if (pos == std::wstring::npos) {
                return std::nullopt;
        }

        pos += static_cast<std::wstring::difference_type>(wcslen(k_tag)); // first char after '#'

        if (pos + 4 > loc.size()) {
                return std::nullopt;
        }

        wchar_t quad[5]{};
        for (int i = 0; i < 4; ++i) {
                quad[i] = loc[pos + static_cast<size_t>(i)];
        }

        return static_cast<int>(wcstoul(quad, nullptr, 16));
}

std::optional<DEVINST> find_device_root(
        _In_ DEVINST controller_dn,
        _In_ int hub_port,
        _In_ unsigned vid,
        _In_ unsigned pid,
        _Inout_ std::wstring &error)
{
        std::vector<DEVINST> flat;
        enum_descendants(controller_dn, flat);

        std::vector<DEVINST> port_matches;

        for (auto dn : flat) {
                if (!devnode_has_vid_pid(dn, vid, pid)) {
                        continue;
                }

                auto loc = get_string_prop(dn, DEVPKEY_Device_LocationInfo);
                if (auto p = port_from_location(loc); p && *p == hub_port) {
                        port_matches.push_back(dn);
                }
        }

        if (port_matches.size() == 1) {
                return port_matches.front();
        }

        if (port_matches.empty()) {
                std::vector<DEVINST> vidpid_only;
                for (auto dn : flat) {
                        if (devnode_has_vid_pid(dn, vid, pid)) {
                                vidpid_only.push_back(dn);
                        }
                }

                if (vidpid_only.size() == 1) {
                        return vidpid_only.front();
                }

                error = L"Could not resolve USB device devnode (VID/PID/port)";
                return std::nullopt;
        }

        error = L"Ambiguous USB device devnode for VID/PID/port";
        return std::nullopt;
}

bool stamp_one_devinst(_In_ DEVINST dn, _In_ ULONG session_id, _Inout_ std::wstring &error)
{
        DEVPROPTYPE ptype{};
        ULONG session_existing{};
        ULONG sz = sizeof(session_existing);

        auto cr = CM_Get_DevNode_PropertyW(
                dn, &DEVPKEY_Device_SessionId, &ptype, reinterpret_cast<PBYTE>(&session_existing), &sz, 0);

        if (cr == CR_SUCCESS && ptype == DEVPROP_TYPE_UINT32) {
                if (session_existing == session_id) {
                        return true;
                }
                if (session_existing != 0 && session_existing != session_id) {
                        error = L"DEVPKEY_Device_SessionId already set to a different session";
                        return false;
                }
        }

        cr = CM_Set_DevNode_PropertyW(
                dn,
                &DEVPKEY_Device_SessionId,
                DEVPROP_TYPE_UINT32,
                reinterpret_cast<PBYTE>(&session_id),
                sizeof(session_id),
                0);

        if (cr != CR_SUCCESS) {
                error = L"CM_Set_DevNode_PropertyW(DEVPKEY_Device_SessionId) failed";
                return false;
        }

        return true;
}

bool stamp_subtree(_In_ DEVINST root, _In_ ULONG session_id, _Inout_ std::wstring &error)
{
        std::vector<DEVINST> nodes;
        enum_descendants(root, nodes);

        for (auto dn : nodes) {
                if (!stamp_one_devinst(dn, session_id, error)) {
                        return false;
                }
        }

        return true;
}

} // namespace

bool usbip::stamp_session_property(_In_ HANDLE vhci_dev, _In_ int port, _In_ unsigned long session_id, _Inout_ std::wstring &error)
{
        if (port <= 0) {
                error = L"invalid port";
                return false;
        }

        auto imported = vhci::get_imported_devices(vhci_dev);
        if (!imported) {
                error = L"get_imported_devices failed";
                return false;
        }

        unsigned vid{};
        unsigned pid{};
        bool found_row{};

        for (auto &d : *imported) {
                if (d.port == port) {
                        vid = d.vendor;
                        pid = d.product;
                        found_row = true;
                        break;
                }
        }

        if (!found_row) {
                error = L"imported device list has no matching port";
                return false;
        }

        DEVINST controller_dn{};
        if (!locate_controller_devinst(controller_dn, error)) {
                return false;
        }

        for (int attempt = 0; attempt < k_poll_attempts; ++attempt) {

                if (auto root = find_device_root(controller_dn, port, vid, pid, error)) {
                        if (!stamp_subtree(*root, session_id, error)) {
                                return false;
                        }

                        Sleep(200);

                        if (!stamp_subtree(*root, session_id, error)) {
                                return false;
                        }

                        return true;
                }

                error.clear();
                Sleep(k_poll_ms);
        }

        error = L"timeout waiting for USB device devnode";
        return false;
}
