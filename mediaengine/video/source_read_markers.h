#pragma once

// Completion of the video encoder's GPU reads of inject source slots, owned by
// the encoder thread and published to other threads as a sequence number.
//
// Each fresh inject frame ends a D3D11 event query after its last read of the
// producer's slot. Two earlier designs failed on NVIDIA with NVENC sharing the
// encoder's D3D11 device:
//   - An ID3D11Fence Signal per frame held CE's whole 3D queue for about two
//     encode times (session 20261016_003600, GPU timeline sourceRead=16-26 ms
//     against 0.13 ms on the CPU), so every conversion waited for NVENC.
//   - Polling the queries from the inject thread (GetData/Flush1 on the
//     immediate context) raced NVENC's own use of that context on the encoder
//     thread; the device was removed with DXGI_ERROR_DRIVER_INTERNAL_ERROR
//     after ~60 frames (session 20261016_004939).
// So only the encoder thread touches the queries (between its encoder submits,
// like the stage probe), and other threads see completion through
// SourceReadSequencer: an atomic sequence plus a wake event. Teardown, when no
// encoder submit can run concurrently, waits once on a fence for the rest.

#include <d3d11.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>

namespace ce::media {

class SourceReadSequencer {
public:
    // Encoder thread. Sequences start at 1; 0 means "no read".
    uint64_t Issue() { return issued_.fetch_add(1, std::memory_order_relaxed) + 1; }
    // Encoder thread. Completion is monotonic: queries of one context complete
    // in order, and the encoder only reports a contiguous prefix.
    void CompleteThrough(uint64_t sequence) {
        uint64_t current = completed_.load(std::memory_order_relaxed);
        while (sequence > current &&
               !completed_.compare_exchange_weak(current, sequence, std::memory_order_release,
                                                 std::memory_order_relaxed)) {
        }
        if (sequence <= current)
            return;
        if (HANDLE wake = wake_.load(std::memory_order_acquire))
            SetEvent(wake);
    }
    // Any thread; no D3D11 access.
    bool IsComplete(uint64_t sequence) const {
        return sequence != 0 && sequence <= completed_.load(std::memory_order_acquire);
    }
    // The releasing thread's auto-reset event, signaled whenever completion advances.
    void SetWakeEvent(HANDLE event) { wake_.store(event, std::memory_order_release); }
    uint64_t Issued() const { return issued_.load(std::memory_order_relaxed); }
    uint64_t Completed() const { return completed_.load(std::memory_order_acquire); }

private:
    std::atomic<uint64_t> issued_{0};
    std::atomic<uint64_t> completed_{0};
    std::atomic<HANDLE> wake_{nullptr};
};

// One per mediaengine.dll, so tokens survive VideoEncoder re-creation.
SourceReadSequencer& InjectSourceReadSequencer();

struct SourceReadMarker {
    uint64_t sequence = 0;
    ID3D11Query* query = nullptr;           // owned
    ID3D11DeviceContext* context = nullptr;  // owned; the query's immediate context
};

// Caller holds the immediate-context lock and guarantees no concurrent encoder
// submit (encoder thread, or teardown). Releases completed markers in order and
// reports them; a removed device completes everything it still owns, since no
// read can execute on it any more. Returns the number completed.
size_t PollSourceReadMarkers(std::deque<SourceReadMarker>& markers, SourceReadSequencer& sequencer);

// Teardown only: waits up to `timeoutMs` on one fence signaled behind every
// outstanding marker, then polls. True when nothing is outstanding.
// `lock`/`unlock` guard the immediate context; the wait itself runs unlocked.
bool FinishSourceReadMarkers(std::deque<SourceReadMarker>& markers, SourceReadSequencer& sequencer,
                             DWORD timeoutMs, void (*lock)(), void (*unlock)());

void ReleaseSourceReadMarker(SourceReadMarker& marker);

}  // namespace ce::media
