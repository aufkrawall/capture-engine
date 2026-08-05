#pragma once

#include <cstdint>

#include "../common/shared_defs.h"

// Media-side half of the recording-status overlay protocol.
//
// Screen-grab capture records the composited desktop, so any CE-owned recording-start
// status still on screen when the capture pipeline arms is recorded with it. Publishing
// the status change when file output goes live is far too late: the WGC/DXGI look-ahead
// reservoir handed to the live output was captured a reservoir-length earlier, and the
// controller-side pseudo-overlay would only notice the transition on its next poll.
//
// Media therefore publishes the capture-dark request before capture starts and waits for
// the controller's proof that its status left the composited screen. Every endpoint is
// fail-open: a missing or unresponsive consumer costs a bounded wait, never a blocked
// recording start.
namespace ce::status_overlay {

// Bounded wait for the controller's dark acknowledgement.
constexpr uint32_t kDarkAckTimeoutMs = 300;

// Controller PID authenticated by the IPC startup handshake. It keys the event pair, so
// the protocol can never bind to a foreign process. Until it is set, every call below is
// a no-op.
void SetControllerPid(uint32_t controllerPid);

// Wake the controller-side overlay so it re-reads the published recording status now
// instead of on its next poll.
void SignalSync();

// Publish the capture-dark request and wait (bounded) until the controller confirms its
// recording-start status is off the composited screen.
void RequestDarkForCapture(CaptureState& runtimeState, const char* reason);

// Clear the request. Called from every publication that resolves the pending start:
// live recording, a terminal failure, or a stop.
void ReleaseDarkForCapture(CaptureState& runtimeState, const char* reason);

}  // namespace ce::status_overlay
