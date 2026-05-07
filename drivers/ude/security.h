/*
 * Copyright (c) 2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#pragma once

#include "context.h"

namespace usbip
{

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void capture_plugin_requestor_sid(_In_opt_ WDFREQUEST capture_request, _Inout_ device_ctx_ext &ext);

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS build_owner_sddl(
        _In_ const device_ctx_ext &ext,
        _Out_writes_(wchar_count * sizeof(WCHAR)) PWCHAR sddl_dst,
        _In_ ULONG wchar_count,
        _Out_ ULONG *wchar_out /* including NUL */);

} // namespace usbip
