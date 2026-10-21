// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// The recording recovery's run on the engine (libcengine 11, part 3): a worker thread repairs what the in-flight
// journal names, the engine thread publishes each repaired file as CE_EVENT_RECORDING_RECOVERED. One worker at a time;
// a trigger while it runs schedules one more pass. Shutdown cancels it: a canceled file stays in the journal.
#include "runtime/core/engine_internal.h"

#include "common/logging/logging.h"
#include "runtime/core/event_draft.h"

#include <cstdio>
#include <exception>

namespace ce::engine {

void Engine::StartRecovery(const char* reason) {
    if (!parts_.recoverRecordings || stopping_)
        return;
    if (recovery_.worker.joinable()) {
        recovery_.rerun = true;
        return;
    }
    recovery_.done.store(false, std::memory_order_relaxed);
    recovery_.cancel = 0;
    recovery_.results.clear();
    LogInfo("[RecordingRecovery] pass started (%s)", reason ? reason : "-");
    try {
        recovery_.worker = std::thread([this]() {
            try {
                recovery_.results = parts_.recoverRecordings(&recovery_.cancel);
            } catch (const std::exception& error) {
                LogError("[RecordingRecovery] pass failed: %s", error.what());
            } catch (...) {
                LogError("[RecordingRecovery] pass failed");
            }
            recovery_.done.store(true, std::memory_order_release);
            if (shared_.wake)
                SetEvent(shared_.wake);  // the engine collects on its next loop turn
        });
    } catch (const std::exception& error) {
        LogError("[RecordingRecovery] the recovery thread could not start: %s", error.what());
    }
}

void Engine::CollectRecovery() {
    if (!recovery_.worker.joinable() || !recovery_.done.load(std::memory_order_acquire))
        return;
    recovery_.worker.join();
    for (const RecoveredRecordingInfo& recording : recovery_.results) {
        char message[96];
        snprintf(message, sizeof(message), "recovered an interrupted recording (tracks end at %lld ms)",
                 static_cast<long long>(recording.endUs / 1000));
        // Guaranteed delivery when a slot is free; a full queue still coalesces the newest repair into state.
        EventQueue::Reservation slot = shared_.events->Reserve(recording.outputUtf8.size() + sizeof(message) + 2);
        const EventDraft draft = RecordingRecoveredEvent(recording.outputUtf8, message);
        if (!slot || !shared_.events->PushReserved(std::move(slot), draft))
            shared_.events->PushState(draft);
    }
    recovery_.results.clear();
    if (recovery_.rerun) {
        recovery_.rerun = false;
        StartRecovery("rerun");
    }
}

void Engine::StopRecovery() {
    if (!recovery_.worker.joinable())
        return;
    recovery_.cancel = 1;
    recovery_.worker.join();
    recovery_.results.clear();
    recovery_.rerun = false;
    LogInfo("[RecordingRecovery] pass canceled at shutdown; unfinished files stay for the next run");
}

}  // namespace ce::engine
