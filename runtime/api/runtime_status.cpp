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
template <class S, class T, class V>
void StoreWithin(S* out, uint32_t extent, T S::*field, V value) {
    const unsigned char* base = reinterpret_cast<const unsigned char*>(out);
    const size_t offset = static_cast<size_t>(reinterpret_cast<const unsigned char*>(&(out->*field)) - base);
    if (offset + sizeof(T) <= extent)
        out->*field = static_cast<T>(value);
}

// The smallest setup status extent that carries a field: struct_size plus the first state field.
constexpr uint32_t kMinimumSetupStatusExtent =
    static_cast<uint32_t>(offsetof(ce_setup_status_t, sensor_driver_installed) + sizeof(uint32_t));

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

extern "C" ce_status_t ce_setup_get_status(ce_runtime_t* runtime, ce_setup_status_t* out) {
    return ce::capi::RunGuarded("ce_setup_get_status", [&]() -> ce_status_t {
        ce_runtime* live = ce::capi::ResolveRuntime(runtime, "ce_setup_get_status");
        if (!live)
            return CE_E_INVALID_ARGUMENT;
        if (!out || out->struct_size < ce::capi::kMinimumSetupStatusExtent)
            return CE_E_INVALID_ARGUMENT;
        ce::runtime::SetupStatus status;
        if (!live->engine->SetupState(status))
            return CE_E_UNSUPPORTED;
        const uint32_t extent = out->struct_size;
        ce::capi::StoreWithin(out, extent, &ce_setup_status_t::sensor_driver_installed,
                              status.sensorDriverInstalled ? 1u : 0u);
        ce::capi::StoreWithin(out, extent, &ce_setup_status_t::elevation_service_state,
                              status.elevationServiceState);
        ce::capi::StoreWithin(out, extent, &ce_setup_status_t::client_autostart, status.clientAutostart ? 1u : 0u);
        ce::capi::StoreWithin(out, extent, &ce_setup_status_t::sensor_driver_offer, status.sensorDriverOffer ? 1u : 0u);
        ce::capi::StoreWithin(out, extent, &ce_setup_status_t::launch_elevated, status.launchElevated ? 1u : 0u);
        ce::capi::StoreWithin(out, extent, &ce_setup_status_t::elevation_service_preferred,
                              status.elevationServicePreferred ? 1u : 0u);
        return CE_OK;
    });
}
