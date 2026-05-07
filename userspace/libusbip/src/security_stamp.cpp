/*
 * Copyright (c) 2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "security_stamp.h"
#include "output.h"

#define INITGUID
#include <initguid.h>
#include <cfgmgr32.h>
#include <devpkey.h>

#include <cwctype>
#include <string>
#include <vector>

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

namespace
{

using namespace usbip;

void to_upper_inplace(_Inout_ std::wstring &s)
{
        for (wchar_t &c : s) {
                c = static_cast<wchar_t>(std::towupper(c));
        }
}

bool substring_icase(_In_ std::wstring_view hay, _In_ const std::wstring &needle_upper)
{
        if (needle_upper.size() > hay.size()) {
                return false;
        }
        for (size_t i = 0; i <= hay.size() - needle_upper.size(); ++i) {
                bool match = true;
                for (size_t j = 0; j < needle_upper.size(); ++j) {
                        if (std::towupper(hay[i + j]) != needle_upper[j]) {
                                match = false;
                                break;
                        }
                }
                if (match) {
                        return true;
                }
        }
        return false;
}

DEVINST locate_usbip_emulated_controller()
{
        ULONG list_len{};
        if (::CM_Get_Device_ID_List_SizeW(&list_len, L"ROOT", CM_GETIDLIST_FILTER_ENUMERATOR) != CR_SUCCESS || !list_len) {
                return 0;
        }

        std::wstring list;
        list.resize(list_len);
        if (::CM_Get_Device_ID_ListW(L"ROOT", list.data(), list_len, CM_GETIDLIST_FILTER_ENUMERATOR) != CR_SUCCESS) {
                return 0;
        }

        for (PCWSTR p = list.data(); p && *p; p += wcslen(p) + 1) {
                if (wcsstr(p, L"USBIP_WIN2") && wcsstr(p, L"UDE")) {
                        DEVINST dn{};
                        if (::CM_Locate_DevNodeW(&dn, const_cast<wchar_t *>(p), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
                                return dn;
                        }
                }
        }

        return 0;
}

DEVINST find_root_hub(_In_ DEVINST vhci)
{
        DEVINST child{};
        if (::CM_Get_Child(&child, vhci, 0) != CR_SUCCESS) {
                return 0;
        }

        for (;;) {
                wchar_t id[MAX_DEVICE_ID_LEN]{};
                if (::CM_Get_Device_IDW(child, id, ARRAYSIZE(id), 0) == CR_SUCCESS) {
                        if (wcsstr(id, L"ROOT_HUB30") || wcsstr(id, L"ROOT_HUB20")) {
                                return child;
                        }
                }

                if (::CM_Get_Sibling(&child, child, 0) != CR_SUCCESS) {
                        break;
                }
        }

        return 0;
}

void collect_matching_descendants(
        _In_ DEVINST root,
        _In_ UINT16 vendor,
        _In_ UINT16 product,
        _Inout_ std::vector<DEVINST> &out)
{
        wchar_t vidpat[32]{};
        wchar_t pidpat[32]{};
        _snwprintf_s(vidpat, _TRUNCATE, L"VID_%04X", vendor);
        _snwprintf_s(pidpat, _TRUNCATE, L"PID_%04X", product);
        std::wstring vidu{vidpat};
        std::wstring pidu{pidpat};
        to_upper_inplace(vidu);
        to_upper_inplace(pidu);

        struct frame
        {
                DEVINST dn;
        };
        std::vector<frame> stack;
        stack.push_back({root});

        while (!stack.empty()) {
                auto cur = stack.back().dn;
                stack.pop_back();

                wchar_t id[MAX_DEVICE_ID_LEN]{};
                if (::CM_Get_Device_IDW(cur, id, ARRAYSIZE(id), 0) == CR_SUCCESS) {
                        std::wstring_view idv{id};
                        if (substring_icase(idv, vidu) && substring_icase(idv, pidu)) {
                                out.push_back(cur);
                        }
                }

                DEVINST c{};
                if (::CM_Get_Child(&c, cur, 0) == CR_SUCCESS) {
                        for (;;) {
                                stack.push_back({c});
                                if (::CM_Get_Sibling(&c, c, 0) != CR_SUCCESS) {
                                        break;
                                }
                        }
                }
        }
}

bool read_device_sds(_In_ DEVINST dn, _Out_ std::wstring &sds)
{
        ULONG type{};
        ULONG need{};
        CONFIGRET cr = ::CM_Get_DevNode_PropertyW(dn, &DEVPKEY_Device_SecuritySDS, &type, nullptr, &need, 0);

        if (cr == CR_NO_SUCH_VALUE || cr == CR_NO_SUCH_DEVINST) {
                sds.clear();
                return true;
        }

        if (cr != CR_BUFFER_SMALL || !need) {
                sds.clear();
                return false;
        }

        std::vector<wchar_t> buf(need / sizeof(wchar_t) + 2, L'\0');
        cr = ::CM_Get_DevNode_PropertyW(
                dn,
                &DEVPKEY_Device_SecuritySDS,
                &type,
                reinterpret_cast<PBYTE>(buf.data()),
                &need,
                0);
        if (cr != CR_SUCCESS) {
                sds.clear();
                return false;
        }

        if (type != DEVPROP_TYPE_STRING) {
                sds.clear();
                return false;
        }

        sds.assign(buf.data());
        return true;
}

bool stamp_one_devnode(_In_ DEVINST dn, _In_ const std::wstring &intended_sddl, _Inout_ std::wstring &error)
{
        std::wstring existing;
        if (!read_device_sds(dn, existing)) {
                error = L"read DEVPKEY_Device_SecuritySDS";
                return false;
        }

        if (!existing.empty() && _wcsicmp(existing.c_str(), intended_sddl.c_str()) != 0) {
                error = L"conflicting existing Device_SecuritySDS";
                return false;
        }

        if (!existing.empty()) {
                return true;
        }

        auto bytes = static_cast<ULONG>((intended_sddl.size() + 1) * sizeof(wchar_t));
        auto cr = ::CM_Set_DevNode_PropertyW(
                dn,
                &DEVPKEY_Device_SecuritySDS,
                DEVPROP_TYPE_STRING,
                reinterpret_cast<PBYTE>(const_cast<wchar_t *>(intended_sddl.c_str())),
                bytes,
                0);

        if (cr != CR_SUCCESS) {
                error = L"CM_Set_DevNode_PropertyW(Device_SecuritySDS)";
                return false;
        }

        return true;
}

void stamp_subtree_bfs(_In_ DEVINST root, _In_ const std::wstring &sddl, _Inout_ std::wstring &error, _Out_ bool &ok)
{
        std::vector<DEVINST> q{root};
        ok = true;

        for (size_t i = 0; i < q.size(); ++i) {
                auto dn = q[i];
                if (!stamp_one_devnode(dn, sddl, error)) {
                        ok = false;
                        return;
                }

                DEVINST c{};
                if (::CM_Get_Child(&c, dn, 0) == CR_SUCCESS) {
                        for (;;) {
                                q.push_back(c);
                                if (::CM_Get_Sibling(&c, c, 0) != CR_SUCCESS) {
                                        break;
                                }
                        }
                }
        }
}

} // namespace

namespace usbip::vhci
{

bool stamp_imported_device_devnode_security(
        _In_ int /*port*/, // reserved for future location-path correlation
        _In_ const std::wstring &sddl,
        _In_ const imported_device &dev,
        _Inout_ std::wstring &error)
{
        error.clear();

        if (sddl.empty()) {
                error = L"empty SDDL";
                return false;
        }

        constexpr int k_max_attempts = 48;
        constexpr DWORD k_delay_ms = 250;

        for (int attempt = 0; attempt < k_max_attempts; ++attempt) {
                auto vhci = locate_usbip_emulated_controller();
                if (!vhci) {
                        ::Sleep(k_delay_ms);
                        continue;
                }

                auto hub = find_root_hub(vhci);
                if (!hub) {
                        error = L"usbip root hub not found";
                        return false;
                }

                std::vector<DEVINST> matches;
                collect_matching_descendants(hub, dev.vendor, dev.product, matches);

                if (matches.empty()) {
                        ::Sleep(k_delay_ms);
                        continue;
                }

                bool all_ok = true;
                for (auto root_match : matches) {
                        bool sub_ok{};
                        stamp_subtree_bfs(root_match, sddl, error, sub_ok);
                        if (!sub_ok) {
                                all_ok = false;
                                break;
                        }
                }

                if (all_ok) {
                        return true;
                }

                if (!error.empty()) {
                        return false;
                }

                ::Sleep(k_delay_ms);
        }

        error = L"timeout enumerating devnodes for security stamp";
        return false;
}

} // namespace usbip::vhci
