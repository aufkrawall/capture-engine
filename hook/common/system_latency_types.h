#pragma once

// Shared vocabulary for the PC-latency estimate: the published snapshot, the
// per-frame low-latency report the graphics runtimes hand back, and the labels
// used by the overlay and the diagnostic log.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ce::system_latency {

enum class Source : uint8_t {
    Unavailable,
    ReflexMarkers,
    Estimated,
};

// Where the simulation-start anchor of a measured frame came from. The value is
// diagnostic: it tells a reader how much of the published number was measured
// rather than modelled from the frame cadence.
//
// Only one producer qualifies. A Present returning looks like a frame boundary
// but is not one: the wrapper is entered more than once per displayed frame in
// several configurations (2.3x measured on a 144 Hz Talos session), and under
// frame generation the Present that returns belongs to the generator's pacing
// thread rather than to the application's frame.
enum class FrameBeginKind : uint8_t {
    // No boundary observed; the frame's CPU work is modelled as one interval.
    Modelled,
    // A low-latency runtime's per-frame sleep returned. The application calls
    // it immediately before sampling input, so it is the closest observable
    // simulation start. It is not used to count application frames: some
    // integrations emit waits at output cadence.
    LowLatencySleepReturn,
};

struct Snapshot {
    float milliseconds = 0.0f;
    Source source = Source::Unavailable;
    uint32_t sampleCount = 0;
    bool valid = false;
    // Distribution of the same window, for diagnostics only. The overlay shows
    // the trimmed mean; a wide min/max spread is how a broken correlation
    // announces itself.
    float medianMilliseconds = 0.0f;
    float minimumMilliseconds = 0.0f;
    float maximumMilliseconds = 0.0f;
    // Nominal FG telemetry is not proof that generated frames are reaching the
    // screen. These flags let the overlay disclose an idle/broken generator
    // instead of presenting its low no-FG latency as an FG result.
    bool frameGenerationConfigured = false;
    bool frameGenerationStateKnown = false;
    bool frameGenerationObserved = false;
};

// Rolling evidence about how the last published samples were produced. Every
// field is a running total unless documented otherwise.
struct Diagnostics {
    uint64_t displaysObserved = 0;
    // Displays whose sensor-side runtime PresentStart association was present.
    // The association is what makes present->screen a causal measurement
    // instead of a guess, so a low ratio invalidates comparisons.
    uint64_t displaysWithPresentAssociation = 0;
    uint64_t displaysWithoutMatchedPresent = 0;
    uint64_t presentsDroppedUnderContention = 0;
    uint64_t samplesRejectedOutOfRange = 0;
    uint64_t sourceTransitions = 0;
    // Components of the most recently accepted fallback sample, in
    // microseconds. Zero until one has been produced.
    int64_t lastAnchorToPresentUs = 0;
    int64_t lastPresentToDisplayUs = 0;
    int64_t lastInputWaitUs = 0;
    int64_t lastBaseIntervalUs = 0;
    // Cadence of the authoritative streams. Application Present is classified
    // at the API boundary; frame begin is the input-sampling anchor paired with
    // it when a low-latency sleep was observed.
    int64_t displayIntervalUs = 0;
    int64_t applicationIntervalUs = 0;
    int64_t frameBeginIntervalUs = 0;
    int64_t markerIntervalUs = 0;
    // Measured output/application cadence ratio. 1000 means no generated
    // output, 2000/3000/4000 are the expected steady 2x/3x/4x families.
    int observedOutputRatioPermille = 0;
    bool frameGenerationObserved = false;
    bool markerCadenceTrusted = true;
    // A generator is holding the application frame back behind the frames it
    // derived from it, so the anchor was stepped one application frame older.
    bool generatorHoldApplied = false;
    FrameBeginKind lastFrameBeginKind = FrameBeginKind::Modelled;
    // Whether the most recent marker-sourced sample was matched through the
    // sensor's runtime-Present association rather than by screen time alone.
    bool lastMarkerUsedAssociation = false;
    // The source that was NOT published, when it also holds a fresh window.
    // Both estimate the same quantity, so a large disagreement means one of
    // the two correlations is wrong and the published number cannot be
    // compared against a reading taken under a different configuration.
    Source crossCheckSource = Source::Unavailable;
    float crossCheckMilliseconds = 0.0f;
    uint64_t markerReportsRejectedForOutputCadence = 0;
    uint64_t measurementEpochResets = 0;
};

struct NativeFrameReport {
    uint64_t frameId = 0;
    uint64_t inputSampleTimeUs = 0;
    uint64_t simulationStartTimeUs = 0;
    uint64_t presentStartTimeUs = 0;
    uint64_t gpuRenderEndTimeUs = 0;
};

struct NativeReport {
    static constexpr size_t kCapacity = 64;
    std::array<NativeFrameReport, kCapacity> frames{};
    size_t count = 0;
};

using SupplementalNativeReportProvider = bool (*)(NativeReport& report);

// Implemented separately for the injected DirectX hook and Vulkan layer.
bool QueryNativeReport(void* device, NativeReport& report);

namespace detail {
inline std::atomic<SupplementalNativeReportProvider>& SupplementalNativeReportProviderSlot() {
    static std::atomic<SupplementalNativeReportProvider> provider{nullptr};
    return provider;
}
}  // namespace detail

// Header-inline so the injected hook and the Vulkan layer each hold their own
// process-wide slot without either having to link the other's report backend.
inline void SetSupplementalNativeReportProvider(SupplementalNativeReportProvider provider) {
    detail::SupplementalNativeReportProviderSlot().store(provider, std::memory_order_release);
}

// A registered provider serves cross-IHV marker reports without a graphics
// device, so callers must not gate the telemetry poll on having resolved one.
inline SupplementalNativeReportProvider GetSupplementalNativeReportProvider() {
    return detail::SupplementalNativeReportProviderSlot().load(std::memory_order_acquire);
}

// A window of measurements older than this no longer describes the current
// configuration, so it is discarded rather than blended into a new one.
inline constexpr int64_t kSampleFreshnessUs = 2'000'000;

inline const char* SourceLogLabel(Source source) {
    switch (source) {
        case Source::ReflexMarkers:
            return "Reflex/PCL markers (input wait estimated)";
        case Source::Estimated:
            return "presentation/display estimate";
        default:
            return "unavailable";
    }
}

inline const char* SourceOverlayLabel(Source source) {
    switch (source) {
        case Source::ReflexMarkers:
            return "PC Latency~";
        case Source::Estimated:
            return "Latency est.";
        default:
            return "PC Latency";
    }
}

inline const char* SnapshotOverlayLabel(const Snapshot& snapshot) {
    if (snapshot.frameGenerationConfigured && snapshot.frameGenerationStateKnown &&
        !snapshot.frameGenerationObserved) {
        return snapshot.source == Source::ReflexMarkers ? "PC Latency~ (FG idle)" : "Latency est. (FG idle)";
    }
    return SourceOverlayLabel(snapshot.source);
}

inline const char* FrameBeginKindLabel(FrameBeginKind kind) {
    switch (kind) {
        case FrameBeginKind::LowLatencySleepReturn:
            return "low-latency-sleep";
        default:
            return "modelled";
    }
}

}  // namespace ce::system_latency
