#pragma once

// Opening the screen-change timing session and turning on the providers it
// reads.
//
// Split out of display_timing_service.cpp, which had reached the 800-line
// ceiling. The service's own translation unit is the correlation reducer; the
// ETW enablement sequence is plumbing that only runs once, so it lives here.

#include <windows.h>
#include <evntrace.h>

namespace ce::display_timing_startup {

// Opens the service's real-time session - reclaiming any leaked by CaptureEngine
// processes that were killed - and enables every provider the reducer reads.
// Returns ERROR_SUCCESS, or the status of the first required step that refused.
//
// The DXGI runtime and DxgKrnl providers are required: without them there are no
// presents to correlate and no flips to complete them. The Intel-PresentMon
// frame-type provider and the NVIDIA display provider are optional - the first is
// absent unless a frame generator that announces frame types is running, the
// second on any non-NVIDIA adapter - so a refusal there is logged and the session
// keeps running with whatever the kernel events alone can say.
//
// On failure the session is closed again, so the caller never has to unwind it.
ULONG OpenSessionAndEnableProviders(TRACEHANDLE* session, const wchar_t* sessionName);

// Explains a startup failure to the log. The screen-change path fails silently
// otherwise: the overlay simply falls back to presentation timing, which looks
// like a frame-time graph that is subtly wrong rather than like an error.
void LogStartupFailure(ULONG error);

}  // namespace ce::display_timing_startup
