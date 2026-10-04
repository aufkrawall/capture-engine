#pragma once

#include "include/libcaptureengine.h"

namespace ce::api {

// Bind only while the initialized controller lives. Callbacks run on its owning thread.
enum class Command { Start, Stop, ToggleVideo, ToggleAudio, Overlay, Benchmark, Screenshot };

struct ControllerBackend {
    ce_status_t (*command)(Command, ce_recording_intent_t, const char*) = nullptr;
    ce_recording_intent_t (*recordingIntent)() = nullptr;
    ce_status_t (*poll)(uint32_t) = nullptr;
};

bool BindControllerBackend(const ControllerBackend& backend);
void UnbindControllerBackend();

}  // namespace ce::api
