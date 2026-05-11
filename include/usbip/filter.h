/*
 * Copyright (c) 2026
 *
 * usbip2_filter.sys public ABI: control-device device-interface GUID and the
 * IOCTLs used by usbip-broker (and the persistent-reattach path in
 * usbip2_ude.sys) to install per-device owner SIDs.
 *
 * The filter exposes a small in-kernel table keyed by the device-instance-id
 * of an emulated USB PDO. When a CreateFile arrives at a filtered PDO, the
 * filter looks up its instance id and refuses callers whose SID does not
 * match the registered owner.
 */
#pragma once

#include <guiddef.h>

#ifdef _KERNEL_MODE
  #include <wdm.h>
#else
  #include <windows.h>
  #include <winioctl.h>
#endif

namespace usbip::filter
{

DEFINE_GUID(GUID_DEVINTERFACE_USBIP_FILTER,
        0x6f3b3a4e, 0x9b3a, 0x4c4d, 0xab, 0x12, 0x55, 0x0a, 0x91, 0x9c, 0xfe, 0x10);

inline constexpr wchar_t kControlDeviceName[] = L"\\Device\\usbip2_filter_ctl";
inline constexpr wchar_t kSymbolicLinkName[]  = L"\\DosDevices\\usbip2_filter_ctl";

namespace ioctl
{

enum class function {
        set_owner = 0x800,
        clear_owner,
        clear_all_owners,
};

constexpr auto make(function id)
{
        return CTL_CODE(FILE_DEVICE_UNKNOWN, static_cast<int>(id),
                        METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
}

enum {
        SET_OWNER         = make(function::set_owner),
        CLEAR_OWNER       = make(function::clear_owner),
        CLEAR_ALL_OWNERS  = make(function::clear_all_owners),
};

constexpr ULONG OWNER_SID_MAX_SIZE     = 256;        // SECURITY_MAX_SID_SIZE w/ headroom
constexpr ULONG INSTANCE_ID_MAX_CHARS  = 256;        // including trailing NUL

struct base
{
        ULONG size; // self size for ABI guard
};

struct set_owner : base
{
        ULONG  session_id;
        ULONG  sid_size;
        UCHAR  sid[OWNER_SID_MAX_SIZE];
        ULONG  instance_id_chars;                    // wchar_t count incl. NUL
        wchar_t instance_id[INSTANCE_ID_MAX_CHARS];
};

struct clear_owner : base
{
        ULONG  instance_id_chars;
        wchar_t instance_id[INSTANCE_ID_MAX_CHARS];
};

struct clear_all_owners : base {};

} // namespace ioctl

} // namespace usbip::filter
