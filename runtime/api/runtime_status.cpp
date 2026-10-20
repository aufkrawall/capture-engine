// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// ce_runtime_get_status: a cheap snapshot, copied into the caller's struct_size (llm-wiki/library/03 "Status").
#include "include/cengine/cengine.h"

#include "runtime/api/runtime_handle.h"

#include <cstddef>

namespace ce::capi {
namespace {

// The smallest extent that carries the runtime state: struct_size itself and runtime_state.
constexpr uint32_t kMinimumStatusExtent =
    static_cast<uint32_t>(offsetof(ce_status_info_t, runtime_state) + sizeof(int32_t));

// Stores `value` into `field` only when the caller's extent covers the whole field. The extent is the caller's
// struct_size, which is never written here.
template <class T, class V>
void StoreWithin(ce_status_info_t* out, uint32_t extent, T ce_status_info_t::*field, V value) {
    const unsigned char* base = reinterpret_cast<const unsigned char*>(out);
    const size_t offset = static_cast<size_t>(reinterpret_cast<const unsigned char*>(&(out->*field)) - base);
    if (offset + sizeof(T) <= extent)
        out->*field = static_cast<T>(value);
}

}  // namespace
}  // namespace ce::capi

extern "C" ce_status_t ce_runtime_get_status(const ce_runtime_t* runtime, ce_status_info_t* out) {
    return ce::capi::RunGuarded("ce_runtime_get_status", [&]() -> ce_status_t {
        ce_runtime* live = ce::capi::ResolveRuntime(runtime, "ce_runtime_get_status");
        if (!live)
            return CE_E_INVALID_ARGUMENT;
        if (!out || out->struct_size < ce::capi::kMinimumStatusExtent)
            return CE_E_INVALID_ARGUMENT;
        const uint32_t extent = out->struct_size;
        const ce::engine::StatusSnapshot snapshot = live->engine->Status();
        ce::capi::StoreWithin(out, extent, &ce_status_info_t::runtime_state, snapshot.runtimeState);
        ce::capi::StoreWithin(out, extent, &ce_status_info_t::recording_state, snapshot.recordingState);
        ce::capi::StoreWithin(out, extent, &ce_status_info_t::recording_mode, snapshot.recordingMode);
        ce::capi::StoreWithin(out, extent, &ce_status_info_t::recording_id, snapshot.recordingId);
        ce::capi::StoreWithin(out, extent, &ce_status_info_t::helpers_ready, snapshot.helpersReady);
        ce::capi::StoreWithin(out, extent, &ce_status_info_t::finalizing_count, snapshot.finalizingCount);
        ce::capi::StoreWithin(out, extent, &ce_status_info_t::settings_revision, snapshot.settingsRevision);
        return CE_OK;
    });
}
