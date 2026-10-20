// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include "include/cengine/cengine.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ce::capi {

// ce_runtime_desc_t as API 2.0 defined it. Newer minors append fields behind it (guarded by
// struct_size), so every client of every 2.x passes at least this much.
inline constexpr uint32_t kRuntimeDescBaseSize = 40;
static_assert(sizeof(ce_runtime_desc_t) == kRuntimeDescBaseSize, "append-only: bump the minor, not this size");

inline constexpr uint32_t kKnownFeatures =
    CE_FEATURE_GLOBAL_HOTKEYS | CE_FEATURE_DESKTOP_OVERLAY | CE_FEATURE_CRASH_HANDLER | CE_FEATURE_WATCH_SETTINGS;
inline constexpr size_t kMaxPathBytes = 98304;  // 32767 UTF-16 units at 3 UTF-8 bytes each, rounded up
inline constexpr size_t kMaxClientNameBytes = 256;

// 03 "Versioning": a different major or a client minor newer than the runtime's cannot be served
// (CE_E_VERSION); an older client minor is fine.
ce_status_t CheckApiVersion(uint32_t clientVersion, uint32_t runtimeVersion = CE_API_VERSION);

// Strict UTF-8 (RFC 3629): no overlongs, surrogates or code points above U+10FFFF.
bool IsValidUtf8(std::string_view text);

// The validated, owned copy create works from: input strings are copied before an accepting call returns.
struct RuntimeDescCopy {
    uint32_t apiVersion = 0;
    uint32_t features = 0;
    std::optional<std::string> packageDir;  // nullopt = the client passed NULL (default location)
    std::optional<std::string> dataDir;
    std::string clientName;  // empty = unnamed
};

// Checks what ce_runtime_create must check before it does anything else: NULL, the extent the client
// declared (reads never go beyond min(struct_size, the fields this runtime knows)), the version,
// reserved/unknown bits, and the strings (non-empty directories, UTF-8, bounded length). `out` may be
// null for a pure check. `reason` (optional) receives a static diagnostic for logging. CE_E_NO_MEMORY
// if a copy cannot be allocated.
ce_status_t ValidateRuntimeDesc(const ce_runtime_desc_t* desc, RuntimeDescCopy* out, const char** reason = nullptr);

}  // namespace ce::capi
