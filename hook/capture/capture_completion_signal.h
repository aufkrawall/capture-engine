#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <utility>

// When the shared capture's completion Signal goes on the game's queue.
//
// The capture copy is recorded right before the game's Present. Signalling the
// shared fence there puts a CE Signal between CE's ECL and the game's Present on
// the game queue: the shape NVIDIA's driver answers with a stall, and the reason
// the overlay already defers its own fence Signal past Present
// (dx12_hook_process_session_draw_submission.cpp). Heroes of the Storm (DXUP on
// D3D12, 140 fps limiter) showed it only while inject capture ran: Present
// down-calls of 2-9 ms on 4-12% of recorded frames, nearly every >9 ms frame
// interval next to one, in every build back to 0.1.7157 and on drivers 621.01
// and 621.12 (llm-wiki/log/recent.md, 2026-10-28).
enum class CaptureCompletionSignal : uint8_t {
    kImmediate,     // Signal right after the copy
    kAfterPresent,  // held until the Present that follows the copy was forwarded
};

constexpr const char* CaptureCompletionSignalName(CaptureCompletionSignal signal) {
    return signal == CaptureCompletionSignal::kAfterPresent ? "after-present" : "immediate";
}

// Held only where the Present that follows the copy is the game's own, on this
// thread, through CE's detour: the foreground route without frame generation or
// runtime-owned presentation. Those routes keep the immediate Signal they were
// validated with; unfocused D3D12 presentation keeps it for its focus-loss
// fence waits.
constexpr CaptureCompletionSignal ResolveCaptureCompletionSignal(bool processHasForeground, bool frameGenerationActive,
                                                                 bool runtimeOwnsPresentation) {
    return processHasForeground && !frameGenerationActive && !runtimeOwnsPresentation
               ? CaptureCompletionSignal::kAfterPresent
               : CaptureCompletionSignal::kImmediate;
}

// One held completion Signal: the copy's queue and the fence value media waits
// for. Work the queue receives between Hold and Release (the game's Present)
// runs before the fence reaches the value, so media sees the frame that much
// later and nothing else changes. At most one is held: the owner releases it
// before the next copy and before any reset, so a Present path that never
// reaches the post-Present boundary costs one frame of latency, never a slot.
class HeldCompletionSignal {
public:
    bool Holding() const { return value_ != 0; }
    uint64_t Value() const { return value_; }

    // Precondition: nothing held (the owner releases first).
    void Hold(ID3D12CommandQueue* queue, uint64_t value) {
        queue_ = queue;
        value_ = value;
    }

    // Queues the held Signal; S_OK when nothing is held. The hold ends either
    // way: a failed Signal means the queue or its device is gone.
    HRESULT Release(ID3D12Fence* fence) {
        if (value_ == 0)
            return S_OK;
        const Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue = std::move(queue_);
        const uint64_t value = std::exchange(value_, uint64_t{0});
        if (!queue || !fence)
            return E_POINTER;
        return queue->Signal(fence, value);
    }

    // Process teardown: the driver may already be gone, so no COM call.
    void Abandon() {
        queue_.Detach();
        value_ = 0;
    }

private:
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    uint64_t value_ = 0;
};

// Copy-fence progress as the Present-stall attribution reads it around the
// game's Present (present_stall_attribution.h).
struct CaptureCopyProgress {
    bool valid = false;            // a capture generation with a live fence
    uint64_t submitted = 0;        // latest fence value a copy was submitted for
    uint64_t completed = 0;        // the fence's completed value
    uint32_t framesCaptured = 0;   // copies submitted by this generation
    bool signalHeld = false;       // the latest copy's Signal waits for the Present
};
