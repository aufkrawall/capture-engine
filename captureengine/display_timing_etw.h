#pragma once

#include <windows.h>
#include <evntrace.h>
#include <tdh.h>

#include <algorithm>
#include <array>
#include <cstddef>
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

inline constexpr uint16_t kRuntimePresentStart = 0x2a;
inline constexpr uint16_t kRuntimeMpoPresentStart = 0x37;
inline constexpr uint16_t kQueuePacketStart = 0xb2;
inline constexpr uint16_t kMmioFlip = 0x74;
inline constexpr uint16_t kMmioMpoFlip = 0x103;
inline constexpr uint16_t kVsync = 0x11;
inline constexpr uint16_t kVsyncMpo = 0x111;
inline constexpr uint16_t kHsyncMpo = 0x17e;
inline constexpr uint16_t kMpoPresentIds = 0x182;
inline constexpr uint16_t kGeneratedFlip = 0x2;
inline constexpr uint16_t kNvidiaFlipRequest = 0x1;

inline constexpr ULONGLONG kRuntimeKeyword = 0x8000000000000002ull;
inline constexpr ULONGLONG kGraphicsKernelKeyword = 0x1;
inline constexpr ULONGLONG kFrameTypeKeyword = 0x1;
inline constexpr ULONGLONG kNvidiaDisplayKeyword = 0x1000000000000000ull;

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

inline TracePropertiesBuffer MakeProperties(const wchar_t* name) noexcept {
    const std::size_t nameBytes = (std::wcslen(name) + 1) * sizeof(wchar_t);
    TracePropertiesBuffer buffer;
    auto* properties = buffer.Get();
    properties->Wnode.BufferSize = static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES) + nameBytes);
    properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    properties->Wnode.ClientContext = 1;
    properties->BufferSize = 64;
    properties->MinimumBuffers = 4;
    properties->MaximumBuffers = 16;
    properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING;
    properties->FlushTimer = 1;
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    std::memcpy(buffer.storage.data() + properties->LoggerNameOffset, name, nameBytes);
    return buffer;
}

inline ULONG EnableFilteredProvider(TRACEHANDLE session, const GUID& provider, ULONGLONG keywords,
                                    std::initializer_list<USHORT> eventIds) {
    const std::size_t filterSize = offsetof(EventIdFilter, events) + eventIds.size() * sizeof(USHORT);
    std::vector<std::byte> filterStorage(filterSize);
    auto* filter = reinterpret_cast<EventIdFilter*>(filterStorage.data());
    filter->filterIn = TRUE;
    filter->reserved = 0;
    filter->count = static_cast<USHORT>(eventIds.size());
    std::copy(eventIds.begin(), eventIds.end(), filter->events);

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

}  // namespace display_timing_etw
