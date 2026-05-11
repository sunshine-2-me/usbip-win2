/*
 * Copyright (c) 2026
 *
 * Synchronous accept loop that hands each connection off to a worker thread.
 * One pipe instance per request keeps impersonation simple.
 */
#pragma once

#include <atomic>

#include <windows.h>

namespace usbip::broker
{

class Policy;
class OwnershipStore;

class PipeServer
{
public:
        explicit PipeServer(Policy &policy, OwnershipStore &ownership)
                : policy_(policy), ownership_(ownership) {}

        // Run the accept loop until stop_event is signalled.
        // Returns Win32 last error code on hard failure, 0 on clean stop.
        DWORD run(HANDLE stop_event);

private:
        Policy &policy_;
        OwnershipStore &ownership_;
        std::atomic<bool> stopping_ { false };
};

} // namespace usbip::broker
