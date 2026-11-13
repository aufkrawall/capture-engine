#pragma once

#include <windows.h>
#include <evntrace.h>
#include <tdh.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <limits>
#include <vector>

// Provider identity and real-time session plumbing for the screen-change timing
// service. Kept apart from the reducer so the correlation logic in
// display_timing_service.cpp stays readable next to it.
namespace display_timing_etw {

// Microsoft-Windows-DXGI: the runtime present that starts a frame.
inline constexpr GUID kRuntimeProvider = {0xca11c036, 0x0102, 0x4a2d, {0xa6, 0xad, 0xf0, 0x3c, 0xfe, 0xd5, 0xd3, 0xc9}};
// Microsoft-Windows-DxgKrnl: kernel submission and flip completion.
inline constexpr GUID kGraphicsKernelProvider = {
    0x802ec45a, 0x1e99, 0x4b83, {0x99, 0x20, 0x87, 0xc9, 0x82, 0x77, 0xba, 0x9d}};
// Intel-PresentMon: the frame-type announcement Intel XeSS-FG and AMD AFMF emit.
// NVIDIA frame generation does not use it; see kNvidiaDisplayProvider.
inline constexpr GUID kFrameTypeProvider = {
    0xecaa4712, 0x4644, 0x442f, {0xb9, 0x4c, 0xa3, 0x2f, 0x6c, 0xf8, 0xa4, 0x99}};
// NVIDIA display driver: announces the time a flip is scheduled to reach the
// screen. This is the only source of paced screen times under DLSS frame
// generation, where the kernel flip events are emitted in bursts.
inline constexpr GUID kNvidiaDisplayProvider = {
    0xae4f8626, 0x8265, 0x40d1, {0xa7, 0x0b, 0x11, 0xb6, 0x42, 0x40, 0xe8, 0xe9}};

// Microsoft-Windows-Win32k: RetrieveInputMessage, emitted on the thread that
// takes an input message out of its queue (the event PresentMon's
// input-to-photon metrics use). The PC-latency estimate anchors on it.
inline constexpr GUID kWin32kProvider = {0x8c416c79, 0xd49b, 0x4f01, {0xa4, 0x67, 0xe5, 0x6d, 0x3a, 0xa8, 0x23, 0x4c}};

// Microsoft-Windows-Kernel-Process: process start events for sub-millisecond
// injection detection before the game's entry point and slInit.
inline constexpr GUID kKernelProcessProvider = {
    0x22fb2cd6, 0x0e7b, 0x422b, {0xa0, 0xc7, 0x2f, 0xad, 0x1f, 0xd0, 0xe7, 0x16}};

inline constexpr uint16_t kRuntimePresentStart = 0x2a;
inline constexpr uint16_t kRuntimeMpoPresentStart = 0x37;
// The matching returns. Display timing ignores them; the window heartbeat's
// stall detector needs to know which Present calls never came back.
inline constexpr uint16_t kRuntimePresentStop = 0x2b;
inline constexpr uint16_t kRuntimeMpoPresentStop = 0x38;
// dxgi!EventOverrideSyncInterval (task SwapChain, level LogAlways, the runtime keyword): Windows' "Variable refresh
// rate" graphics setting replaced a present's SyncInterval=1 on a variable-refresh output. It fires only for such
// presents; see captureengine/display_timing/display_timing_vrr_override.h.
inline constexpr uint16_t kRuntimeOverrideSyncInterval = 0x4a;
// Every runtime event a display-timing session takes.
inline constexpr std::array<USHORT, 5> kRuntimeEventIds = {kRuntimePresentStart, kRuntimeMpoPresentStart,
                                                          kRuntimePresentStop, kRuntimeMpoPresentStop,
                                                          kRuntimeOverrideSyncInterval};
inline constexpr uint16_t kQueuePacketStart = 0xb2;
// QueuePacket_Stop: the packet finished; carries SubmitSequence only.
inline constexpr uint16_t kQueuePacketStop = 0xb4;
inline constexpr uint16_t kMmioFlip = 0x74;
inline constexpr uint16_t kMmioMpoFlip = 0x103;
inline constexpr uint16_t kVsync = 0x11;
inline constexpr uint16_t kVsyncMpo = 0x111;
inline constexpr uint16_t kHsyncMpo = 0x17e;
inline constexpr uint16_t kMpoPresentIds = 0x182;
inline constexpr uint16_t kGeneratedFlip = 0x2;
inline constexpr uint16_t kNvidiaFlipRequest = 0x1;
inline constexpr uint16_t kRetrieveInputMessage = 0x3f;
// Win32k composition tokens (PresentMon's Microsoft_Windows_Win32k.h): created when a process presents to a windowed
// surface, and changing state until the compositor retires or discards them.
inline constexpr uint16_t kWin32kTokenCreated = 0x00c9;
inline constexpr uint16_t kWin32kTokenStateChanged = 0x012d;
inline constexpr uint16_t kKernelProcessStartEventId = 1;

inline constexpr ULONGLONG kRuntimeKeyword = 0x8000000000000002ull;
inline constexpr ULONGLONG kGraphicsKernelKeyword = 0x1;
inline constexpr ULONGLONG kFrameTypeKeyword = 0x1;
inline constexpr ULONGLONG kNvidiaDisplayKeyword = 0x1000000000000000ull;
// RetrieveInputMessage's own keyword set (MessagePump and its companions).
inline constexpr ULONGLONG kWin32kInputKeyword = 0x0400000040400000ull;
// The two token events' keywords: TokenStateChanged 0x8000000000001000, TokenCompositionSurfaceObject
// 0x8000000400001000 (PresentMon's descriptors); the mask covers both.
inline constexpr ULONGLONG kWin32kTokenKeyword = 0x8000000400001000ull;
inline constexpr ULONGLONG kKernelProcessKeyword = 0x10;

// FlipEntryStatusAfterFlip values that defer the screen time to the matching
// ?SyncDPC event instead of completing at the flip itself.
inline constexpr uint32_t kFlipWaitVSync = 5;
inline constexpr uint32_t kFlipWaitHSync = 15;

inline constexpr std::size_t kTraceSessionNameCapacity = 64;
inline constexpr ULONG kEventIdFilterType = 0x80000200;
inline constexpr ULONG kIgnoreZeroKeywordEvents = 0x00000010;

struct EventIdFilter {
    BOOLEAN filterIn;
    UCHAR reserved;
    USHORT count;
    USHORT events[1];
};

struct alignas(EVENT_TRACE_PROPERTIES) TracePropertiesBuffer {
    std::array<std::byte, sizeof(EVENT_TRACE_PROPERTIES) + kTraceSessionNameCapacity * sizeof(wchar_t)> storage{};

    EVENT_TRACE_PROPERTIES* Get() noexcept {
        return reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage.data());
    }
};

template <typename T>
bool ReadProperty(const EVENT_RECORD* event, const wchar_t* name, T& value,
                  ULONG arrayIndex = std::numeric_limits<ULONG>::max()) {
    PROPERTY_DATA_DESCRIPTOR descriptor = {};
    descriptor.PropertyName = reinterpret_cast<ULONGLONG>(name);
    descriptor.ArrayIndex = arrayIndex;
    ULONG size = sizeof(value);
    return TdhGetProperty(const_cast<EVENT_RECORD*>(event), 0, nullptr, 1, &descriptor, size,
                          reinterpret_cast<PBYTE>(&value)) == ERROR_SUCCESS;
}

// The two Win32k token events' payloads (PresentMon's Microsoft_Windows_Win32k.h, both version 0), as the provider
// writes them: packed, the pointers as wide as the emitting process's. Read straight from the event's user data, as
// PresentMon does: the state events fire for every windowed present on the machine and a property-by-property decode
// of each is not worth its cost.
// Version 1 (Windows 11 build 26300, 56-byte payloads in a 2026-11-11 GTA trace) only appends fields -
// TokenCompositionSurfaceObject FlipInterval/DestWidth/DestHeight, TokenStateChanged EarlyComposition - so the prefix
// below reads both. Rejecting version 1 had left every token count at zero on that build. A later version may move
// fields; it is refused and counted (DisplayTimingHealth::tokensUndecodable) rather than misread.
inline constexpr UCHAR kWin32kTokenNewestDecodableVersion = 1;

inline bool IsDecodableWin32kTokenVersion(UCHAR version) noexcept {
    return version <= kWin32kTokenNewestDecodableVersion;
}
#pragma pack(push, 1)
template <typename PointerT>
struct Win32kTokenCreatedPayload {
    PointerT token;
    PointerT surface;
    uint32_t swapChainIndex;
    uint64_t presentCount;
    uint64_t surfaceLuid;
    uint64_t bindId;
};

template <typename PointerT>
struct Win32kTokenStatePayload {
    PointerT surface;
    uint32_t swapChainIndex;
    uint32_t presentCount;
    uint64_t fenceValue;
    uint32_t newState;
    uint32_t independentFlip;
    uint32_t skipIndependentFlip;
    uint64_t surfaceLuid;
    uint64_t bindId;
};
#pragma pack(pop)

struct Win32kTokenCreated {
    uint64_t surfaceLuid = 0;
    uint64_t presentCount = 0;
    uint64_t bindId = 0;
};

struct Win32kTokenState {
    uint64_t surfaceLuid = 0;
    uint32_t presentCount = 0;
    uint64_t bindId = 0;
    uint32_t newState = 0;
    bool independentFlip = false;  // the present went to the screen by independent flip, not composition
};

namespace detail {
template <typename PointerT>
bool DecodeTokenCreated(const void* data, std::size_t length, Win32kTokenCreated& out) {
    Win32kTokenCreatedPayload<PointerT> payload;
    if (!data || length < sizeof(payload))
        return false;
    std::memcpy(&payload, data, sizeof(payload));
    out = {payload.surfaceLuid, payload.presentCount, payload.bindId};
    return true;
}

template <typename PointerT>
bool DecodeTokenState(const void* data, std::size_t length, Win32kTokenState& out) {
    Win32kTokenStatePayload<PointerT> payload;
    if (!data || length < sizeof(payload))
        return false;
    std::memcpy(&payload, data, sizeof(payload));
    out = {payload.surfaceLuid, payload.presentCount, payload.bindId, payload.newState, payload.independentFlip != 0};
    return true;
}
}  // namespace detail

// False when the payload is shorter than its layout. `pointer64`: the event header's 64-bit flag.
inline bool DecodeWin32kTokenCreated(const void* data, std::size_t length, bool pointer64, Win32kTokenCreated& out) {
    return pointer64 ? detail::DecodeTokenCreated<uint64_t>(data, length, out)
                     : detail::DecodeTokenCreated<uint32_t>(data, length, out);
}

inline bool DecodeWin32kTokenState(const void* data, std::size_t length, bool pointer64, Win32kTokenState& out) {
    return pointer64 ? detail::DecodeTokenState<uint64_t>(data, length, out)
                     : detail::DecodeTokenState<uint32_t>(data, length, out);
}

#ifndef EVENT_TRACE_USE_MS_FLUSH_TIMER
#define EVENT_TRACE_USE_MS_FLUSH_TIMER 0x00000010
#endif

inline TracePropertiesBuffer MakeProperties(const wchar_t* name, ULONG flushTimerMs = 1) noexcept {
    const std::size_t rawLen = name ? std::wcslen(name) : 0;
    const std::size_t clampedLen = (std::min)(rawLen, kTraceSessionNameCapacity - 1);
    const std::size_t nameBytes = clampedLen * sizeof(wchar_t);
    TracePropertiesBuffer buffer;
    auto* properties = buffer.Get();
    properties->Wnode.BufferSize = static_cast<ULONG>(buffer.storage.size());
    properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    properties->Wnode.ClientContext = 1;
    properties->BufferSize = 64;
    properties->MinimumBuffers = 4;
    properties->MaximumBuffers = 16;
    // A failed write to another collector must not suppress this healthy session's events.
    // Native Presents/flips vanished with successful startup until this isolation was enabled.
    properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING |
                              EVENT_TRACE_INDEPENDENT_SESSION_MODE | EVENT_TRACE_USE_MS_FLUSH_TIMER;
    properties->FlushTimer = flushTimerMs;
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    if (clampedLen > 0) {
        std::memcpy(buffer.storage.data() + properties->LoggerNameOffset, name, nameBytes);
    }
    *reinterpret_cast<wchar_t*>(buffer.storage.data() + properties->LoggerNameOffset + nameBytes) = L'\0';
    return buffer;
}

// ETW takes at most this many ids in one event-id filter (MAX_EVENT_FILTER_EVENT_ID_COUNT).
inline constexpr std::size_t kMaximumFilteredEventIds = 64;

inline ULONG EnableFilteredProvider(TRACEHANDLE session, const GUID& provider, ULONGLONG keywords,
                                    const USHORT* eventIds, std::size_t eventIdCount) {
    const std::size_t filterSize = offsetof(EventIdFilter, events) + eventIdCount * sizeof(USHORT);
    std::vector<std::byte> filterStorage(filterSize);
    auto* filter = reinterpret_cast<EventIdFilter*>(filterStorage.data());
    filter->filterIn = TRUE;
    filter->reserved = 0;
    filter->count = static_cast<USHORT>(eventIdCount);
    std::copy(eventIds, eventIds + eventIdCount, filter->events);

    EVENT_FILTER_DESCRIPTOR descriptor = {};
    descriptor.Ptr = reinterpret_cast<ULONGLONG>(filter);
    descriptor.Size = static_cast<ULONG>(filterStorage.size());
    descriptor.Type = kEventIdFilterType;

    ENABLE_TRACE_PARAMETERS parameters = {};
    parameters.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    parameters.EnableProperty = kIgnoreZeroKeywordEvents;
    parameters.EnableFilterDesc = &descriptor;
    parameters.FilterDescCount = 1;
    return EnableTraceEx2(session, &provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE, keywords, 0, 0,
                          &parameters);
}

inline ULONG EnableFilteredProvider(TRACEHANDLE session, const GUID& provider, ULONGLONG keywords,
                                    std::initializer_list<USHORT> eventIds) {
    return EnableFilteredProvider(session, provider, keywords, eventIds.begin(), eventIds.size());
}

inline ULONG EnableRuntimePresentProvider(TRACEHANDLE session) {
    return EnableFilteredProvider(session, kRuntimeProvider, kRuntimeKeyword, kRuntimeEventIds.data(),
                                  kRuntimeEventIds.size());
}

struct DisplayTimingProviderStatus {
    // The DXGI runtime and graphics kernel providers; without them there is no
    // screen-change timing at all.
    ULONG required = ERROR_SUCCESS;
    ULONG generatedFrames = ERROR_SUCCESS;
    ULONG nvidiaSchedule = ERROR_SUCCESS;
    ULONG inputRetrieval = ERROR_SUCCESS;
    ULONG kernelProcess = ERROR_SUCCESS;
};

// Which half of the provider set a session enables. A CE-owned display session
// takes All. The elevation service's shared session keeps ProcessStart on for
// as long as anyone leases it, because process discovery needs nothing else,
// and adds Display only while a display-timing consumer holds a lease: every
// Display provider fires in every presenting, GPU-submitting or input-reading
// process on the machine.
enum class ProviderGroup { All, Display, ProcessStart };

// The one provider set every display-timing session enables, whether CE opens
// it itself or the elevation service owns it. The service once kept its own
// copy of this list, and the Win32k input provider added to CE's copy never
// reached it: a whole Talos session (20261006_150600) consumed the service's
// trace and saw input(retrievals=0). Optional providers are only attempted once
// the required ones are on. For ProcessStart alone, `required` reports the
// process provider.
inline DisplayTimingProviderStatus EnableDisplayTimingProviders(TRACEHANDLE session,
                                                                ProviderGroup group = ProviderGroup::All) {
    DisplayTimingProviderStatus status;
    if (group != ProviderGroup::ProcessStart) {
        status.required = EnableRuntimePresentProvider(session);
        if (status.required == ERROR_SUCCESS) {
            status.required = EnableFilteredProvider(session, kGraphicsKernelProvider, kGraphicsKernelKeyword,
                                                     {kQueuePacketStart, kQueuePacketStop, kMmioFlip, kMmioMpoFlip,
                                                      kVsync, kVsyncMpo, kHsyncMpo, kMpoPresentIds});
        }
        if (status.required != ERROR_SUCCESS)
            return status;
        status.generatedFrames =
            EnableFilteredProvider(session, kFrameTypeProvider, kFrameTypeKeyword, {kGeneratedFlip});
        // Absent on non-NVIDIA adapters, where flip event timestamps already are
        // the screen times.
        status.nvidiaSchedule =
            EnableFilteredProvider(session, kNvidiaDisplayProvider, kNvidiaDisplayKeyword, {kNvidiaFlipRequest});
        // Without it the PC-latency estimate models the input-to-Present span for
        // games with no latency markers instead of measuring it. The composition-token events ride the same
        // enable (one provider enable per session): they say how the compositor ended a followed process's
        // presents, which the flip stream cannot (display_timing_tokens.h).
        status.inputRetrieval = EnableFilteredProvider(session, kWin32kProvider,
                                                       kWin32kInputKeyword | kWin32kTokenKeyword,
                                                       {kRetrieveInputMessage, kWin32kTokenCreated,
                                                        kWin32kTokenStateChanged});
    }
    if (group != ProviderGroup::Display) {
        status.kernelProcess =
            EnableFilteredProvider(session, kKernelProcessProvider, kKernelProcessKeyword, {kKernelProcessStartEventId});
        if (group == ProviderGroup::ProcessStart)
            status.required = status.kernelProcess;
    }
    return status;
}

// Turns the Display half off again on a session that stays open for process
// discovery. Disabling a provider that never got enabled is harmless.
inline void DisableDisplayTimingProviders(TRACEHANDLE session) {
    for (const GUID& provider :
         {kRuntimeProvider, kGraphicsKernelProvider, kFrameTypeProvider, kNvidiaDisplayProvider, kWin32kProvider}) {
        EnableTraceEx2(session, &provider, EVENT_CONTROL_CODE_DISABLE_PROVIDER, 0, 0, 0, 0, nullptr);
    }
}

}  // namespace display_timing_etw
