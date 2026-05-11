/*
 * Copyright (c) 2026
 */
#include "pdo_post_attach.h"

#include "attach_state.h"
#include "filter_client.h"
#include "ownership.h"
#include "vhci_access.h"

#include <cwctype>
#include <stdio.h>
#include <string>
#include <vector>

#include <initguid.h>
#include <windows.h>
#include <cfgmgr32.h>
#include <devpkey.h>

#include <spdlog/spdlog.h>

#include <usbip/vhci.h>

#pragma comment(lib, "CfgMgr32.lib")

namespace usbip::broker
{

namespace
{

std::wstring instance_id_of(DEVINST devinst)
{
        ULONG cch = 0;
        if (CM_Get_Device_ID_Size(&cch, devinst, 0) != CR_SUCCESS) {
                return {};
        }
        std::wstring s(cch + 1, L'\0');
        if (CM_Get_Device_IDW(devinst, s.data(), static_cast<ULONG>(s.size()), 0) != CR_SUCCESS) {
                return {};
        }
        s.resize(wcslen(s.c_str()));
        return s;
}

bool wicontains(std::wstring_view haystack, std::wstring_view needle)
{
        if (needle.empty() || needle.size() > haystack.size()) {
                return false;
        }
        for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
                bool ok = true;
                for (std::size_t j = 0; j < needle.size(); ++j) {
                        if (std::towlower(haystack[i + j]) != std::towlower(needle[j])) {
                                ok = false;
                                break;
                        }
                }
                if (ok) {
                        return true;
                }
        }
        return false;
}

std::wstring busid_utf8_to_lower_wide(std::string_view busid)
{
        std::wstring w;
        w.reserve(busid.size());
        for (unsigned char c : busid) {
                if (c <= 127) {
                        w.push_back(static_cast<wchar_t>(std::tolower(static_cast<int>(c))));
                }
        }
        return w;
}

bool is_usb_instance_id(std::wstring_view id)
{
        return id.size() >= 4 && (id[0] == L'U' || id[0] == L'u') &&
               (id[1] == L'S' || id[1] == L's') && (id[2] == L'B' || id[2] == L'b') && id[3] == L'\\';
}

DEVINST devinst_for_first_vhci_interface()
{
        auto *guid = const_cast<GUID *>(&::usbip::vhci::GUID_DEVINTERFACE_USB_HOST_CONTROLLER);
        ULONG list_chars = 0;
        if (CM_Get_Device_Interface_List_SizeW(&list_chars, guid, nullptr,
                                               CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
                return 0;
        }
        std::wstring buf(list_chars, L'\0');
        if (CM_Get_Device_Interface_ListW(guid, nullptr, buf.data(), list_chars,
                                          CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
                return 0;
        }
        const wchar_t *first = buf.c_str();
        if (!*first) {
                return 0;
        }

        DEVPROPTYPE pt{};
        ULONG bytes = 0;
        CM_Get_Device_Interface_PropertyW(first, &DEVPKEY_Device_InstanceId, &pt, nullptr, &bytes,
                                            0);
        if (!bytes) {
                return 0;
        }
        std::wstring inst(bytes / sizeof(wchar_t) + 1, L'\0');
        if (CM_Get_Device_Interface_PropertyW(first, &DEVPKEY_Device_InstanceId, &pt,
                                                reinterpret_cast<PBYTE>(inst.data()), &bytes,
                                                0) != CR_SUCCESS) {
                return 0;
        }
        inst.resize(bytes / sizeof(wchar_t));

        DEVINST dn = 0;
        if (CM_Locate_DevNodeW(&dn, inst.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
                return 0;
        }
        return dn;
}

void collect_matching_usb_nodes(DEVINST cur,
                                std::wstring_view needle,
                                std::vector<std::wstring> *out)
{
        const auto id = instance_id_of(cur);
        if (!id.empty() && is_usb_instance_id(id) && wicontains(id, needle)) {
                out->push_back(id);
        }

        DEVINST child = 0;
        if (CM_Get_Child(&child, cur, 0) != CR_SUCCESS || !child) {
                return;
        }
        for (;;) {
                collect_matching_usb_nodes(child, needle, out);
                DEVINST next = 0;
                if (CM_Get_Sibling(&next, child, 0) != CR_SUCCESS || !next) {
                        break;
                }
                child = next;
        }
}

} // namespace

void try_register_filter_via_vhci_enumeration(int port,
                                              std::string_view busid,
                                              std::span<const std::uint8_t> sid_bytes,
                                              DWORD session_id)
{
        constexpr int kMaxAttempts = 24;

        const auto busid_w = busid_utf8_to_lower_wide(busid);
        const auto busid_norm = OwnershipStore::lowercase_ascii(busid);

        if (busid_w.empty()) {
                spdlog::warn(
                        "pdo_post_attach: busid has no ASCII letters/digits for substring match; "
                        "matching by VID/PID from driver only port={}",
                        port);
        }

        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
                if (attempt > 0) {
                        Sleep(45);
                }

                auto st = AttachState::instance().get(port);
                if (!st || OwnershipStore::lowercase_ascii(st->busid) != busid_norm) {
                        return;
                }
                if (!st->pdo_instance_id.empty()) {
                        return;
                }

                const DEVINST vhci_dn = devinst_for_first_vhci_interface();
                if (!vhci_dn) {
                        continue;
                }

                std::wstring vidpid_needle;
                if (auto vhci_h = vhci_access::open();
                    vhci_h != INVALID_HANDLE_VALUE) {
                        if (auto vp = vhci_access::imported_vid_pid_for_port(vhci_h, port)) {
                                wchar_t tmp[40]{};
                                _snwprintf_s(tmp, std::size(tmp), _TRUNCATE, L"VID_%04X&PID_%04X",
                                             static_cast<unsigned>(vp->first),
                                             static_cast<unsigned>(vp->second));
                                vidpid_needle = tmp;
                        }
                        CloseHandle(vhci_h);
                }

                std::vector<std::wstring> matches;
                if (!vidpid_needle.empty()) {
                        collect_matching_usb_nodes(vhci_dn, vidpid_needle, &matches);
                }
                if (matches.empty() && !busid_w.empty()) {
                        collect_matching_usb_nodes(vhci_dn, busid_w, &matches);
                }
                if (matches.empty()) {
                        continue;
                }
                if (matches.size() > 1) {
                        spdlog::warn(
                                "pdo_post_attach: ambiguous USB instance match (count={}) for "
                                "port={} busid='{}' (vidpid_needle empty={})",
                                matches.size(), port, busid, vidpid_needle.empty());
                        return;
                }

                const std::wstring &pdo_id = matches.front();
                AttachState::instance().set_pdo_instance(port, pdo_id);
                FilterClient::register_owner(pdo_id, sid_bytes, session_id);
                spdlog::info("pdo_post_attach: SET_OWNER via enumeration port={}", port);
                return;
        }

        if (auto st = AttachState::instance().get(port);
            st && st->pdo_instance_id.empty()
            && OwnershipStore::lowercase_ascii(st->busid) == busid_norm) {
                spdlog::warn(
                        "pdo_post_attach: filter SET_OWNER not applied after {} attempts port={} "
                        "busid='{}' (no unique USB\\\\ instance under VHCI; filter may stay "
                        "transparent)",
                        kMaxAttempts, port, busid);
        }
}

} // namespace usbip::broker
