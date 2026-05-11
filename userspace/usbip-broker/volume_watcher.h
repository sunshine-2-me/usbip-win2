/*
 * Copyright (c) 2026
 *
 * Phase 8: per-session drive-letter mounting for USBIP mass-storage volumes.
 *
 * Listens for GUID_DEVINTERFACE_VOLUME arrivals via a hidden message-only
 * window in a worker thread, walks PnP parents up to find the owning USBIP
 * USB device, looks up the AttachState record for that device, and runs
 * `mountvol` inside the owner's RDP session via CreateProcessAsUser.
 *
 * If global automount is disabled (`mountvol /N`), the volume will not
 * surface in any other session - it only appears in the owning session.
 */
#pragma once

#include <atomic>
#include <thread>

#include <windows.h>

namespace usbip::broker
{

class VolumeWatcher
{
public:
        VolumeWatcher();
        ~VolumeWatcher();

        VolumeWatcher(const VolumeWatcher&) = delete;
        VolumeWatcher &operator=(const VolumeWatcher&) = delete;

        // Start the worker thread + window + RegisterDeviceNotification.
        // Best-effort: returns false if the worker could not be started.
        bool start();

        // Signal the worker to exit and join.
        void stop();

private:
        static unsigned __stdcall thread_main(void *self);

        void run();

        std::atomic<bool>     running_{false};
        HANDLE                thread_{nullptr};
        unsigned              thread_id_{0};
        DWORD                 worker_tid_{0};
        HWND                  hwnd_{nullptr};
        HDEVNOTIFY            notify_{nullptr};
};

} // namespace usbip::broker
