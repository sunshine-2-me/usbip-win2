/*
 * Copyright (c) 2021-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "usbip.h"
#include <libusbip\vhci.h>
#include <libusbip\persistent.h>

#include <spdlog\spdlog.h>

namespace
{

using namespace usbip;

auto attach_persistent_devices(HANDLE dev)
{
        auto v = vhci::get_persistent(dev);
        if (!v) {
                const DWORD err = GetLastError();
                spdlog::error("{}", GetLastErrorMsg(err));
                return false;
        }

        spdlog::debug("attach: {} persistent device(s) to (re)attach", v->size());
        for (auto &i: *v) {
                printf("%s:%s/%s\n", i.hostname.c_str(), i.service.c_str(), i.busid.c_str());
                spdlog::debug("attach: persistent entry host='{}' service='{}' busid='{}'", i.hostname, i.service,
                              i.busid);
                if (!vhci::attach(dev, i)) {
                        const DWORD err = GetLastError();
                        spdlog::error("{}", GetLastErrorMsg(err));
                }
        }

        return true;
}

auto stop_attach_attempts(_In_ HANDLE dev, _In_opt_ const device_location *loc)
{
        auto cnt = vhci::stop_attach_attempts(dev, loc);
        auto ok = cnt >= 0;

        if (ok) {
                spdlog::debug("{} request(s) stopped", cnt);
        } else {
                const DWORD err = GetLastError();
                spdlog::error("{}", GetLastErrorMsg(err));
        }

        return ok;
}

} // namespace


bool usbip::cmd_attach(void *p)
{
        auto &args = *reinterpret_cast<attach_args*>(p);

        spdlog::debug(
                "attach: persistent={} stop={} stop_all={} once={} terse={} remote='{}' busid='{}' tcp_port='{}'",
                args.persistent, args.stop, args.stop_all, args.once, args.terse, args.remote, args.busid,
                global_args.tcp_port);

        auto dev = vhci::open();
        if (!dev) {
                const DWORD err = GetLastError();
                spdlog::error("{}", GetLastErrorMsg(err));
                return false;
        }
        spdlog::debug("attach: vhci opened");

        if (args.persistent) {
                spdlog::debug("attach: attaching persistent device list");
                return attach_persistent_devices(dev.get());
        }

        device_location location {
                .hostname = args.remote, 
                .service = global_args.tcp_port, 
                .busid = args.busid,
        };

        if (args.stop || args.stop_all) {
                assert(args.stop != args.stop_all);
                spdlog::debug("attach: stop attach attempts (single_device={})", args.stop);
                return stop_attach_attempts(dev.get(), args.stop ? &location : nullptr);
        }

        auto options = args.once ? vhci::ATTACH_ONCE : 0;
        spdlog::debug("attach: vhci::attach host='{}' service='{}' busid='{}' options={:#x}", location.hostname,
                      location.service, location.busid, static_cast<unsigned>(options));

        auto port = vhci::attach(dev.get(), location, options);
        if (!port) {
                const DWORD err = GetLastError();
                spdlog::error("{}", GetLastErrorMsg(err));
                return false;
        }

        spdlog::debug("attach: success port={}", port);

        if (args.terse) {
                printf("%d\n", port);
        } else {
                printf("succesfully attached to port %d\n", port);
        }

        return true;
}
