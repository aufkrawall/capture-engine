#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// When the GPU ran the inject capture's copy, against the game's Present call.
//
// The `[PRESENT STALL]` copy-fence probe went blind once the completion Signal
// was held past Present (capture_completion_signal.h): at the down-call the
// frame's copy is unsignalled by construction. Heroes of the Storm still
// stalled its Present 2-5 ms on ~1 frame in 7-10 with the Signal held, and the
// frame reached the display 3-4 ms after the call (llm-wiki/log/recent.md,
// 2026-10-28). Two GPU timestamps bracket the copy inside its own command list
// (the first lands once the work queued before it has drained, the second once
// the copy finished) and are resolved into a readback buffer in that same list:
// no extra submission, Signal or Wait on the game's queue. The queue's clock
// calibration maps them onto QueryPerformanceCounter, so each captured Present
// can be told whether its down-call returned when the GPU reached or finished
// the copy, or waited on something the copy does not explain.
//
// Every call happens under the owning capture's state lock.

// A GPU timestamp's QPC value: the queue's clock calibration taken near it.
struct GpuClockCalibration {
    uint64_t gpuTicks = 0;
    int64_t qpcTicks = 0;
    uint64_t gpuFrequency = 0;
    int64_t qpcFrequency = 0;
};

// The calibration point moved by the tick distance in each clock's own rate. Exact
// enough within the recalibration interval (well under a microsecond of drift).
inline int64_t MapGpuTicksToQpc(const GpuClockCalibration& calibration, uint64_t gpuTicks) {
    if (calibration.gpuFrequency == 0 || calibration.qpcFrequency <= 0) {
        return 0;
    }
    const double gpuDelta = gpuTicks >= calibration.gpuTicks ? static_cast<double>(gpuTicks - calibration.gpuTicks)
                                                             : -static_cast<double>(calibration.gpuTicks - gpuTicks);
    const double scale = static_cast<double>(calibration.qpcFrequency) / static_cast<double>(calibration.gpuFrequency);
    return calibration.qpcTicks + static_cast<int64_t>(gpuDelta * scale);
}

// The game's Present down-call that followed a copy, as the stall probe timed it.
struct CapturePresentDownCall {
    int64_t entryQpc = 0;
    int64_t exitQpc = 0;
    bool slow = false;         // a captured down-call at or above the slow threshold
    uint64_t slowOrdinal = 0;  // its ordinal among slow captured calls (present_stall_attribution.h)
};

// One copy whose GPU timestamps are readable.
struct CaptureCopyGpuSample {
    bool presentNoted = false;     // false: the copy's Present never reached the probe
    bool timestampsValid = false;  // false: unreadable or out of order
    int64_t copyStartQpc = 0;      // the GPU reached the copy
    int64_t copyEndQpc = 0;        // the copy, its barriers and nothing after it finished
    CapturePresentDownCall present;
};

class CaptureCopyGpuTimer {
public:
    // Device objects of one generation, handed to the generation's retirement
    // while a copy that writes them may still execute.
    struct Resources {
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> queryHeap;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        void Detach() {
            queryHeap.Detach();
            readback.Detach();
        }
    };

    // Recalibrated after this long, or when the copy moves to another queue.
    static constexpr int64_t kCalibrationIntervalMs = 1000;

    // Two timestamp queries and a 16-byte readback row per slot. False leaves
    // the timer off; the capture runs unchanged.
    bool Initialize(ID3D12Device* device, uint32_t slots);
    bool Enabled() const { return resources_.queryHeap != nullptr; }

    // Into the slot's copy list: Begin first, End after the copy's last barrier
    // (it also records the resolve). Begin returns false while disabled.
    bool BeginCopy(ID3D12GraphicsCommandList* list, uint32_t slot);
    void EndCopy(ID3D12GraphicsCommandList* list, uint32_t slot);
    // After the list was executed on `queue`: the fence value whose completion
    // makes the slot's row readable. `nowQpc` is the hook clock.
    void Submitted(ID3D12CommandQueue* queue, uint32_t slot, uint64_t fenceValue, int64_t nowQpc);

    // The Present down-call after the latest submitted copy. nullptr: a down-call
    // that followed no copy, so a copy still waiting lost its Present (another
    // path presented it) and is handed over unpaired rather than mismatched.
    void NotePresent(const CapturePresentDownCall* present);
    // Hands over up to `capacity` slots whose fence value completed; the rest
    // wait for the next call. Returns the number written.
    size_t Harvest(uint64_t completedFenceValue, CaptureCopyGpuSample* out, size_t capacity);

    // End of a generation. Retire: the records go, the device objects are
    // returned for the retired generation to hold. Reset releases them now.
    // Abandon detaches them (process teardown, device removal).
    Resources Retire();
    void Reset();
    void Abandon();

private:
    enum class SlotState : uint8_t { kEmpty, kSubmitted, kPresentNoted };
    struct SlotRecord {
        SlotState state = SlotState::kEmpty;
        uint64_t fenceValue = 0;
        GpuClockCalibration calibration;
        CapturePresentDownCall present;
    };

    bool Calibrate(ID3D12CommandQueue* queue, int64_t nowQpc);
    CaptureCopyGpuSample ReadSlot(uint32_t slot, const SlotRecord& record);

    Resources resources_;
    std::vector<SlotRecord> slots_;
    int64_t pendingSlot_ = -1;  // submitted, its Present not yet noted
    ID3D12CommandQueue* calibrationQueue_ = nullptr;  // identity only
    GpuClockCalibration calibration_;
    int64_t calibratedAtQpc_ = 0;
};
