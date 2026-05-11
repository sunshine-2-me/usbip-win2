/*
 * Copyright (c) 2026
 */
#include "pipe_server.h"
#include "handler.h"
#include "ownership.h"
#include "policy.h"
#include "protocol.h"

#include <thread>

#include <windows.h>
#include <sddl.h>
#include <aclapi.h>

#include <spdlog/spdlog.h>

namespace usbip::broker
{

namespace
{

struct SecAttrHolder
{
        SECURITY_ATTRIBUTES sa{};
        PSECURITY_DESCRIPTOR sd = nullptr;

        ~SecAttrHolder() { if (sd) LocalFree(sd); }

        bool init()
        {
                if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                                kPipeSddl, SDDL_REVISION_1, &sd, nullptr)) {
                        spdlog::error("ConvertStringSecurityDescriptorToSecurityDescriptorW err={}",
                                GetLastError());
                        return false;
                }
                sa.nLength = sizeof(sa);
                sa.lpSecurityDescriptor = sd;
                sa.bInheritHandle = FALSE;
                return true;
        }
};

HANDLE create_pipe_instance(LPSECURITY_ATTRIBUTES sa, bool first)
{
        DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
        if (first) {
                open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
        }

        return CreateNamedPipeW(
                kPipeName,
                open_mode,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                PIPE_UNLIMITED_INSTANCES,
                static_cast<DWORD>(kMaxMessageBytes),
                static_cast<DWORD>(kMaxMessageBytes),
                0,
                sa);
}

void worker(HANDLE pipe, Policy &policy, OwnershipStore &ownership)
{
        spdlog::debug("pipe worker {:p}", static_cast<void*>(pipe));
        try {
                RequestHandler h(policy, ownership);
                while (h.serve_one(pipe)) {
                        // serve more requests on the same connection
                }
        } catch (const std::exception &ex) {
                spdlog::error("worker exception: {}", ex.what());
        }

        spdlog::debug("pipe worker {:p} disconnecting",
                      static_cast<void*>(pipe));
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
}

} // namespace

DWORD PipeServer::run(HANDLE stop_event)
{
        SecAttrHolder sec;
        if (!sec.init()) {
                return GetLastError();
        }

        auto first = true;

        while (!stopping_.load(std::memory_order_relaxed)) {
                auto pipe = create_pipe_instance(&sec.sa, first);
                first = false;

                if (pipe == INVALID_HANDLE_VALUE) {
                        auto err = GetLastError();
                        spdlog::error("CreateNamedPipeW err={}", err);
                        return err;
                }

                OVERLAPPED ov{};
                ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (!ov.hEvent) {
                        CloseHandle(pipe);
                        return GetLastError();
                }

                BOOL connected = ConnectNamedPipe(pipe, &ov);
                auto err = connected ? ERROR_SUCCESS : GetLastError();

                if (!connected && err == ERROR_PIPE_CONNECTED) {
                        connected = TRUE;
                }
                if (!connected && err == ERROR_IO_PENDING) {
                        HANDLE waits[] = { ov.hEvent, stop_event };
                        auto wr = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                        if (wr == WAIT_OBJECT_0 + 1) {
                                stopping_.store(true);
                                CancelIoEx(pipe, &ov);
                                CloseHandle(ov.hEvent);
                                CloseHandle(pipe);
                                break;
                        }
                        DWORD bytes = 0;
                        if (GetOverlappedResult(pipe, &ov, &bytes, TRUE)) {
                                connected = TRUE;
                        }
                }

                CloseHandle(ov.hEvent);

                if (!connected) {
                        spdlog::warn("ConnectNamedPipe failed err={}", err);
                        CloseHandle(pipe);
                        continue;
                }

                spdlog::debug("pipe client accepted {:p}", static_cast<void*>(pipe));

                std::thread t(worker, pipe, std::ref(policy_), std::ref(ownership_));
                t.detach();
        }

        return 0;
}

} // namespace usbip::broker
