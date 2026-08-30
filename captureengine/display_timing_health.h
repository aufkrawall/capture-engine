#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Which kernel event carried a frame's screen time. Reported per window because
// only the immediate flip path can take NVIDIA's scheduled-flip announcement,
// so the split is what says whether that correction reaches the published
// series at all on a given machine and present mode.
enum class DisplayCompletionSource : std::size_t {
    VSyncDpc = 0,
    SyncDpcMultiPlane = 1,
    ImmediateFlip = 2,
    ImmediateMultiPlaneFlip = 3,
    Count = 4,
};

// One window's counters, snapshotted under the lock and formatted outside it.
struct DisplayTimingHealth {
    uint64_t presents = 0;
    uint64_t associations = 0;
    uint64_t queued = 0;
    uint64_t published = 0;
    uint64_t suppressed = 0;
    uint64_t regressed = 0;
    uint64_t payloadReceived = 0;
    uint64_t payloadValid = 0;
    uint64_t payloadCorrelated = 0;
    uint64_t payloadPending = 0;
    uint64_t payloadPendingObserved = 0;
    uint64_t authoritative = 0;
    uint64_t payloadDuplicate = 0;
    uint64_t payloadLate = 0;
    uint64_t fallbackPublished = 0;
    uint64_t fallbackSuppressed = 0;
    uint64_t nvReceived = 0;
    uint64_t nvUndecodable = 0;
    uint64_t nvApplied = 0;
    int64_t nvAverageDelayUs = 0;
    int64_t nvMaxDelayUs = 0;
    std::array<uint64_t, static_cast<std::size_t>(DisplayCompletionSource::Count)> completions = {};
};
