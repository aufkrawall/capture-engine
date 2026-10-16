#pragma once

// NVENC on its own D3D11 device.
//
// On NVIDIA the first D3D11 command after an NVENC encode submit on the same
// device waits for that encode. Every per-frame stall found on the encoder
// device came from this (fence Signal, Flush boundary, VideoProcessorBlt, the
// post-submit repeat-cache copy), and after those were removed the timeline
// `submit` step still waited ~7-11 ms GPU against ~0.15 ms CPU (sessions
// 20261016_003600..015331): conv N -> encode N -> [wait encode N] -> conv N+1.
// NVENC then idled for conv N+1 plus CE's turn on the 3D engine after every
// frame, which turns into starvation once a game saturates the 3D engine.
//
// The bridge splits the two roles:
//   - CE's work device keeps every conversion, cursor, repeat-cache and probe
//     command and never sees an NVENC submit;
//   - the encoder device only owns FFmpeg's pool textures (shared NT handles,
//     opened once per texture on the work device) and is driven by NVENC alone;
//   - CE signals a fence on the work device after the last write of a frame and
//     submits that frame only once the CPU observes the fence complete, so no
//     3D-queue command ever sits between two encodes and NVENC's sync with its
//     own device has nothing to wait for. A pool texture is written again only
//     after FFmpeg returned it to the pool (NVENC finished reading it).
// The same boundary (shared texture + fence) is what a D3D12 or Vulkan video
// encoder would import, so the conversion stays on the work device for those.

#include <d3d11_4.h>
#include <windows.h>

#include "encoder_gpu_stage_probe.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct AVFrame;

namespace ce::media {

// MiscFlags of encoder pool textures the work device opens.
inline constexpr UINT kEncoderBridgePoolMiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

// Encoders whose input comes from a device of their own. NVENC only: the
// coupling above is measured there; Quick Sync derives its device from the
// work device, and AMF/Media Foundation keep the shared device until measured.
bool EncoderUsesSeparateDevice(const char* encoderName);

// Frames whose conversion may still run on the GPU while newer frames arrive.
// A submit waits on the CPU only when this many newer frames are pending, so
// the budget covers conversion latency under 3D contention without blocking.
inline constexpr int kDeferredSubmitBudgetMs = 33;
inline constexpr size_t kDeferredSubmitMaxDepth = 8;
size_t DeferredSubmitDepth(int fps);

enum class SubmitGateAction : uint8_t {
    Hold,          // the oldest frame's conversion is still running; send later
    Send,          // the oldest frame is ready
    WaitThenSend,  // over depth (or draining): wait for the oldest, then send
};
SubmitGateAction NextSubmitGateAction(size_t pending, size_t depth, bool frontComplete, bool drainAll);

// A frame handed to the encoder at its work-device fence value, waiting until
// the CPU observes the conversion complete. `frame` is an owned reference.
struct PendingEncoderSubmit {
    AVFrame* frame = nullptr;
    uint64_t fenceValue = 0;
    gpu_stage::Path path = gpu_stage::Path::Fresh;
    const char* label = "";
    bool logPacketDebug = false;
};

// Per-second gate statistics for [VideoEncoder Timing].
struct SubmitGateStats {
    uint64_t waits = 0;
    uint64_t waitAccumUs = 0;
    uint64_t waitMaxUs = 0;
    size_t pendingPeak = 0;
    void Reset(size_t pendingNow) {
        *this = {};
        pendingPeak = pendingNow;
    }
};

class EncoderDeviceBridge {
public:
    EncoderDeviceBridge() = default;
    ~EncoderDeviceBridge();
    EncoderDeviceBridge(const EncoderDeviceBridge&) = delete;
    EncoderDeviceBridge& operator=(const EncoderDeviceBridge&) = delete;

    // Creates the encoder device on the work device's adapter plus the work
    // device's completion fence.
    bool Create(ID3D11Device* workDevice, std::string* error);
    void Reset();
    bool Active() const { return encoderDevice_ != nullptr; }
    ID3D11Device* EncoderDevice() const { return encoderDevice_; }

    // The work-device alias of an encoder pool texture; created once and cached
    // with a reference on both sides. nullptr on failure.
    ID3D11Texture2D* WorkTexture(ID3D11Texture2D* encoderTexture);
    size_t CachedTextureCount() const { return aliases_.size(); }

    // Signals the next fence value after the work queued so far and submits it.
    // Needs the caller's immediate-context lock. 0 on failure.
    uint64_t SignalWorkDone(ID3D11DeviceContext* workContext);
    bool IsComplete(uint64_t value) const;
    enum class WaitResult : uint8_t { Complete, Timeout, DeviceLost, Failed };
    WaitResult Wait(uint64_t value, DWORD timeoutMs);

private:
    struct Alias {
        ID3D11Texture2D* encoderTexture = nullptr;
        ID3D11Texture2D* workTexture = nullptr;
    };
    ID3D11Texture2D* OpenOnWorkDevice(ID3D11Texture2D* encoderTexture, HRESULT* hr);

    ID3D11Device* encoderDevice_ = nullptr;
    ID3D11Device1* workDevice1_ = nullptr;
    ID3D11Fence* fence_ = nullptr;
    HANDLE event_ = nullptr;
    uint64_t signaled_ = 0;
    std::vector<Alias> aliases_;
};

}  // namespace ce::media
