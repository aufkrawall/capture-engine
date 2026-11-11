#pragma once

#include <windows.h>
#include <evntrace.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "common/platform/display_timing_etw.h"

// A bounded raw ETW recording of the display pipeline, kept for scanout-stall diagnosis.
//
// GTA V Enhanced kept presenting at ~131/s for 38 s after a save load while nothing reached the screen
// (20261110_222057). CE's real-time display session decodes only the events its timing needs, so it could tell that the
// tracked presents stopped completing, not where between the runtime, the compositor and the display driver they
// stopped. This recorder writes everything those stages emit - DXGI presents, DxgKrnl submission, history, flips and
// vblanks, DWM composition, Win32k composition tokens and surface bindings, NVIDIA's flip schedule, process starts - into
// a circular file
// (the newest kMaximumFileMegabytes), so the run-up to an episode is in it when the episode is noticed. Sealing stops the
// session (ETW finalizes the file), moves the file aside and starts recording again. Timestamps are QPC
// (ClientContext 1), the clock of CE's own logs.
//
// The recorder needs the right to start a file-mode session for kernel providers. The elevation service owns it for an
// unelevated CaptureEngine; its work file lives in a folder only administrators can write, so nothing can redirect it.
namespace display_flight_recorder {

// Microsoft-Windows-Dwm-Core: the compositor's frame and flip-chain events.
inline constexpr GUID kCompositorProvider = {
    0x9e9bba3c, 0x2e38, 0x40cb, {0x99, 0xf4, 0x9e, 0x82, 0x81, 0x42, 0x51, 0x64}};
// DxgKrnl's Performance channel keyword next to Base: the present-history and flip events PresentMon relies on.
inline constexpr ULONGLONG kGraphicsKernelRecordKeywords = 0x8000000000000001ull;
inline constexpr ULONG kMaximumFileMegabytes = 256;
inline constexpr ULONG kBufferKilobytes = 256;

// Microsoft-Windows-Win32k, by event id: the composition tokens, and the window manager's side of a window's surface -
// which swapchain is bound to which window and when the compositor took it, sprites and logical surfaces, visibility
// regions, focus and fullscreen, and every process's composition batch. GTA's cold DLSS-G load (2026-11-11) showed DWM
// binding the new chain's surface only while processing a batch ~1.4 s later; these events name that batch's sender
// and the binding states. Other Win32k events (messages, input, GDI regions) fire for every window on the machine.
inline constexpr std::array<USHORT, 36> kWindowManagerEvents = {
    2,     // FocusChange
    18,    // SwapChainBind (hWnd, binding)
    19,    // SwapChainUnBind
    26,    // FocusedProcessChange
    27,    // DwmSpriteCreate
    28,    // DwmSpriteDestroy
    29,    // LogicalSurfCreate
    30,    // LogicalSurfDestroy
    31,    // LogicalSurfPhysSurfBind
    32,    // LogicalSurfPhysSurfUnbind
    46,    // BindLogicalSurface (hWnd, surface)
    47,    // UnBindLogicalSurface
    48,    // BindLogicalSurfaceRH
    49,    // DwmSpriteLogicalSurfBind
    50,    // DwmSpriteLogicalSurfUnBind
    52,    // PhysicalSurfCreate
    55,    // SwapChainBindingOpen (size, format, buffer count)
    56,    // SwapChainBindingRelease (compositor status)
    57,    // SwapChainBindingStatus
    58,    // DesktopResolutionFailure
    151,   // DCompCommitBatch (the committing process is the event's)
    201,   // TokenCompositionSurfaceObject
    205,   // ForegroundWindowFullScreen start
    206,   // ForegroundWindowFullScreen stop
    301,   // TokenStateChanged
    400,   // DCompDeferBatch
    413,   // TokenIndependentFlipSkipComplete
    461,   // DwmVisRgnUpdate
    462,   // DwmVisRgnDirty
    487,   // CompositionTargetGlitch
    511,   // FlipManagerContentRebind
    522,   // FlipManagerContentFlip
    525,   // FlipManagerContentUnbind
    528,   // TokenManagerDisableScanoutToken
    551,   // SmoothResizeTimeout
    10002  // WindowLayoutChange
};
static_assert(kWindowManagerEvents.size() <= display_timing_etw::kMaximumFilteredEventIds);
// Updates (tokens, bindings, batches) | Focus | UserActivity | UIUnresponsiveness | Rendering (visibility regions) |
// ResponseTime (layout). The id filter decides; the keywords only have to cover the ids.
inline constexpr ULONGLONG kWindowManagerKeywords =
    display_timing_etw::kWin32kTokenKeyword | 0x2000ull | 0x20000ull | 0x40000ull | 0x100000ull | 0x0001000000000000ull;

struct ProviderStatus {
    ULONG runtime = ERROR_SUCCESS;         // Microsoft-Windows-DXGI
    ULONG graphicsKernel = ERROR_SUCCESS;  // Microsoft-Windows-DxgKrnl
    ULONG compositor = ERROR_SUCCESS;      // Microsoft-Windows-Dwm-Core
    ULONG windowManager = ERROR_SUCCESS;   // Microsoft-Windows-Win32k (kWindowManagerEvents)
    ULONG nvidiaSchedule = ERROR_SUCCESS;  // NVIDIA display driver (absent on other adapters)
    ULONG frameType = ERROR_SUCCESS;       // Intel-PresentMon frame type
    ULONG process = ERROR_SUCCESS;         // Microsoft-Windows-Kernel-Process
    // The recording is only worth its cost with the two graphics stages it exists for.
    bool usable() const noexcept { return runtime == ERROR_SUCCESS && graphicsKernel == ERROR_SUCCESS; }
};

inline ULONG EnableProvider(TRACEHANDLE session, const GUID& provider, UCHAR level, ULONGLONG keywords) {
    return EnableTraceEx2(session, &provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, level, keywords, 0, 0, nullptr);
}

// Unfiltered by event id, unlike the real-time set: the point is the events CE does not decode yet. Win32k stays limited
// to kWindowManagerEvents, its other keywords fire for every window message on the machine.
inline ProviderStatus EnableProviders(TRACEHANDLE session) {
    namespace dte = display_timing_etw;
    ProviderStatus status;
    status.runtime = EnableProvider(session, dte::kRuntimeProvider, TRACE_LEVEL_VERBOSE, dte::kRuntimeKeyword);
    status.graphicsKernel =
        EnableProvider(session, dte::kGraphicsKernelProvider, TRACE_LEVEL_VERBOSE, kGraphicsKernelRecordKeywords);
    status.compositor = EnableProvider(session, kCompositorProvider, TRACE_LEVEL_INFORMATION, ~0ull);
    status.windowManager = dte::EnableFilteredProvider(session, dte::kWin32kProvider, kWindowManagerKeywords,
                                                       kWindowManagerEvents.data(), kWindowManagerEvents.size());
    status.nvidiaSchedule =
        EnableProvider(session, dte::kNvidiaDisplayProvider, TRACE_LEVEL_VERBOSE, dte::kNvidiaDisplayKeyword);
    status.frameType = EnableProvider(session, dte::kFrameTypeProvider, TRACE_LEVEL_VERBOSE, dte::kFrameTypeKeyword);
    status.process =
        EnableProvider(session, dte::kKernelProcessProvider, TRACE_LEVEL_INFORMATION, dte::kKernelProcessKeyword);
    return status;
}

// EVENT_TRACE_PROPERTIES with room for the logger name and the log file path behind it.
class FileProperties {
public:
    FileProperties(const wchar_t* sessionName, const wchar_t* logFile) {
        storage_.resize(sizeof(EVENT_TRACE_PROPERTIES) + 2 * kNameBytes);
        auto* properties = Get();
        properties->Wnode.BufferSize = static_cast<ULONG>(storage_.size());
        properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        properties->Wnode.ClientContext = 1;  // QPC, the clock of CE's logs
        properties->BufferSize = kBufferKilobytes;
        properties->MinimumBuffers = 16;
        properties->MaximumBuffers = 64;
        properties->MaximumFileSize = kMaximumFileMegabytes;
        properties->FlushTimer = 1;
        // A failed write in another collector must not suppress this session's events (display_timing_etw.h).
        properties->LogFileMode = EVENT_TRACE_FILE_MODE_CIRCULAR | EVENT_TRACE_INDEPENDENT_SESSION_MODE;
        properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        properties->LogFileNameOffset = static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES) + kNameBytes);
        Copy(properties->LoggerNameOffset, sessionName);
        Copy(properties->LogFileNameOffset, logFile);
    }
    EVENT_TRACE_PROPERTIES* Get() noexcept { return reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage_.data()); }

private:
    static constexpr std::size_t kNameBytes = (MAX_PATH + 1) * sizeof(wchar_t);
    void Copy(ULONG offset, const wchar_t* text) {
        if (!text)
            return;
        const std::size_t length = (std::min)(std::wcslen(text), static_cast<std::size_t>(MAX_PATH));
        std::memcpy(storage_.data() + offset, text, length * sizeof(wchar_t));
    }
    std::vector<unsigned char> storage_;
};

struct SealResult {
    ULONG status = ERROR_SUCCESS;  // stopping the session and moving its file
    ULONG restartStatus = ERROR_SUCCESS;
    ULONG eventsLost = 0;
    ULONG buffersLost = 0;
    ULONGLONG bytes = 0;
};

// One circular recording. Not thread-safe: its owner serializes Start, Seal and Stop.
class Recorder {
public:
    Recorder() = default;
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    ~Recorder() { Stop(); }

    // A session of the same name left by a crashed owner is stopped first: the name is this owner's alone.
    ULONG Start(const wchar_t* sessionName, const std::wstring& workFile) {
        Stop();
        sessionName_ = sessionName ? sessionName : L"";
        workFile_ = workFile;
        DeleteFileW(workFile_.c_str());
        FileProperties properties(sessionName_.c_str(), workFile_.c_str());
        TRACEHANDLE session = 0;
        ULONG status = StartTraceW(&session, sessionName_.c_str(), properties.Get());
        if (status == ERROR_ALREADY_EXISTS) {
            FileProperties stale(sessionName_.c_str(), nullptr);
            ControlTraceW(0, sessionName_.c_str(), stale.Get(), EVENT_TRACE_CONTROL_STOP);
            FileProperties retry(sessionName_.c_str(), workFile_.c_str());
            status = StartTraceW(&session, sessionName_.c_str(), retry.Get());
        }
        if (status != ERROR_SUCCESS)
            return status;
        session_ = session;
        providers_ = EnableProviders(session_);
        if (!providers_.usable()) {
            const ULONG failure = providers_.runtime != ERROR_SUCCESS ? providers_.runtime : providers_.graphicsKernel;
            Stop();
            return failure;
        }
        return ERROR_SUCCESS;
    }

    // Stops the session (ETW writes the circular file out), moves it to `sealedFile` and records again.
    SealResult Seal(const std::wstring& sealedFile) {
        SealResult result;
        if (!session_) {
            result.status = ERROR_INVALID_STATE;
            return result;
        }
        FileProperties properties(sessionName_.c_str(), nullptr);
        result.status = ControlTraceW(session_, sessionName_.c_str(), properties.Get(), EVENT_TRACE_CONTROL_STOP);
        session_ = 0;
        result.eventsLost = properties.Get()->EventsLost;
        result.buffersLost = properties.Get()->LogBuffersLost + properties.Get()->RealTimeBuffersLost;
        if (result.status == ERROR_SUCCESS) {
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (GetFileAttributesExW(workFile_.c_str(), GetFileExInfoStandard, &attributes))
                result.bytes = (static_cast<ULONGLONG>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
            if (!MoveFileExW(workFile_.c_str(), sealedFile.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                result.status = GetLastError();
        }
        const std::wstring name = sessionName_;
        const std::wstring work = workFile_;
        result.restartStatus = Start(name.c_str(), work);
        return result;
    }

    void Stop() {
        if (session_) {
            FileProperties properties(sessionName_.c_str(), nullptr);
            ControlTraceW(session_, sessionName_.c_str(), properties.Get(), EVENT_TRACE_CONTROL_STOP);
            session_ = 0;
        }
        if (!workFile_.empty())
            DeleteFileW(workFile_.c_str());
    }

    bool Running() const noexcept { return session_ != 0; }
    const ProviderStatus& Providers() const noexcept { return providers_; }

private:
    TRACEHANDLE session_ = 0;
    std::wstring sessionName_;
    std::wstring workFile_;
    ProviderStatus providers_;
};

}  // namespace display_flight_recorder
