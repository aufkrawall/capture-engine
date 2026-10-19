#include "status_overlay_sync.h"

#include <windows.h>

#include <atomic>

#include "common/logging/logging.h"

namespace ce::status_overlay {

namespace {

std::atomic<uint32_t> g_ControllerPid{0};

HANDLE OpenStatusEvent(bool ackEvent, DWORD access) {
    const uint32_t controllerPid = g_ControllerPid.load(std::memory_order_acquire);
    if (controllerPid == 0) {
        return nullptr;
    }

    wchar_t eventName[64] = {};
    if (ackEvent) {
        GenerateStatusOverlayDarkAckEventName(eventName, 64, controllerPid);
    } else {
        GenerateStatusOverlaySyncEventName(eventName, 64, controllerPid);
    }
    return OpenEventW(access, FALSE, eventName);
}

// The consumer's published endpoints. The controller-side overlay creates both events when its
// status-sync watcher starts and closes them when it stops, so holding both is exactly "a status
// consumer is alive"; either one missing means there is none to wait for.
struct ConsumerEndpoints {
    HANDLE ack = nullptr;
    HANDLE sync = nullptr;

    bool Present() const {
        return ack && sync;
    }
    ~ConsumerEndpoints() {
        if (ack) {
            CloseHandle(ack);
        }
        if (sync) {
            CloseHandle(sync);
        }
    }
};

}  // namespace

const char* DarkHandshakeName(DarkHandshake handshake) {
    switch (handshake) {
        case DarkHandshake::kNoConsumer:
            return "no_consumer";
        case DarkHandshake::kAcknowledged:
            return "acknowledged";
        case DarkHandshake::kTimedOut:
            return "timed_out";
    }
    return "unknown";
}

void SetControllerPid(uint32_t controllerPid) {
    g_ControllerPid.store(controllerPid, std::memory_order_release);
}

void SignalSync() {
    HANDLE syncEvent = OpenStatusEvent(false, EVENT_MODIFY_STATE);
    if (!syncEvent) {
        return;
    }
    SetEvent(syncEvent);
    CloseHandle(syncEvent);
}

DarkHandshake RequestDarkForCapture(CaptureState& runtimeState, const char* reason, uint32_t ackTimeoutMs) {
    runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagStatusOverlayDarkForCapture, true);

    ConsumerEndpoints consumer;
    consumer.ack = OpenStatusEvent(true, EVENT_MODIFY_STATE | SYNCHRONIZE);
    consumer.sync = OpenStatusEvent(false, EVENT_MODIFY_STATE);
    if (!consumer.Present()) {
        LogInfo("[StatusOverlayDark] handshake=%s waitedMs=0 (%s): No controller status consumer; capture starts "
                "without a dark handshake",
                DarkHandshakeName(DarkHandshake::kNoConsumer), reason ? reason : "capture start");
        return DarkHandshake::kNoConsumer;
    }

    // Drop any acknowledgement left over from an earlier request before asking again.
    ResetEvent(consumer.ack);
    SetEvent(consumer.sync);
    const uint64_t waitStartTick = GetTickCount64();
    const DWORD wait = WaitForSingleObject(consumer.ack, ackTimeoutMs);
    const uint64_t waitedMs = GetTickCount64() - waitStartTick;
    if (wait == WAIT_OBJECT_0) {
        LogInfo("[StatusOverlayDark] handshake=%s waitedMs=%llu (%s): Status overlay confirmed dark",
                DarkHandshakeName(DarkHandshake::kAcknowledged), static_cast<unsigned long long>(waitedMs),
                reason ? reason : "capture start");
        return DarkHandshake::kAcknowledged;
    }
    LogWarn(
        "[StatusOverlayDark] handshake=%s waitedMs=%llu timeoutMs=%u (%s): No dark acknowledgement (result=%lu); "
        "capture starts anyway and the first frames may still show the recording-start status",
        DarkHandshakeName(DarkHandshake::kTimedOut), static_cast<unsigned long long>(waitedMs), ackTimeoutMs,
        reason ? reason : "capture start", wait);
    return DarkHandshake::kTimedOut;
}

void ReleaseDarkForCapture(CaptureState& runtimeState, const char* reason) {
    const bool wasSet = runtimeState.HasRuntimeFlag(kCaptureRuntimeFlagStatusOverlayDarkForCapture);
    runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagStatusOverlayDarkForCapture, false);
    if (wasSet) {
        LogInfo("[StatusOverlayDark] Released (%s)", reason ? reason : "unspecified");
    }
}

}  // namespace ce::status_overlay
