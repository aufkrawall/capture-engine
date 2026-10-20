// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// The commands of the C ABI (llm-wiki/library/03 "Commands"): each entry point builds one engine command, admits it
// and reports the request id. The engine owns the policy; these functions only translate arguments.
#include "include/cengine/cengine.h"

#include "common/logging/logging.h"
#include "runtime/api/capi_text.h"
#include "runtime/api/handle_registry.h"
#include "runtime/api/runtime_handle.h"
#include "runtime/core/core.h"

#include <utility>

namespace ce::capi {
namespace {

using ce::engine::Command;
using ce::engine::CommandKind;

// The shape every command entry point shares: resolve the handle, build the command (which reads the client's
// strings), admit it. *out is 0 unless the command is admitted; out may be NULL.
template <class BuildCommand>
ce_status_t RunCommand(const char* entryPoint, ce_runtime_t* handle, ce_request_t* out, BuildCommand&& build) {
    return RunGuarded(entryPoint, [&]() -> ce_status_t {
        if (out)
            *out = 0;
        ce_runtime* runtime = ResolveRuntime(handle, entryPoint);
        if (!runtime)
            return CE_E_INVALID_ARGUMENT;
        Command command;
        const ce_status_t built = build(command);
        if (built != CE_OK)
            return built;
        const ce::engine::Admission admission = runtime->engine->Submit(std::move(command));
        if (admission.status != CE_OK) {
            LogDebug("[CApi] %s not admitted: %s", entryPoint, ce_status_string(admission.status));
            return admission.status;
        }
        if (out)
            *out = admission.request;
        return CE_OK;
    });
}

// A recording command. A NULL reason is an empty one.
ce_status_t RecordingCommand(Command& command, CommandKind kind, int32_t mode, const char* reason) {
    command.kind = kind;
    command.mode = mode;
    return reason ? ReadClientText(reason, kMaxReasonBytes, command.text) : CE_OK;
}

ce_status_t PlainCommand(Command& command, CommandKind kind) {
    command.kind = kind;
    return CE_OK;
}

}  // namespace
}  // namespace ce::capi

extern "C" {

ce_status_t ce_recording_start(ce_runtime_t* rt, int32_t mode, const char* reason, ce_request_t* out) {
    return ce::capi::RunCommand("ce_recording_start", rt, out, [&](ce::engine::Command& command) {
        return ce::capi::RecordingCommand(command, ce::engine::CommandKind::StartRecording, mode, reason);
    });
}

ce_status_t ce_recording_stop(ce_runtime_t* rt, const char* reason, ce_request_t* out) {
    return ce::capi::RunCommand("ce_recording_stop", rt, out, [&](ce::engine::Command& command) {
        return ce::capi::RecordingCommand(command, ce::engine::CommandKind::StopRecording, CE_MODE_VIDEO, reason);
    });
}

ce_status_t ce_recording_toggle(ce_runtime_t* rt, int32_t mode, const char* reason, ce_request_t* out) {
    return ce::capi::RunCommand("ce_recording_toggle", rt, out, [&](ce::engine::Command& command) {
        return ce::capi::RecordingCommand(command, ce::engine::CommandKind::ToggleRecording, mode, reason);
    });
}

ce_status_t ce_overlay_toggle(ce_runtime_t* rt, ce_request_t* out) {
    return ce::capi::RunCommand("ce_overlay_toggle", rt, out, [](ce::engine::Command& command) {
        return ce::capi::PlainCommand(command, ce::engine::CommandKind::ToggleOverlay);
    });
}

ce_status_t ce_benchmark_toggle(ce_runtime_t* rt, ce_request_t* out) {
    return ce::capi::RunCommand("ce_benchmark_toggle", rt, out, [](ce::engine::Command& command) {
        return ce::capi::PlainCommand(command, ce::engine::CommandKind::ToggleBenchmark);
    });
}

ce_status_t ce_screenshot_take(ce_runtime_t* rt, ce_request_t* out) {
    return ce::capi::RunCommand("ce_screenshot_take", rt, out, [](ce::engine::Command& command) {
        return ce::capi::PlainCommand(command, ce::engine::CommandKind::TakeScreenshot);
    });
}

ce_status_t ce_target_launch(ce_runtime_t* rt, const char* command_line, ce_request_t* out) {
    return ce::capi::RunCommand("ce_target_launch", rt, out, [&](ce::engine::Command& command) -> ce_status_t {
        command.kind = ce::engine::CommandKind::LaunchTarget;
        if (!command_line)
            return CE_E_INVALID_ARGUMENT;
        return ce::capi::ReadClientText(command_line, ce::capi::kMaxCommandLineBytes, command.text);
    });
}

// The action is checked at admission (an unknown code is CE_E_INVALID_ARGUMENT, before READY is looked at).
ce_status_t ce_setup_request(ce_runtime_t* rt, int32_t action, ce_request_t* out) {
    return ce::capi::RunCommand("ce_setup_request", rt, out, [action](ce::engine::Command& command) -> ce_status_t {
        command.kind = ce::engine::CommandKind::Setup;
        command.setupAction = action;
        return CE_OK;
    });
}

}  // extern "C"
