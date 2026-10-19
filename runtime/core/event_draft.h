// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include "include/cengine/cengine_draft.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace ce::engine {

using EventPayload = decltype(ce_event_t::data);

// What the engine says happened. The queue copies the strings into the record it hands to the
// client, so a draft (and every view in it) may die the moment a push returns. `payload` carries the
// scalar union members; the queue fills the pointer members (`finalized.output_path`,
// `screenshot.paths/path_count`) from `outputPath` / `paths` and ignores whatever the draft left there.
struct EventDraft {
    int32_t type = 0;  // CE_EVENT_*
    ce_request_t request = 0;
    ce_status_t status = CE_OK;
    std::string_view message;                 // UTF-8 diagnostic; empty = none
    std::string_view outputPath;              // CE_EVENT_RECORDING_FINALIZED only
    std::span<const std::string_view> paths;  // CE_EVENT_SCREENSHOT only
    EventPayload payload{};
};

// One builder per event type, so the union member of each type is written in exactly one place.
inline EventDraft RuntimeStateEvent(int32_t state, std::string_view message = {}) {
    EventDraft draft;
    draft.type = CE_EVENT_RUNTIME_STATE;
    draft.message = message;
    draft.payload.runtime.state = state;
    return draft;
}

inline EventDraft CommandCompletedEvent(ce_request_t request, ce_status_t status, std::string_view message = {}) {
    EventDraft draft;
    draft.type = CE_EVENT_COMMAND_COMPLETED;
    draft.request = request;
    draft.status = status;
    draft.message = message;
    return draft;
}

inline EventDraft RecordingStateEvent(ce_request_t request, uint64_t recordingId, int32_t state, int32_t mode,
                                      uint32_t failure, std::string_view message = {}) {
    EventDraft draft;
    draft.type = CE_EVENT_RECORDING_STATE;
    draft.request = request;
    draft.message = message;
    draft.payload.recording.recording_id = recordingId;
    draft.payload.recording.state = state;
    draft.payload.recording.mode = mode;
    draft.payload.recording.failure = failure;
    return draft;
}

inline EventDraft RecordingFinalizedEvent(uint64_t recordingId, ce_status_t status, std::string_view outputPath,
                                          std::string_view message = {}) {
    EventDraft draft;
    draft.type = CE_EVENT_RECORDING_FINALIZED;
    draft.status = status;
    draft.message = message;
    draft.outputPath = outputPath;
    draft.payload.finalized.recording_id = recordingId;
    return draft;
}

inline EventDraft ScreenshotEvent(ce_request_t request, ce_status_t status, std::span<const std::string_view> paths,
                                  std::string_view message = {}) {
    EventDraft draft;
    draft.type = CE_EVENT_SCREENSHOT;
    draft.request = request;
    draft.status = status;
    draft.message = message;
    draft.paths = paths;
    return draft;
}

inline EventDraft SettingsChangedEvent(uint64_t revision, int32_t source) {
    EventDraft draft;
    draft.type = CE_EVENT_SETTINGS_CHANGED;
    draft.payload.settings.revision = revision;
    draft.payload.settings.source = source;
    return draft;
}

inline EventDraft SettingsRejectedEvent(std::string_view message) {
    EventDraft draft;
    draft.type = CE_EVENT_SETTINGS_REJECTED;
    draft.message = message;
    return draft;
}

inline EventDraft HelperEvent(uint32_t helper, int32_t state, std::string_view message = {}) {
    EventDraft draft;
    draft.type = CE_EVENT_HELPER;
    draft.message = message;
    draft.payload.helper.helper = helper;
    draft.payload.helper.state = state;
    return draft;
}

inline EventDraft HotkeyEvent(int32_t action) {
    EventDraft draft;
    draft.type = CE_EVENT_HOTKEY;
    draft.payload.hotkey.action = action;
    return draft;
}

}  // namespace ce::engine
