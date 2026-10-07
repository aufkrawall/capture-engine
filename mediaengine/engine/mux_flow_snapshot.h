#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ce::media {

// Versioned DLL output: the mux writer's cumulative counters for the current
// recording. Counter reads are atomic; the API holds the engine lifetime lock. Byte counts are packet payload; the queue figures use
// the queue's own accounting (payload plus per-packet overhead) so fill against the
// limit stays exact. A counter moving backwards means a new recording started.
struct alignas(8) MuxFlowSnapshotV1 {
    uint32_t size = sizeof(MuxFlowSnapshotV1);
    uint32_t valid = 0;
    uint64_t enqueuedBytes = 0;
    uint64_t writtenBytes = 0;
    uint64_t writerBusyUs = 0;
    uint64_t queuedBytes = 0;
    uint64_t queueLimitBytes = 0;
};

static_assert(sizeof(MuxFlowSnapshotV1) == 48);
static_assert(alignof(MuxFlowSnapshotV1) == 8);
static_assert(offsetof(MuxFlowSnapshotV1, enqueuedBytes) == 8);
static_assert(offsetof(MuxFlowSnapshotV1, queueLimitBytes) == 40);
static_assert(std::is_standard_layout_v<MuxFlowSnapshotV1>);
static_assert(std::is_trivially_copyable_v<MuxFlowSnapshotV1>);

}  // namespace ce::media
