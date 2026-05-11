/*
 * Copyright (c) 2026
 *
 * Per-request dispatch: parse JSON, capture identity, consult policy, drive
 * the VHCI driver (with impersonation), serialize JSON response.
 */
#pragma once

#include <string>

#include <windows.h>

namespace usbip::broker
{

class Policy;
class OwnershipStore;

class RequestHandler
{
public:
        explicit RequestHandler(Policy &policy, OwnershipStore &ownership)
                : policy_(policy), ownership_(ownership) {}

        // Read one message from the pipe, dispatch, write response.
        // Returns true if the connection should stay open for another message.
        bool serve_one(HANDLE pipe);

private:
        std::string handle(HANDLE pipe, std::string_view request_json);

        Policy &policy_;
        OwnershipStore &ownership_;
};

} // namespace usbip::broker
