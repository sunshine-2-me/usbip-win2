/*
 * Copyright (c) 2021-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#pragma once

#include <cstddef>
#include <guiddef.h>

#ifdef _KERNEL_MODE
  #include <wdm.h>
  #include <minwindef.h>
#else
  #include <windows.h>
  #include <winioctl.h>
#endif

#include "ch9.h"
#include "consts.h"

/*
 * Strings encoding is UTF8. 
 */

namespace usbip::vhci
{

DEFINE_GUID(GUID_DEVINTERFACE_USB_HOST_CONTROLLER,
        0xB4030C06, 0xDC5F, 0x4FCC, 0x87, 0xEB, 0xE5, 0x51, 0x5A, 0x09, 0x35, 0xC0);

struct base
{
        ULONG size; // IN, self size
};

struct imported_device_location
{
        int port; // OUT, >= 1 or zero if an error

        char busid[BUS_ID_SIZE];
        char service[32]; // NI_MAXSERV
        char host[1025];  // NI_MAXHOST in ws2def.h
};
static_assert(!offsetof(imported_device_location, port)); // must be the first member

struct imported_device_properties
{
        UINT32 devid;
//      static_assert(sizeof(devid) == sizeof(usbip_header_basic::devid));

        usb_device_speed speed;
        static_assert(sizeof(speed) == sizeof(int));

        UINT16 vendor;
        UINT16 product;
};

struct imported_device : imported_device_location, imported_device_properties {};

enum class state { unplugged, connecting, connected, plugged, disconnected, unplugging };

/*
 * There can be multiple event sources for one device,
 * each of them emits events with a unique source_id.
 */
struct device_state : base, imported_device
{
        state state;
        ULONG source_id;
};

} // namespace usbip::vhci


namespace usbip::vhci::ioctl
{

enum class function { // 12 bit
        plugin_hardware = 0x800, // values of less than 0x800 are reserved for Microsoft
        plugout_hardware, 
        get_imported_devices,
        set_persistent,
        get_persistent,
        stop_attach_attempts,
        plugin_hardware_once,
        plugout_hardware_and_reattach,
        plugin_hardware_for_user, // explicit-SID variant for persistent / offline reattach (System-only)
        get_device_owner, // PDO -> owner SID/session lookup, used by usbip2_filter and broker
};

constexpr auto make(function id)
{
        return CTL_CODE(FILE_DEVICE_UNKNOWN, static_cast<int>(id), METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
}

enum {
        PLUGIN_HARDWARE = make(function::plugin_hardware),
        PLUGOUT_HARDWARE = make(function::plugout_hardware),
        GET_IMPORTED_DEVICES = make(function::get_imported_devices),
        SET_PERSISTENT = make(function::set_persistent),
        GET_PERSISTENT = make(function::get_persistent),
        STOP_ATTACH_ATTEMPTS = make(function::stop_attach_attempts),
        PLUGIN_HARDWARE_ONCE = make(function::plugin_hardware_once),
        PLUGOUT_HARDWARE_AND_REATTACH = make(function::plugout_hardware_and_reattach), // for internal use only
        PLUGIN_HARDWARE_FOR_USER = make(function::plugin_hardware_for_user),
        GET_DEVICE_OWNER = make(function::get_device_owner),
};

struct plugin_hardware : base, imported_device_location {};

/*
 * Explicit-owner-SID variant of plugin_hardware. Only callable by LocalSystem.
 * Used by the broker service for persistent reattach after reboot, where there
 * is no interactive caller to capture the SID from.
 *
 * SECURITY_MAX_SID_SIZE in WinNT.h is 68 bytes; 256 leaves comfortable headroom
 * for unusual derived SIDs and keeps the struct fixed size for IOCTL buffering.
 */
constexpr ULONG OWNER_SID_MAX_SIZE = 256;

struct plugin_hardware_for_user : base, imported_device_location
{
        ULONG sid_size;                       // bytes used in sid[]
        ULONG session_id;                     // optional, 0 if unknown
        UCHAR sid[OWNER_SID_MAX_SIZE];        // self-relative SID buffer
};

struct get_device_owner : base
{
        int port;                             // IN, hub port number, >= 1
        ULONG session_id;                     // OUT
        ULONG sid_size;                       // OUT, bytes used in sid[]
        UCHAR sid[OWNER_SID_MAX_SIZE];        // OUT, self-relative SID buffer
};

struct stop_attach_attempts : base, imported_device_location
{
        int count; // OUT, number of canceled requests
};

struct plugout_hardware : base
{
        int port; // all ports if <= 0
};

struct get_imported_devices : base
{
        imported_device devices[ANYSIZE_ARRAY];
};

constexpr auto get_imported_devices_size(_In_ ULONG n)
{
        return offsetof(get_imported_devices, devices) + n*sizeof(*get_imported_devices::devices);
}

} // namespace usbip::vhci::ioctl
