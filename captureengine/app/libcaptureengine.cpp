// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall

#include "libcaptureengine_internal.h"

#include <windows.h>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>

#include "common/logging/logging.h"
#include "common/logging/log_meter.h"
#include "common/platform/build_identity.h"

struct ce_engine {};

namespace {
// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - recursive_mutex construction is noexcept on this toolchain
std::recursive_mutex g_ApiMutex;
ce::api::ControllerBackend g_Backend;
DWORD g_ControllerThread = 0;
std::unique_ptr<ce_engine_t> g_Engine;
bool g_InCallback = false;
bool g_Polling = false;

ce_status_t ValidateEngine(const ce_engine_t* engine) {
    if (!engine || engine != g_Engine.get())
        return CE_ERROR_INVALID_ARGUMENT;
    if (g_ControllerThread == 0)
        return CE_ERROR_NOT_INITIALIZED;
    if (GetCurrentThreadId() != g_ControllerThread || g_InCallback)
        return CE_ERROR_INVALID_STATE;
    return CE_SUCCESS;
}

template <typename Callback>
ce_status_t Invoke(const ce_engine_t* engine, Callback callback, bool polling = false) {
    std::lock_guard<std::recursive_mutex> lock(g_ApiMutex);
    const ce_status_t validation = ValidateEngine(engine);
    if (validation != CE_SUCCESS)
        return validation;
    if (polling && g_Polling)
        return CE_ERROR_INVALID_STATE;
    struct CallbackScope {
        bool& active;
        explicit CallbackScope(bool& flag) : active(flag) {
            active = true;
        }
        ~CallbackScope() {
            active = false;
        }
    } scope(polling ? g_Polling : g_InCallback);
    try {
        return callback();
    } catch (...) {
        static ce::log_meter::ChangeGate gate;
        if (gate.Observe(0))
            LogError("[EngineAPI] Controller callback threw; reporting process failure");
        return CE_ERROR_PROCESS_FAILURE;
    }
}

ce_status_t SendCommand(ce_engine_t* engine, ce::api::Command command,
                        ce_recording_intent_t intent = CE_RECORDING_INTENT_IDLE, const char* reason = nullptr) {
    return Invoke(engine, [&] { return g_Backend.command(command, intent, reason); });
}
}  // namespace

namespace ce::api {
bool BindControllerBackend(const ControllerBackend& backend) {
    std::lock_guard<std::recursive_mutex> lock(g_ApiMutex);
    if (g_ControllerThread != 0 || g_Engine || g_InCallback || g_Polling ||
        !backend.command || !backend.recordingIntent || !backend.poll)
        return false;
    g_Backend = backend;
    g_ControllerThread = GetCurrentThreadId();
    LogDebug("[EngineAPI] Controller backend bound to its owner thread");
    return true;
}

void UnbindControllerBackend() {
    std::lock_guard<std::recursive_mutex> lock(g_ApiMutex);
    if (GetCurrentThreadId() != g_ControllerThread || g_InCallback || g_Polling)
        return;
    g_Backend = {};
    g_ControllerThread = 0;
    LogDebug("[EngineAPI] Controller backend detached");
}
}  // namespace ce::api

extern "C" {
ce_status_t ce_engine_config_init_default(ce_engine_config_t* config) {
    if (!config)
        return CE_ERROR_INVALID_ARGUMENT;
    std::memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->log_level = CE_LOG_LEVEL_INFO;
    config->enable_hotkeys = true;
    config->enable_system_tray = true;
    return CE_SUCCESS;
}

ce_status_t ce_engine_create(const ce_engine_config_t* config, ce_engine_t** out_engine) {
    if (!out_engine)
        return CE_ERROR_INVALID_ARGUMENT;
    *out_engine = nullptr;
    if (config) {
        if (config->struct_size != sizeof(*config) || config->log_level < CE_LOG_LEVEL_NONE ||
            config->log_level > CE_LOG_LEVEL_TRACE)
            return CE_ERROR_INVALID_ARGUMENT;
        // The controller already loaded its configuration. Do not silently ignore settings
        // or retain borrowed path strings while claiming a new runtime was initialized.
        if (config->config_file_path || config->log_directory || config->log_level != CE_LOG_LEVEL_INFO ||
            !config->enable_hotkeys || !config->enable_system_tray || config->start_minimized)
            return CE_ERROR_UNSUPPORTED;
    }
    std::lock_guard<std::recursive_mutex> lock(g_ApiMutex);
    if (g_ControllerThread == 0)
        return CE_ERROR_NOT_INITIALIZED;
    if (GetCurrentThreadId() != g_ControllerThread || g_InCallback || g_Polling)
        return CE_ERROR_INVALID_STATE;
    if (g_Engine)
        return CE_ERROR_ALREADY_INITIALIZED;
    g_Engine.reset(new (std::nothrow) ce_engine);
    if (!g_Engine)
        return CE_ERROR_PROCESS_FAILURE;
    *out_engine = g_Engine.get();
    return CE_SUCCESS;
}

ce_status_t ce_engine_destroy(ce_engine_t* engine) {
    std::lock_guard<std::recursive_mutex> lock(g_ApiMutex);
    if (!engine || engine != g_Engine.get())
        return CE_ERROR_INVALID_ARGUMENT;
    if (g_InCallback || g_Polling || (g_ControllerThread != 0 && GetCurrentThreadId() != g_ControllerThread))
        return CE_ERROR_INVALID_STATE;
    g_Engine.reset();
    return CE_SUCCESS;
}

ce_status_t ce_engine_start_recording(ce_engine_t* engine, ce_recording_intent_t intent, const char* reason) {
    if (intent != CE_RECORDING_INTENT_VIDEO && intent != CE_RECORDING_INTENT_AUDIO_ONLY)
        return CE_ERROR_INVALID_ARGUMENT;
    return SendCommand(engine, ce::api::Command::Start, intent, reason);
}

ce_status_t ce_engine_stop_recording(ce_engine_t* engine, const char* reason) {
    return SendCommand(engine, ce::api::Command::Stop, CE_RECORDING_INTENT_IDLE, reason);
}

ce_status_t ce_engine_toggle_recording(ce_engine_t* engine) {
    return SendCommand(engine, ce::api::Command::ToggleVideo);
}

ce_status_t ce_engine_toggle_audio_only(ce_engine_t* engine) {
    return SendCommand(engine, ce::api::Command::ToggleAudio);
}

ce_status_t ce_engine_toggle_overlay(ce_engine_t* engine) {
    return SendCommand(engine, ce::api::Command::Overlay);
}

ce_status_t ce_engine_toggle_benchmark(ce_engine_t* engine) {
    return SendCommand(engine, ce::api::Command::Benchmark);
}

ce_status_t ce_engine_take_screenshot(ce_engine_t* engine) {
    return SendCommand(engine, ce::api::Command::Screenshot);
}

ce_status_t ce_engine_is_recording(const ce_engine_t* engine, bool* out_is_recording) {
    if (!out_is_recording)
        return CE_ERROR_INVALID_ARGUMENT;
    *out_is_recording = false;
    return Invoke(engine, [&] {
        *out_is_recording = g_Backend.recordingIntent() != CE_RECORDING_INTENT_IDLE;
        return CE_SUCCESS;
    });
}

ce_status_t ce_engine_get_recording_stats(const ce_engine_t* engine, ce_recording_stats_t* out_stats) {
    if (!out_stats || out_stats->struct_size != sizeof(*out_stats))
        return CE_ERROR_INVALID_ARGUMENT;
    return Invoke(engine, [] {
        // The private media process has no complete snapshot for these fields yet.
        // Keep the caller's buffer intact instead of reporting invented zero statistics.
        return CE_ERROR_UNSUPPORTED;
    });
}

ce_status_t ce_engine_poll_events(ce_engine_t* engine, uint32_t timeout_ms) {
    if (timeout_ms == UINT32_MAX)
        return CE_ERROR_INVALID_ARGUMENT;
    return Invoke(engine, [&] { return g_Backend.poll(timeout_ms); }, true);
}

const char* ce_status_to_string(ce_status_t status) {
    switch (status) {
        case CE_SUCCESS:
            return "Success";
        case CE_ERROR_INVALID_ARGUMENT:
            return "Invalid argument";
        case CE_ERROR_INVALID_STATE:
            return "Invalid state";
        case CE_ERROR_NOT_INITIALIZED:
            return "Not initialized";
        case CE_ERROR_ALREADY_INITIALIZED:
            return "Already initialized";
        case CE_ERROR_PROCESS_FAILURE:
            return "Process failure";
        case CE_ERROR_IPC_FAILURE:
            return "IPC failure";
        case CE_ERROR_IO_FAILURE:
            return "I/O failure";
        case CE_ERROR_TIMEOUT:
            return "Timeout";
        case CE_ERROR_UNSUPPORTED:
            return "Unsupported";
        case CE_ERROR_UNKNOWN:
        default:
            return "Unknown error";
    }
}

const char* ce_version_string(void) {
    return GetCaptureVersion();
}
}  // extern "C"
