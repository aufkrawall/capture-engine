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
// A Present returning looks like a frame boundary but is not one: the wrapper is
// entered more than once per displayed frame in several configurations (2.3x
// measured on a 144 Hz Talos session), and under
// frame generation the Present that returns belongs to the generator's pacing
// thread rather than to the application's frame.
enum class FrameBeginKind : uint8_t {
    // No boundary observed; the frame's CPU work is modelled as one interval.
    Modelled,
    // A low-latency runtime's per-frame sleep returned. The application calls
    // it immediately before sampling input, so it is the closest observable
    // simulation start. It is not used to count application frames: some
    // integrations emit waits at output cadence.
    //
    // Only usable when it returned on the thread that presents: an engine whose
    // game thread runs ahead of its render thread sleeps for frame N+1 or N+2
    // before frame N is presented, so the newest sleep says nothing about the
    // frame being presented (see FrameBeginObservation::sleepOnOtherThread).
    LowLatencySleepReturn,
    // The game's own SimulationStart marker, paired with its PresentStart
    // marker by frame ID. Unlike the sleep this is frame-identity exact, so it
    // holds for engines whose game thread runs ahead of the presenting thread.
    SimulationMarker,
    // The presenting thread's last input-message retrieval before this
    // application Present (Win32k ETW). The point the frame read its input.
    InputRetrieval,
    // No boundary for this frame: the median of this thread's recent measured
    // input-to-Present spans. Only used while those measurements are fresh.
    Learned,
};

inline constexpr size_t kFrameBeginKindCount = 5;

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
    uint64_t samplesRejectedPresentToDisplay = 0;
    uint64_t samplesRejectedBaseInterval = 0;
    uint64_t samplesRejectedTotalLatency = 0;
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
    // Displays that reached the screen between a frame's runtime PresentStart
    // and its own display, median and maximum over the recent window: the flip
    // or render queue it waited behind. Vsync capping the frame rate is a full
    // queue here; zero means each frame went straight to the screen.
    int framesQueuedAhead = 0;
    int framesQueuedAheadMax = 0;
    int64_t applicationIntervalUs = 0;
    int64_t frameBeginIntervalUs = 0;
    int64_t markerIntervalUs = 0;
    // Measured output/application cadence ratio. 1000 means no generated
    // output, 2000/3000/4000 are the expected steady 2x/3x/4x families.
    int observedOutputRatioPermille = 0;
    // Application frames handed to a pacing generator that had not yet reached
    // the screen. Zero means the queue state is not measurable from the current
    // epoch, not that the queue is empty; the correlator then holds the
    // documented single-frame hold.
    uint32_t applicationFramesInFlight = 0;
    // Whether the application-source Present stream is still arriving. A stale
    // stream's cadence is ignored, so applicationIntervalUs then reports the
    // marker or published-base cadence that replaced it.
    bool applicationPresentStreamFresh = false;
    bool frameGenerationObserved = false;
    bool markerCadenceTrusted = true;
    // A generator is holding the application frame back behind the frames it
    // derived from it, so the anchor was stepped one application frame older.
    bool generatorHoldApplied = false;
    // Whether that hold was measured against the application's own Present, or
    // modelled as one output interval because no application frame reached the
    // correlator. A runtime that presents from its own thread hides the
    // application Present from the present hook, and the modelled form is only
    // a floor - so the two must never be reported as the same thing. Estimate
    // path only; the marker path carries the game's own timestamps.
    bool generatorHoldMeasured = false;
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
    // Queue seeds abandoned because conservation left the physically possible range
    // (more than the maximum depth in flight, or more retired than presented): frames
    // the generator discarded (FSR FG's warm-up after switching on) or displays the
    // stream never delivered. The single-frame hold stands until the next seed.
    uint64_t queueDepthCountsRejected = 0;
    // Accepted estimate samples by the kind of anchor that produced them,
    // indexed by FrameBeginKind. One window mixes them, so the published value
    // is only as measured as this distribution says.
    std::array<uint64_t, kFrameBeginKindCount> anchorKindSamples{};
    // Application frames whose newest low-latency sleep returned on another
    // thread than the Present and was therefore not used as their anchor.
    uint64_t sleepAnchorsOnOtherThread = 0;
    // SimulationStart markers rejected as an anchor because their PresentStart
    // marker predates the previous application Present (another frame's).
    uint64_t markerAnchorsStale = 0;
    // Input-retrieval bursts received from the sensor, and frames that had
    // none on their presenting thread but some on another thread - an engine
    // that reads input on its game thread and presents from a render thread,
    // where the retrieval cannot be paired with the presented frame.
    uint64_t inputRetrievalsObserved = 0;
    uint64_t inputRetrievalFramesOnOtherThread = 0;
    // Marker-anchored application frames by where the game emitted the
    // PresentStart marker (see MarkerThread), and the median span from that
    // marker to the application Present it was paired with. A marker on the
    // presenting thread a few microseconds before Present brackets the call as
    // the Reflex contract says; anything else may pair a later frame's markers.
    uint64_t markerOnPresentingThread = 0;
    uint64_t markerOnOtherThread = 0;
    int64_t markerToPresentUs = 0;
    // Generator outputs whose application frame was found by the generator's
    // own frame ID (FSR FG), outputs that carried an ID no observed application
    // Present had, and the median number of newer application frames already
    // presented when an output carrying an older one went out: the generator's
    // queue, measured by identity rather than by counting.
    uint64_t generatorFramesMatchedById = 0;
    uint64_t generatorFramesUnmatchedById = 0;
    int generatorQueueDepthById = 0;
    // Half the display's scanout period, added to every sample: the average
    // time from the screen-time event until the scan reaches a pixel. Zero
    // until the sensor reported the refresh period.
    int64_t scanoutToCenterUs = 0;
    // The estimate's anchor-to-runtime-Present span split at the application
    // Present: the game's own part (frame boundary to its Present, measured
    // frames only) and what happened after it (a generator's or interposer's
    // hold, or CE's own pacing wait). Medians; 0 when not observed.
    int64_t anchorToApplicationPresentUs = 0;
    int64_t applicationToRuntimePresentUs = 0;
};

// Cadences measured from the streams themselves, for frame-generation readouts
// that would otherwise repeat a runtime-reported figure. Zero when the stream
// behind one is not currently arriving.
struct MeasuredRates {
    int64_t outputIntervalUs = 0;
    int64_t sourceIntervalUs = 0;
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

// Logarithmic band of a published reading, 25 % wide: the overlay logs a
// sample whenever the band changes, so a reading the user saw move is on
// record even between the periodic samples. 0 for no reading.
inline uint32_t LatencyLogBand(float milliseconds) {
    if (!(milliseconds > 0.0f))
        return 0;
    float edge = 1.0f;
    for (uint32_t band = 1; band < 64; ++band) {
        const float next = edge * 1.25f;
        if (next > milliseconds)
            return band;
        edge = next;
    }
    return 64;
}

inline const char* SnapshotOverlayLabel(const Snapshot& snapshot) {
    return SourceOverlayLabel(snapshot.source);
}

inline const char* FrameBeginKindLabel(FrameBeginKind kind) {
    switch (kind) {
        case FrameBeginKind::LowLatencySleepReturn:
            return "low-latency-sleep";
        case FrameBeginKind::SimulationMarker:
            return "simulation-marker";
        case FrameBeginKind::InputRetrieval:
            return "input-retrieval";
        case FrameBeginKind::Learned:
            return "learned";
        default:
            return "modelled";
    }
}

}  // namespace ce::system_latency
