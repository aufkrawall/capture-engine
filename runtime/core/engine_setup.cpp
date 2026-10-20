// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// The platform setup command of the engine (llm-wiki/library/03 "Platform setup", DR-27). A setup request is admitted
// like any command. The engine starts its action on the engine thread, collects the result in the service step and
// publishes the one COMMAND_COMPLETED then. Removing the sensor driver stops the sensor helper first and holds it
// stopped until the action is over, so no health tick restarts a helper whose driver is being removed.
#include "runtime/core/engine_internal.h"

#include "common/logging/logging.h"

#include <windows.h>

#include <string>

namespace ce::engine {
namespace {

// A declined prompt (UAC) is CE_E_CANCELED; every other failure is CE_E_PROCESS with the Win32 code in the message.
ce_status_t StatusOf(unsigned long win32Result) {
    switch (win32Result) {
        case ERROR_SUCCESS:
            return CE_OK;
        case ERROR_BUSY:
            return CE_E_BUSY;
        case ERROR_CANCELLED:
            return CE_E_CANCELED;
        default:
            return CE_E_PROCESS;
    }
}

std::string MessageOf(unsigned long win32Result) {
    return win32Result == ERROR_SUCCESS ? std::string() : "Windows error " + std::to_string(win32Result);
}

}  // namespace

void Engine::StartSetup(PendingCommand& pending) {
    if (!setupActions_) {
        shared_.events->PushReserved(std::move(pending.completion),
                                     CommandCompletedEvent(pending.request, CE_E_UNSUPPORTED,
                                                           "platform setup is not available in this runtime"));
        return;
    }
    if (setupActions_->Busy()) {
        // Checked before the sensor helper is touched: a refused request must not stop anything.
        shared_.events->PushReserved(std::move(pending.completion),
                                     CommandCompletedEvent(pending.request, CE_E_BUSY,
                                                           "another setup action is in flight"));
        return;
    }
    const auto action = static_cast<ce::runtime::SetupAction>(pending.command.setupAction);
    const bool removesDriver = action == ce::runtime::SetupAction::SensorDriverUninstall;
    if (removesDriver && !children_->StopSensorsForSetup()) {
        // The helper is still running: nothing is started, and the hold ends with this request.
        children_->ResumeSensorsAfterSetup();
        LogWarn("[Setup] request=%llu sensor helper did not exit; driver removal not started",
                static_cast<unsigned long long>(pending.request));
        shared_.events->PushReserved(std::move(pending.completion),
                                     CommandCompletedEvent(pending.request, CE_E_PROCESS,
                                                           "sensor helper did not exit; the driver was not removed"));
        return;
    }
    const unsigned long started = setupActions_->Start(pending.request, action);
    if (started != ERROR_SUCCESS) {
        if (removesDriver)
            children_->ResumeSensorsAfterSetup();
        shared_.events->PushReserved(std::move(pending.completion),
                                     CommandCompletedEvent(pending.request, StatusOf(started),
                                                           MessageOf(started)));
        return;
    }
    setupPending_ = SetupPending{pending.request, std::move(pending.completion)};
}

void Engine::CollectSetup() {
    if (!setupActions_ || !setupPending_)
        return;
    for (const ce::runtime::SetupCompletion& done : setupActions_->Collect())
        FinishSetup(done.win32Result);
}

void Engine::FinishSetup(unsigned long win32Result) {
    // The sensor hold ends with its action, whatever the result: the next health tick restarts the helper according to
    // the configuration. A helper that was never held is not affected.
    if (children_)
        children_->ResumeSensorsAfterSetup();
    SetupPending finished = std::move(*setupPending_);
    setupPending_.reset();
    shared_.events->PushReserved(std::move(finished.completion),
                                 CommandCompletedEvent(finished.request, StatusOf(win32Result), MessageOf(win32Result)));
}

void Engine::ShutdownSetup() {
    if (!setupActions_)
        return;
    // Waits for an action in flight (the same wait the frontend had), then completes it with its real result.
    const std::optional<ce::runtime::SetupCompletion> done = setupActions_->Shutdown();
    if (!setupPending_)
        return;
    if (done) {
        FinishSetup(done->win32Result);
        return;
    }
    if (children_)
        children_->ResumeSensorsAfterSetup();
    SetupPending finished = std::move(*setupPending_);
    setupPending_.reset();
    shared_.events->PushReserved(std::move(finished.completion),
                                 CommandCompletedEvent(finished.request, CE_E_INVALID_STATE,
                                                       "the setup action has no result at shutdown"));
}

}  // namespace ce::engine
