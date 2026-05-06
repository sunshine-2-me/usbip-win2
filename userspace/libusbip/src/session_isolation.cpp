#include "session_isolation.h"

#include "output.h"
#include "setupapi.h"

#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <processthreadsapi.h>
#include <SetupAPI.h>
#include <usbip/vhci.h>

#include <cwchar>
#include <optional>
#include <string>
#include <vector>

namespace
{

using namespace usbip;

std::optional<DWORD> get_current_session_id() noexcept
{
        DWORD session_id = 0;
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) {
                return std::nullopt;
        }

        return session_id;
}

std::optional<std::wstring> get_interface_path() noexcept
{
        auto guid = const_cast<GUID*>(&vhci::GUID_DEVINTERFACE_USB_HOST_CONTROLLER);

        for (std::wstring multi_sz;;) {
                ULONG cch = 0;
                auto err = CM_Get_Device_Interface_List_SizeW(
                        &cch,
                        guid,
                        nullptr,
                        CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
                if (err != CR_SUCCESS) {
                        SetLastError(CM_MapCrToWin32Err(err, ERROR_INVALID_PARAMETER));
                        return std::nullopt;
                }

                multi_sz.resize(cch);
                err = CM_Get_Device_Interface_ListW(
                        guid,
                        nullptr,
                        multi_sz.data(),
                        cch,
                        CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
                if (err == CR_BUFFER_SMALL) {
                        continue;
                }
                if (err != CR_SUCCESS) {
                        SetLastError(CM_MapCrToWin32Err(err, ERROR_NOT_ENOUGH_MEMORY));
                        return std::nullopt;
                }

                const wchar_t* first = multi_sz.c_str();
                if (*first == L'\0') {
                        SetLastError(ERROR_NOT_FOUND);
                        return std::nullopt;
                }

                std::wstring path(first);
                const wchar_t* second = first + path.size() + 1;
                if (*second != L'\0') {
                        SetLastError(ERROR_GEN_FAILURE);
                        return std::nullopt;
                }

                return path;
        }
}

std::optional<std::wstring> get_devinst_id(_In_ DEVINST devinst) noexcept
{
        ULONG chars = 0;
        auto cr = CM_Get_Device_ID_Size(&chars, devinst, 0);
        if (cr != CR_SUCCESS) {
                SetLastError(CM_MapCrToWin32Err(cr, ERROR_NOT_FOUND));
                return std::nullopt;
        }

        std::wstring id(chars + 1, L'\0');
        cr = CM_Get_Device_IDW(devinst, id.data(), chars + 1, 0);
        if (cr != CR_SUCCESS) {
                SetLastError(CM_MapCrToWin32Err(cr, ERROR_NOT_FOUND));
                return std::nullopt;
        }

        id.resize(chars);
        return id;
}

std::optional<std::wstring> get_device_instance_id(
        _In_ HDEVINFO info,
        _Inout_ SP_DEVINFO_DATA& dev_data) noexcept
{
        std::vector<wchar_t> buffer(512);

        for (;;) {
                DWORD required = 0;
                if (SetupDiGetDeviceInstanceIdW(
                            info,
                            &dev_data,
                            buffer.data(),
                            static_cast<DWORD>(buffer.size()),
                            &required)) {
                        return std::wstring(buffer.data());
                }

                auto err = GetLastError();
                if (err != ERROR_INSUFFICIENT_BUFFER) {
                        SetLastError(err);
                        return std::nullopt;
                }

                if (required == 0) {
                        SetLastError(ERROR_INVALID_DATA);
                        return std::nullopt;
                }

                buffer.resize(required);
        }
}

std::optional<std::wstring> get_vhci_instance_id() noexcept
{
        auto path = get_interface_path();
        if (!path) {
                return std::nullopt;
        }

        hdevinfo devs(SetupDiGetClassDevsW(
                &vhci::GUID_DEVINTERFACE_USB_HOST_CONTROLLER,
                nullptr,
                nullptr,
                DIGCF_DEVICEINTERFACE | DIGCF_PRESENT));
        if (!devs) {
                return std::nullopt;
        }

        SP_DEVICE_INTERFACE_DATA if_data{};
        if_data.cbSize = sizeof(if_data);
        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devs.get(), nullptr,
                        &vhci::GUID_DEVINTERFACE_USB_HOST_CONTROLLER, i, &if_data); ++i) {
                DWORD needed = 0;
                (void)SetupDiGetDeviceInterfaceDetailW(devs.get(), &if_data, nullptr, 0, &needed, nullptr);
                if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
                        continue;
                }

                std::vector<BYTE> detail_storage(needed);
                auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detail_storage.data());
                detail->cbSize = sizeof(*detail);

                SP_DEVINFO_DATA dev_data{};
                dev_data.cbSize = sizeof(dev_data);
                if (!SetupDiGetDeviceInterfaceDetailW(
                            devs.get(),
                            &if_data,
                            detail,
                            needed,
                            nullptr,
                            &dev_data)) {
                        continue;
                }

                if (_wcsicmp(detail->DevicePath, path->c_str()) != 0) {
                        continue;
                }

                auto instance_id = get_device_instance_id(devs.get(), dev_data);
                if (!instance_id) {
                        return std::nullopt;
                }

                return instance_id;
        }

        auto err = GetLastError();
        if (err == ERROR_NO_MORE_ITEMS) {
                err = ERROR_NOT_FOUND;
        }
        SetLastError(err);
        return std::nullopt;
}

bool has_ancestor(_In_ DEVINST child, _In_ const std::wstring& ancestor_instance_id) noexcept
{
        DEVINST current = child;

        for (int depth = 0; depth < 32; ++depth) {
                DEVINST parent = 0;
                auto cr = CM_Get_Parent(&parent, current, 0);
                if (cr == CR_NO_SUCH_DEVNODE) {
                        return false;
                }
                if (cr != CR_SUCCESS) {
                        SetLastError(CM_MapCrToWin32Err(cr, ERROR_NOT_FOUND));
                        return false;
                }

                auto parent_id = get_devinst_id(parent);
                if (!parent_id) {
                        return false;
                }

                if (_wcsicmp(parent_id->c_str(), ancestor_instance_id.c_str()) == 0) {
                        return true;
                }

                current = parent;
        }

        SetLastError(ERROR_GEN_FAILURE);
        return false;
}

bool try_read_int32_property(_In_ HDEVINFO info, _Inout_ SP_DEVINFO_DATA& dev_data, _In_ const DEVPROPKEY& key, _Out_ LONG& value) noexcept
{
        DEVPROPTYPE type = DEVPROP_TYPE_EMPTY;
        if (!SetupDiGetDevicePropertyW(
                    info,
                    &dev_data,
                    &key,
                    &type,
                    reinterpret_cast<PBYTE>(&value),
                    sizeof(value),
                    nullptr,
                    0)) {
                return false;
        }

        return type == DEVPROP_TYPE_INT32;
}

bool set_and_verify_session_id(
        _In_ HDEVINFO info,
        _Inout_ SP_DEVINFO_DATA& dev_data,
        _In_ DWORD session_id) noexcept
{
        if (!SetupDiSetDevicePropertyW(
                    info,
                    &dev_data,
                    &DEVPKEY_Device_SessionId,
                    DEVPROP_TYPE_UINT32,
                    reinterpret_cast<const PBYTE>(&session_id),
                    sizeof(session_id),
                    0)) {
                return false;
        }

        DEVPROPTYPE type = DEVPROP_TYPE_EMPTY;
        DWORD actual = 0;
        if (!SetupDiGetDevicePropertyW(
                    info,
                    &dev_data,
                    &DEVPKEY_Device_SessionId,
                    &type,
                    reinterpret_cast<PBYTE>(&actual),
                    sizeof(actual),
                    nullptr,
                    0)) {
                return false;
        }

        if (type != DEVPROP_TYPE_UINT32 || actual != session_id) {
                SetLastError(ERROR_INVALID_DATA);
                return false;
        }

        return true;
}

} // namespace


bool usbip::session_isolation::isolate_attached_usb_device_to_current_session(
        _In_ HANDLE,
        _In_ int port,
        _In_ DWORD timeout_ms) noexcept
{
        if (port <= 0) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return false;
        }

        auto session_id = get_current_session_id();
        if (!session_id) {
                return false;
        }
        if (*session_id == 0) {
                SetLastError(ERROR_ACCESS_DENIED);
                return false;
        }

        auto vhci_instance_id = get_vhci_instance_id();
        if (!vhci_instance_id) {
                if (!GetLastError()) {
                        SetLastError(ERROR_NOT_FOUND);
                }
                return false;
        }

        hdevinfo devices(SetupDiGetClassDevsW(
                nullptr,
                L"USB",
                nullptr,
                DIGCF_ALLCLASSES | DIGCF_PRESENT));
        if (!devices) {
                return false;
        }

        const ULONGLONG deadline = GetTickCount64() + timeout_ms;
        for (;;) {
                SP_DEVINFO_DATA matched{};
                matched.cbSize = sizeof(matched);
                std::optional<std::wstring> matched_instance_id;
                int matches = 0;

                SP_DEVINFO_DATA dev_data{};
                dev_data.cbSize = sizeof(dev_data);
                for (DWORD i = 0; SetupDiEnumDeviceInfo(devices.get(), i, &dev_data); ++i) {
                        LONG address = 0;
                        if (!try_read_int32_property(devices.get(), dev_data, DEVPKEY_Device_Address, address)) {
                                continue;
                        }
                        if (address != port) {
                                continue;
                        }

                        if (!has_ancestor(dev_data.DevInst, *vhci_instance_id)) {
                                continue;
                        }

                        auto instance_id = get_device_instance_id(devices.get(), dev_data);
                        if (!instance_id) {
                                continue;
                        }

                        ++matches;
                        matched = dev_data;
                        matched_instance_id = std::move(instance_id);
                }

                auto enum_err = GetLastError();
                if (enum_err != ERROR_NO_MORE_ITEMS) {
                        if (enum_err) {
                                return false;
                        }
                }

                if (matches > 1) {
                        SetLastError(ERROR_GEN_FAILURE);
                        return false;
                }

                if (matches == 1 && matched_instance_id) {
                        if (!set_and_verify_session_id(devices.get(), matched, *session_id)) {
                                return false;
                        }

                        libusbip::output(
                                L"Session isolation set: vhci '{}', port {}, session {}, devnode '{}'",
                                *vhci_instance_id,
                                port,
                                *session_id,
                                *matched_instance_id);
                        return true;
                }

                if (GetTickCount64() >= deadline) {
                        SetLastError(ERROR_TIMEOUT);
                        return false;
                }

                Sleep(100);
        }
}
