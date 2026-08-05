#include "status_overlay_sync.h"

#include <windows.h>

#include <atomic>

#include "../common/logging.h"

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

}  // namespace

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

void RequestDarkForCapture(CaptureState& runtimeState, const char* reason) {
    runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagStatusOverlayDarkForCapture, true);

    HANDLE ackEvent = OpenStatusEvent(true, EVENT_MODIFY_STATE | SYNCHRONIZE);
    HANDLE syncEvent = OpenStatusEvent(false, EVENT_MODIFY_STATE);
    if (!ackEvent || !syncEvent) {
        LogInfo("[StatusOverlayDark] No controller status consumer for %s; capture starts without a dark handshake",
                reason ? reason : "capture start");
        if (ackEvent) {
            CloseHandle(ackEvent);
        }
        if (syncEvent) {
            CloseHandle(syncEvent);
        }
        return;
    }

    // Drop any acknowledgement left over from an earlier request before asking again.
    ResetEvent(ackEvent);
    SetEvent(syncEvent);
    const uint64_t waitStartTick = GetTickCount64();
    const DWORD wait = WaitForSingleObject(ackEvent, kDarkAckTimeoutMs);
    const uint64_t waitedMs = GetTickCount64() - waitStartTick;
    if (wait == WAIT_OBJECT_0) {
        LogInfo("[StatusOverlayDark] Status overlay confirmed dark after %llums (%s)",
                static_cast<unsigned long long>(waitedMs), reason ? reason : "capture start");
    } else {
        LogWarn(
            "[StatusOverlayDark] No dark acknowledgement within %ums (result=%lu, %s); capture starts anyway and the "
            "first frames may still show the recording-start status",
            kDarkAckTimeoutMs, wait, reason ? reason : "capture start");
    }
    CloseHandle(ackEvent);
    CloseHandle(syncEvent);
}

void ReleaseDarkForCapture(CaptureState& runtimeState, const char* reason) {
    const bool wasSet = runtimeState.HasRuntimeFlag(kCaptureRuntimeFlagStatusOverlayDarkForCapture);
    runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagStatusOverlayDarkForCapture, false);
    if (wasSet) {
        LogInfo("[StatusOverlayDark] Released (%s)", reason ? reason : "unspecified");
    }
}

}  // namespace ce::status_overlay
