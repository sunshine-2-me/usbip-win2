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
        if (auto v = vhci::get_persistent(dev); !v) {
                spdlog::error(GetLastErrorMsg());
                return false;
        } else {
                spdlog::debug("attach persistent: {} device(s) from driver", v->size());
                for (auto &i: *v) {
                        printf("%s:%s/%s\n", i.hostname.c_str(), i.service.c_str(), i.busid.c_str());
                        if (!vhci::attach(dev, i)) {
                                spdlog::error(GetLastErrorMsg());
                        }
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
                spdlog::error(GetLastErrorMsg());
        }

        return ok;
}

} // namespace


bool usbip::cmd_attach(void *p)
{
        auto &args = *reinterpret_cast<attach_args*>(p);

        spdlog::debug("attach: remote='{}' busid='{}' tcp='{}' terse={} stop={} stop_all={} once={} persistent={}",
                      args.remote, args.busid, global_args.tcp_port, args.terse, args.stop, args.stop_all, args.once,
                      args.persistent);

        auto dev = vhci::open();
        if (!dev) {
                spdlog::error(GetLastErrorMsg());
                return false;
        }
        spdlog::debug("attach: vhci handle opened");

        if (args.persistent) {
                spdlog::debug("attach: loading persistent devices from driver");
                return attach_persistent_devices(dev.get());
        }

        device_location location {
                .hostname = args.remote, 
                .service = global_args.tcp_port, 
                .busid = args.busid,
        };

        if (args.stop || args.stop_all) {
                assert(args.stop != args.stop_all);
                spdlog::debug("attach: stop attach attempts (single_target={})", args.stop);
                return stop_attach_attempts(dev.get(), args.stop ? &location : nullptr);
        }

        auto options = args.once ? vhci::ATTACH_ONCE : 0;

        spdlog::debug("attach: plugin remote {}:{} busid {} options={:#x}", args.remote, global_args.tcp_port,
                      args.busid, options);

        auto port = vhci::attach(dev.get(), location, options);
        if (!port) {
                spdlog::error(GetLastErrorMsg());
                return false;
        }

        spdlog::debug("attach: driver assigned hub port {}", port);

        if (args.terse) {
                printf("%d\n", port);
        } else {
                printf("succesfully attached to port %d\n", port);
        }

        return true;
}
