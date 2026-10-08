// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include "include/cengine/cengine_draft.h"

#ifdef __cplusplus
#define CE_ABI_ASSERT static_assert
#define CE_ABI_ALIGNOF alignof
#else
#define CE_ABI_ASSERT _Static_assert
#define CE_ABI_ALIGNOF _Alignof
#endif

#define CE_ABI_TYPE(type, size, alignment)                \
    CE_ABI_ASSERT(sizeof(type) == (size), #type " size"); \
    CE_ABI_ASSERT(CE_ABI_ALIGNOF(type) == (alignment), #type " alignment")
#define CE_ABI_FIELD(type, field, offset, size)                                   \
    CE_ABI_ASSERT(offsetof(type, field) == (offset), #type "." #field " offset"); \
    CE_ABI_ASSERT(sizeof(((type*)0)->field) == (size), #type "." #field " width")

CE_ABI_ASSERT(sizeof(void*) == 8, "v2 targets Windows x64");
CE_ABI_TYPE(ce_status_t, 4, 4);
CE_ABI_TYPE(ce_request_t, 8, 8);

CE_ABI_TYPE(ce_runtime_desc_t, 40, 8);
CE_ABI_FIELD(ce_runtime_desc_t, struct_size, 0, 4);
CE_ABI_FIELD(ce_runtime_desc_t, api_version, 4, 4);
CE_ABI_FIELD(ce_runtime_desc_t, package_dir, 8, 8);
CE_ABI_FIELD(ce_runtime_desc_t, data_dir, 16, 8);
CE_ABI_FIELD(ce_runtime_desc_t, client_name, 24, 8);
CE_ABI_FIELD(ce_runtime_desc_t, features, 32, 4);
CE_ABI_FIELD(ce_runtime_desc_t, reserved, 36, 4);

CE_ABI_TYPE(ce_status_info_t, 40, 8);
CE_ABI_FIELD(ce_status_info_t, struct_size, 0, 4);
CE_ABI_FIELD(ce_status_info_t, runtime_state, 4, 4);
CE_ABI_FIELD(ce_status_info_t, recording_state, 8, 4);
CE_ABI_FIELD(ce_status_info_t, recording_mode, 12, 4);
CE_ABI_FIELD(ce_status_info_t, recording_id, 16, 8);
CE_ABI_FIELD(ce_status_info_t, helpers_ready, 24, 4);
CE_ABI_FIELD(ce_status_info_t, finalizing_count, 28, 4);
CE_ABI_FIELD(ce_status_info_t, settings_revision, 32, 8);

CE_ABI_TYPE(ce_event_t, 112, 8);
CE_ABI_FIELD(ce_event_t, struct_size, 0, 4);
CE_ABI_FIELD(ce_event_t, type, 4, 4);
CE_ABI_FIELD(ce_event_t, sequence, 8, 8);
CE_ABI_FIELD(ce_event_t, time_us, 16, 8);
CE_ABI_FIELD(ce_event_t, request, 24, 8);
CE_ABI_FIELD(ce_event_t, status, 32, 4);
CE_ABI_FIELD(ce_event_t, reserved0, 36, 4);
CE_ABI_FIELD(ce_event_t, message, 40, 8);
CE_ABI_FIELD(ce_event_t, data, 48, 64);
CE_ABI_FIELD(ce_event_t, data.runtime.state, 48, 4);
CE_ABI_FIELD(ce_event_t, data.recording.recording_id, 48, 8);
CE_ABI_FIELD(ce_event_t, data.recording.state, 56, 4);
CE_ABI_FIELD(ce_event_t, data.recording.mode, 60, 4);
CE_ABI_FIELD(ce_event_t, data.recording.failure, 64, 4);
CE_ABI_FIELD(ce_event_t, data.finalized.recording_id, 48, 8);
CE_ABI_FIELD(ce_event_t, data.finalized.output_path, 56, 8);
CE_ABI_FIELD(ce_event_t, data.screenshot.paths, 48, 8);
CE_ABI_FIELD(ce_event_t, data.screenshot.path_count, 56, 4);
CE_ABI_FIELD(ce_event_t, data.settings.revision, 48, 8);
CE_ABI_FIELD(ce_event_t, data.settings.source, 56, 4);
CE_ABI_FIELD(ce_event_t, data.helper.helper, 48, 4);
CE_ABI_FIELD(ce_event_t, data.helper.state, 52, 4);
CE_ABI_FIELD(ce_event_t, data.hotkey.action, 48, 4);
CE_ABI_FIELD(ce_event_t, data.dropped.count, 48, 8);
CE_ABI_FIELD(ce_event_t, data.reserved, 48, 64);

CE_ABI_TYPE(ce_settings_diagnostic_t, 32, 8);
CE_ABI_FIELD(ce_settings_diagnostic_t, struct_size, 0, 4);
CE_ABI_FIELD(ce_settings_diagnostic_t, severity, 4, 4);
CE_ABI_FIELD(ce_settings_diagnostic_t, section, 8, 8);
CE_ABI_FIELD(ce_settings_diagnostic_t, key, 16, 8);
CE_ABI_FIELD(ce_settings_diagnostic_t, message, 24, 8);

CE_ABI_TYPE(ce_setup_status_t, 16, 4);
CE_ABI_FIELD(ce_setup_status_t, struct_size, 0, 4);
CE_ABI_FIELD(ce_setup_status_t, sensor_driver_installed, 4, 4);
CE_ABI_FIELD(ce_setup_status_t, elevation_service_state, 8, 4);
CE_ABI_FIELD(ce_setup_status_t, client_autostart, 12, 4);

CE_ABI_TYPE(ce_monitor_info_t, 404, 4);
CE_ABI_FIELD(ce_monitor_info_t, id, 0, 256);
CE_ABI_FIELD(ce_monitor_info_t, name, 256, 128);
CE_ABI_FIELD(ce_monitor_info_t, left, 384, 4);
CE_ABI_FIELD(ce_monitor_info_t, top, 388, 4);
CE_ABI_FIELD(ce_monitor_info_t, right, 392, 4);
CE_ABI_FIELD(ce_monitor_info_t, bottom, 396, 4);
CE_ABI_FIELD(ce_monitor_info_t, primary, 400, 4);

#undef CE_ABI_FIELD
#undef CE_ABI_TYPE
#undef CE_ABI_ALIGNOF
#undef CE_ABI_ASSERT
