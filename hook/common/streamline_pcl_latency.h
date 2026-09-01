#pragma once

#include "system_latency_metrics.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ce::system_latency {

// Lock-free capture for the two game-owned PCL markers needed by the overlay's
// display correlator. Marker callbacks are latency-sensitive and may arrive on
// different game threads, while the overlay reads the completed history only
// four times per second.
class PclMarkerHistory {
public:
    static constexpr uint32_t kSimulationStartMarker = 0;
    static constexpr uint32_t kPresentStartMarker = 4;

    bool Record(uint32_t marker, uint64_t frameId, int64_t timestampUs) noexcept {
        if (timestampUs <= 0)
            return false;

        Slot& slot = slots_[frameId % slots_.size()];
        if (marker == kSimulationStartMarker) {
            // Clear completion before publishing a reused slot. A reader that
            // still sees the old frame ID therefore rejects the partial pair.
            slot.presentStartTimeUs.store(0, std::memory_order_relaxed);
            slot.simulationStartTimeUs.store(timestampUs, std::memory_order_relaxed);
            slot.frameId.store(frameId, std::memory_order_release);
            return true;
        }
        if (marker != kPresentStartMarker || slot.frameId.load(std::memory_order_acquire) != frameId)
            return false;

        const int64_t simulationStartUs = slot.simulationStartTimeUs.load(std::memory_order_relaxed);
        if (simulationStartUs <= 0 || timestampUs < simulationStartUs)
            return false;
        slot.presentStartTimeUs.store(timestampUs, std::memory_order_release);
        return true;
    }

    bool BuildReport(NativeReport& report) const noexcept {
        report = {};
        std::array<NativeFrameReport, kSlotCount> completed{};
        size_t completedCount = 0;

        for (const Slot& slot : slots_) {
            const uint64_t frameIdBefore = slot.frameId.load(std::memory_order_acquire);
            const int64_t simulationStartUs = slot.simulationStartTimeUs.load(std::memory_order_relaxed);
            const int64_t presentStartUs = slot.presentStartTimeUs.load(std::memory_order_acquire);
            const uint64_t frameIdAfter = slot.frameId.load(std::memory_order_acquire);
            if (frameIdBefore != frameIdAfter || simulationStartUs <= 0 || presentStartUs < simulationStartUs)
                continue;

            NativeFrameReport& frame = completed[completedCount++];
            frame.frameId = frameIdAfter;
            frame.simulationStartTimeUs = static_cast<uint64_t>(simulationStartUs);
            frame.presentStartTimeUs = static_cast<uint64_t>(presentStartUs);
        }

        if (completedCount == 0)
            return false;
        std::sort(completed.begin(), completed.begin() + completedCount,
                  [](const NativeFrameReport& lhs, const NativeFrameReport& rhs) {
                      return lhs.presentStartTimeUs < rhs.presentStartTimeUs;
                  });

        const size_t retainedCount = (std::min)(completedCount, report.frames.size());
        const size_t firstRetained = completedCount - retainedCount;
        for (size_t i = 0; i < retainedCount; ++i)
            report.frames[i] = completed[firstRetained + i];
        report.count = retainedCount;
        return true;
    }

    bool BuildFreshReport(NativeReport& report, int64_t currentQpcUs,
                          int64_t maximumAgeUs = 2'000'000) const noexcept {
        if (!BuildReport(report) || currentQpcUs <= 0 || maximumAgeUs < 0) {
            report = {};
            return false;
        }

        const uint64_t newestPresentTimeUs = report.frames[report.count - 1].presentStartTimeUs;
        const uint64_t currentTimeUs = static_cast<uint64_t>(currentQpcUs);
        if (newestPresentTimeUs > currentTimeUs) {
            report = {};
            return false;
        }
        const uint64_t ageUs = currentTimeUs - newestPresentTimeUs;
        if (ageUs > static_cast<uint64_t>(maximumAgeUs)) {
            report = {};
            return false;
        }
        return true;
    }

    void Reset() noexcept {
        for (Slot& slot : slots_) {
            slot.presentStartTimeUs.store(0, std::memory_order_relaxed);
            slot.simulationStartTimeUs.store(0, std::memory_order_relaxed);
            slot.frameId.store(0, std::memory_order_release);
        }
    }

private:
    static constexpr size_t kSlotCount = 128;

    struct Slot {
        std::atomic<uint64_t> frameId{0};
        std::atomic<int64_t> simulationStartTimeUs{0};
        std::atomic<int64_t> presentStartTimeUs{0};
    };

    std::array<Slot, kSlotCount> slots_{};
};

}  // namespace ce::system_latency
