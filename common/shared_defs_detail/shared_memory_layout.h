#pragma once

#include <intrin.h>  // for _mm_pause
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>  // for memcpy in seqlock helpers
#include <type_traits>

#include "../build_identity.h"

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

#include "frame_ring.h"

// SharedMemoryLayout: the top-level mapped region every process agrees on.

#pragma pack(push, 8)

struct SharedMemoryLayout {
    // ============================================================================
    // Header - MUST be first for version validation before accessing other fields
    // NOTE: Layout changes require a version/name bump. Runtime consumers
    // validate the complete compiled ABI before dereferencing the payload.
    // ============================================================================
    std::atomic<uint32_t> magic{0};                        // Offset 0: Published last after initialization
    std::atomic<uint32_t> version{SHARED_MEMORY_VERSION};  // Offset 4: Layout version
    std::atomic<uint32_t> structSize{0};                   // Offset 8: sizeof(SharedMemoryLayout) for ABI check
    std::atomic<uint32_t> abiSignature{0};                 // Offset 12: Compiled layout fingerprint

    // Atomic access helpers for header fields
    uint32_t GetMagic() const {
        return magic.load(std::memory_order_acquire);
    }
    void SetMagic(uint32_t val) {
        magic.store(val, std::memory_order_release);
    }
    uint32_t GetVersion() const {
        return version.load(std::memory_order_acquire);
    }
    void SetVersion(uint32_t val) {
        version.store(val, std::memory_order_release);
    }

    // Host -> Hook (Host writes, Hook reads - use atomic accessors)
    OverlayConfig overlayConfig;
    // Seqlock for overlayConfig: odd = write in progress, even = stable.
    // Writers call BeginWriteOverlayConfig/EndWriteOverlayConfig.
    // Readers call ReadOverlayConfig() which retries until consistent.
    std::atomic<uint32_t> overlayConfigSeq{0};
    SharedGraphicsConfig graphicsConfig;  // Added graphics overrides

private:
    // Atomic backing fields for thread-safe cross-process access
    std::atomic<uint32_t> hostPID_{0};
    std::atomic<uint32_t> requestExit_{0};
    std::atomic<uint32_t> debugLogging_{0};
    std::atomic<uint32_t> logLevel_{0};
    std::atomic<int32_t> gpuPriority_{0};
    std::atomic<int32_t> copyQueuePriority_{0};
    std::atomic<int32_t> fenceWaitMode_{0};
    std::atomic<uint32_t> useGameQueue_{0};

public:
    char logFilePath[260];  // Path to log file (captureengine.log) - set once at
                            // init

    // Seqlock helpers for overlayConfig.
    // Host writer: BeginWriteOverlayConfig() ... write fields ... EndWriteOverlayConfig()
    // Hook reader: use ReadOverlayConfig() which retries until consistent.
    void BeginWriteOverlayConfig() {
        // Enter write section by transitioning sequence to odd.
        // Use CAS loop to avoid the wrap-around race window where two fetch_add
        // calls leave the sequence momentarily even while the writer is still active.
        uint32_t seq = overlayConfigSeq.load(std::memory_order_relaxed);
        uint32_t desired;
        do {
            desired = seq + 1;
            if ((desired & 1u) == 0u) {
                desired++;  // Skip even values to stay locked (odd)
            }
        } while (!overlayConfigSeq.compare_exchange_weak(seq, desired, std::memory_order_acq_rel,
                                                         std::memory_order_relaxed));
    }
    void EndWriteOverlayConfig() {
        // Publish writer completion by transitioning sequence back to even.
        overlayConfigSeq.fetch_add(1, std::memory_order_release);
    }
    OverlayConfig ReadOverlayConfig() const {
        OverlayConfig result{};
        uint32_t seq1 = 0;
        uint32_t seq2 = 0;
        // Spin limit guards against livelock if a writer stalls/crashes while
        // holding the odd sequence. After the limit, return whatever was last read.
        int spinCount = 0;
        do {
            seq1 = overlayConfigSeq.load(std::memory_order_acquire);
            if (seq1 & 1u) {
                // Writer active — spin briefly then retry
                _mm_pause();
                if (++spinCount > 10000)
                    return result;  // Return empty config; caller should retry later
                continue;
            }
            spinCount = 0;  // Reset on a clean even-seq read
            memcpy(&result, &overlayConfig, sizeof(OverlayConfig));
            seq2 = overlayConfigSeq.load(std::memory_order_acquire);
            if (seq1 != seq2) {
                _mm_pause();
            }
        } while (seq1 != seq2 || (seq2 & 1u));
        return result;
    }

    // Atomic accessors for Host -> Hook fields
    uint32_t GetHostPID() const {
        return hostPID_.load(std::memory_order_acquire);
    }
    void SetHostPID(uint32_t val) {
        hostPID_.store(val, std::memory_order_release);
    }

    bool GetRequestExit() const {
        return requestExit_.load(std::memory_order_acquire) != 0;
    }
    void SetRequestExit(bool val) {
        requestExit_.store(val ? 1u : 0u, std::memory_order_release);
    }

    bool GetDebugLogging() const {
        return debugLogging_.load(std::memory_order_acquire) != 0;
    }
    void SetDebugLogging(bool val) {
        debugLogging_.store(val ? 1u : 0u, std::memory_order_release);
    }

    LogLevel GetLogLevel() const {
        return static_cast<LogLevel>(logLevel_.load(std::memory_order_acquire));
    }
    void SetLogLevel(LogLevel val) {
        logLevel_.store(static_cast<uint32_t>(val), std::memory_order_release);
    }

    int32_t GetGpuPriority() const {
        return gpuPriority_.load(std::memory_order_acquire);
    }
    void SetGpuPriority(int32_t val) {
        gpuPriority_.store(val, std::memory_order_release);
    }

    int32_t GetCopyQueuePriority() const {
        return copyQueuePriority_.load(std::memory_order_acquire);
    }
    void SetCopyQueuePriority(int32_t val) {
        copyQueuePriority_.store(val, std::memory_order_release);
    }

    int32_t GetFenceWaitMode() const {
        return fenceWaitMode_.load(std::memory_order_acquire);
    }
    void SetFenceWaitMode(int32_t val) {
        fenceWaitMode_.store(val, std::memory_order_release);
    }

    bool GetUseGameQueue() const {
        return useGameQueue_.load(std::memory_order_acquire) != 0;
    }
    void SetUseGameQueue(bool val) {
        useGameQueue_.store(val ? 1u : 0u, std::memory_order_release);
    }

    // FPS Limiter Settings (Host -> Hook)
    struct FPSLimiterSettings {
    private:
        std::atomic<uint32_t> captureSyncEnabled_{0};
        std::atomic<int32_t> captureSyncMultiplier_{0};  // 1-8
        std::atomic<uint32_t> generalEnabled_{0};
        std::atomic<int32_t> generalFps_{0};
        std::atomic<int32_t> captureFps_{0};               // Video capture FPS (set when recording starts)
        std::atomic<uint32_t> useVFR_{0};                  // If true, limiter acts as passthrough
        std::atomic<uint32_t> captureSyncLimiterMode_{3};  // LimiterMode enum (default: kAuto=3)
        std::atomic<uint32_t> generalLimiterMode_{3};      // LimiterMode enum (default: kAuto=3)

    public:
        // Atomic accessors
        bool GetCaptureSyncEnabled() const {
            return captureSyncEnabled_.load(std::memory_order_acquire) != 0;
        }
        void SetCaptureSyncEnabled(bool val) {
            captureSyncEnabled_.store(val ? 1u : 0u, std::memory_order_release);
        }

        int32_t GetCaptureSyncMultiplier() const {
            return captureSyncMultiplier_.load(std::memory_order_acquire);
        }
        void SetCaptureSyncMultiplier(int32_t val) {
            captureSyncMultiplier_.store(val, std::memory_order_release);
        }

        bool GetGeneralEnabled() const {
            return generalEnabled_.load(std::memory_order_acquire) != 0;
        }
        void SetGeneralEnabled(bool val) {
            generalEnabled_.store(val ? 1u : 0u, std::memory_order_release);
        }

        int32_t GetGeneralFps() const {
            return generalFps_.load(std::memory_order_acquire);
        }
        void SetGeneralFps(int32_t val) {
            generalFps_.store(val, std::memory_order_release);
        }

        int32_t GetCaptureFps() const {
            return captureFps_.load(std::memory_order_acquire);
        }
        void SetCaptureFps(int32_t val) {
            captureFps_.store(val, std::memory_order_release);
        }

        bool GetUseVFR() const {
            return useVFR_.load(std::memory_order_acquire) != 0;
        }
        void SetUseVFR(bool val) {
            useVFR_.store(val ? 1u : 0u, std::memory_order_release);
        }

        uint32_t GetCaptureSyncLimiterMode() const {
            return captureSyncLimiterMode_.load(std::memory_order_acquire);
        }
        void SetCaptureSyncLimiterMode(uint32_t val) {
            captureSyncLimiterMode_.store(val, std::memory_order_release);
        }

        uint32_t GetGeneralLimiterMode() const {
            return generalLimiterMode_.load(std::memory_order_acquire);
        }
        void SetGeneralLimiterMode(uint32_t val) {
            generalLimiterMode_.store(val, std::memory_order_release);
        }

        // Remote Limiter IPC
        std::atomic<uint32_t> requestCount{0};  // Hook increments to request present
        std::atomic<uint32_t> releaseCount{0};  // Limiter increments to release hook

        // Named event for efficient signaling (hook waits, limiter signals)
        wchar_t releaseEventName[64];  // Name of the release event (created by Limiter)

        // NEW: Request event (Hook signals, Limiter waits)
        wchar_t requestEventName[64];  // Name of the request event (created by Hook
                                       // or Limiter?) -> Created by Limiter

        // Session ID to detect hook restarts
        std::atomic<uint32_t> hookSessionId{0};

        // High-precision sync (Target QPC ticks for next frame)
        std::atomic<int64_t> targetTimeTicks{0};
    } fpsLimiter;

    // Hook -> Host - shared texture ring.
    // Textures swap roles: hook writes to one while encoder reads from another
    // Hook writes, Host reads - use atomic accessors for thread safety
private:
    std::atomic<uint64_t> sharedHandles_[SHARED_TEXTURE_SLOT_COUNT]{};  // HANDLE cast to uint64_t
    std::atomic<uint64_t> fenceShareHandle_{0};
    std::atomic<uint64_t> fenceValue_{0};
    std::atomic<int32_t> currentReadIndex_{0};
    std::atomic<int64_t> timestamp_{0};
    std::atomic<uint32_t> width_{0};
    std::atomic<uint32_t> height_{0};
    std::atomic<uint32_t> format_{0};  // DXGI_FORMAT
    std::atomic<uint32_t> isHDR_{0};
    std::atomic<int32_t> luidLowPart_{0};
    std::atomic<int32_t> luidHighPart_{0};
    std::atomic<uint32_t> luidSourcePid_{0};
    std::atomic<uint32_t> sourcePid_{0};

public:
    // Atomic accessors for shared texture handles
    uint64_t GetSharedHandle(int index) const {
        if (index < 0 || index >= SHARED_TEXTURE_SLOT_COUNT)
            return 0;
        return sharedHandles_[index].load(std::memory_order_acquire);
    }
    void SetSharedHandle(int index, uint64_t val) {
        if (index < 0 || index >= SHARED_TEXTURE_SLOT_COUNT)
            return;
        sharedHandles_[index].store(val, std::memory_order_release);
    }

    uint64_t GetFenceShareHandle() const {
        return fenceShareHandle_.load(std::memory_order_acquire);
    }
    void SetFenceShareHandle(uint64_t val) {
        fenceShareHandle_.store(val, std::memory_order_release);
    }

    uint64_t GetFenceValue() const {
        return fenceValue_.load(std::memory_order_acquire);
    }
    void SetFenceValue(uint64_t val) {
        fenceValue_.store(val, std::memory_order_release);
    }

    int32_t GetCurrentReadIndex() const {
        return currentReadIndex_.load(std::memory_order_acquire);
    }
    void SetCurrentReadIndex(int32_t val) {
        currentReadIndex_.store(val, std::memory_order_release);
    }

    int64_t GetTimestamp() const {
        return timestamp_.load(std::memory_order_acquire);
    }
    void SetTimestamp(int64_t val) {
        timestamp_.store(val, std::memory_order_release);
    }

    uint32_t GetWidth() const {
        return width_.load(std::memory_order_acquire);
    }
    void SetWidth(uint32_t val) {
        width_.store(val, std::memory_order_release);
    }

    uint32_t GetHeight() const {
        return height_.load(std::memory_order_acquire);
    }
    void SetHeight(uint32_t val) {
        height_.store(val, std::memory_order_release);
    }

    uint32_t GetFormat() const {
        return format_.load(std::memory_order_acquire);
    }
    void SetFormat(uint32_t val) {
        format_.store(val, std::memory_order_release);
    }

    bool GetIsHDR() const {
        return isHDR_.load(std::memory_order_acquire) != 0;
    }
    void SetIsHDR(bool val) {
        isHDR_.store(val ? 1u : 0u, std::memory_order_release);
    }

    int32_t GetLuidLowPart() const {
        return luidLowPart_.load(std::memory_order_acquire);
    }
    void SetLuidLowPart(int32_t val) {
        luidLowPart_.store(val, std::memory_order_release);
    }

    int32_t GetLuidHighPart() const {
        return luidHighPart_.load(std::memory_order_acquire);
    }
    void SetLuidHighPart(int32_t val) {
        luidHighPart_.store(val, std::memory_order_release);
    }

    uint32_t GetLuidSourcePid() const {
        return luidSourcePid_.load(std::memory_order_acquire);
    }
    void SetLuidSourcePid(uint32_t val) {
        luidSourcePid_.store(val, std::memory_order_release);
    }

    uint32_t GetSourcePid() const {
        return sourcePid_.load(std::memory_order_acquire);
    }
    void SetSourcePid(uint32_t val) {
        sourcePid_.store(val, std::memory_order_release);
    }

    CaptureState runtimeState;

    // System Metrics (Host -> Hook)
    // Collected by Host Process to avoid Anti-Cheat interference in Hook
    struct SharedSystemMetrics {
        // Even while stable, odd while the host is publishing a new snapshot.
        std::atomic<uint32_t> publicationSequence{0};
        std::atomic<float> cpuUsage{0.0f};
        std::atomic<float> ramUsage{0.0f};  // GB
        std::atomic<float> gpuUsage{0.0f};
        std::atomic<float> vramUsage{0.0f};    // MB
        std::atomic<uint64_t> vramTotal{0};    // Bytes
        std::atomic<uint32_t> maxCoreLoad{0};  // NEW: Max single core load
        std::atomic<float> cpuTemperatureC{0.0f};
        std::atomic<float> gpuTemperatureC{0.0f};
        std::atomic<float> cpuPackagePowerW{0.0f};
        std::atomic<float> gpuPackagePowerW{0.0f};
        std::atomic<float> gpuFanRpm{0.0f};
        std::atomic<uint32_t> validityMask{0};
        std::atomic<uint32_t> sourcePid{0};
        std::atomic<int32_t> adapterLuidLow{0};
        std::atomic<int32_t> adapterLuidHigh{0};
        std::atomic<uint32_t> adapterSource{0};
    } systemMetrics;

    // Actual screen-change timestamps collected out of process. Presentation
    // timing remains process-local and is used whenever this stream is not
    // selected or not healthy.
    SharedDisplayTiming displayTiming;

    // DLSS State (Hook -> Host)
    struct DLSSState {
        std::atomic<bool> srActive{false};
        std::atomic<bool> rrActive{false};
        std::atomic<char> srPreset{'?'};       // 'A'-'K' or '?'
        std::atomic<char> rrPreset{'?'};       // 'A'-'G' or '?'
        std::atomic<float> renderScale{0.0f};  // e.g. 1.5 for Quality (100/66)
        std::atomic<int32_t> versionMajor{0};
        std::atomic<int32_t> versionMinor{0};
        std::atomic<int32_t> versionPatch{0};
        std::atomic<int32_t> qualityMode{-1};   // -1=Unknown, 0=Perf, 1=Bal, 2=Qual,
                                                // 3=UltraPerf, 4=UltraQual, 5=DLAA
        std::atomic<bool> fgActive{false};      // Redundant with g_FGCompat but useful for IPC/Host visibility
        std::atomic<int32_t> mfgMultiplier{0};  // 0=No DLSS FG, 2/3/4 = effective output multiplier
    } dlssState;

    // Encoder queue monitoring (Host -> Hook)
    // Hook skips frames when throttleCapture is true to let encoder catch up
    std::atomic<bool> throttleCapture{false};    // True = encoder falling behind, skip frames
    std::atomic<uint32_t> encoderQueueDepth{0};  // Current pending frames in encoder queue

    // Encoder-created shared textures for Vulkan interop
    // When capturing Vulkan games, the encoder creates D3D11 textures and exports
    // handles. VulkanCapture imports these using
    // VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT.
    struct EncoderTextures {
    private:
        std::atomic<uint64_t>
            textureHandles_[ENCODER_TEXTURE_SLOT_COUNT]{};  // NT handles from D3D11 CreateSharedHandle
        std::atomic<uint64_t>
            kmtTextureHandles_[ENCODER_TEXTURE_SLOT_COUNT]{};  // KMT handles from IDXGIResource::GetSharedHandle
        std::atomic<uint64_t> fenceHandle_{0};
        std::atomic<uint32_t> width_{0};
        std::atomic<uint32_t> height_{0};
        std::atomic<uint32_t> format_{0};

    public:
        // Atomic accessors for texture handles
        uint64_t GetTextureHandle(int index) const {
            if (index < 0 || index >= ENCODER_TEXTURE_SLOT_COUNT)
                return 0;
            return textureHandles_[index].load(std::memory_order_acquire);
        }
        void SetTextureHandle(int index, uint64_t val) {
            if (index < 0 || index >= ENCODER_TEXTURE_SLOT_COUNT)
                return;
            textureHandles_[index].store(val, std::memory_order_release);
        }

        // KMT handle accessors (global WDDM handles for cross-process Vulkan import)
        uint64_t GetKmtTextureHandle(int index) const {
            if (index < 0 || index >= ENCODER_TEXTURE_SLOT_COUNT)
                return 0;
            return kmtTextureHandles_[index].load(std::memory_order_acquire);
        }
        void SetKmtTextureHandle(int index, uint64_t val) {
            if (index < 0 || index >= ENCODER_TEXTURE_SLOT_COUNT)
                return;
            kmtTextureHandles_[index].store(val, std::memory_order_release);
        }

        uint64_t GetFenceHandle() const {
            return fenceHandle_.load(std::memory_order_acquire);
        }
        void SetFenceHandle(uint64_t val) {
            fenceHandle_.store(val, std::memory_order_release);
        }

        uint32_t GetWidth() const {
            return width_.load(std::memory_order_acquire);
        }
        void SetWidth(uint32_t val) {
            width_.store(val, std::memory_order_release);
        }

        uint32_t GetHeight() const {
            return height_.load(std::memory_order_acquire);
        }
        void SetHeight(uint32_t val) {
            height_.store(val, std::memory_order_release);
        }

        uint32_t GetFormat() const {
            return format_.load(std::memory_order_acquire);
        }
        void SetFormat(uint32_t val) {
            format_.store(val, std::memory_order_release);
        }

        std::atomic<bool> ready{false};     // True when NT handles are valid
        std::atomic<bool> kmtReady{false};  // True when KMT handles are valid
    } encoderTextures;

    // Layer -> Encoder: when true, encoder uses its own textures directly
    // instead of opening shared handles from ring buffer (DXVK zero-copy path)
    std::atomic<bool> useEncoderTextures{false};

    // Frame ring buffer for lossless capture
    FrameRingBuffer frameRing;

    // Logging Ring Buffer (MPSC: Producers=Hook threads, Consumer=LoggerService)
    struct LogBuffer {
        static constexpr uint32_t SLOT_COUNT = 128;
        static constexpr uint32_t SLOT_SIZE = 512;

        char buffer[SLOT_COUNT][SLOT_SIZE]{};
        std::atomic<uint8_t> committed[SLOT_COUNT]{};     // Set to 1 after slot is fully written
        alignas(64) std::atomic<uint32_t> writeIndex{0};  // Shared across threads, only written by Hook
        alignas(64) std::atomic<uint32_t> readIndex{0};   // Only written by discrete Logger process
        std::atomic<uint32_t> overflowCount{0};           // Tracks lost log entries
    } logs;

    // Shmem Fallback Metadata
private:
    std::atomic<uint32_t> shmemMappingCreated_{0};
    std::atomic<uint32_t> shmemMappingSize_{0};  // Size of the separate mapping

public:
    bool GetShmemMappingCreated() const {
        return shmemMappingCreated_.load(std::memory_order_acquire) != 0;
    }
    void SetShmemMappingCreated(bool val) {
        shmemMappingCreated_.store(val ? 1u : 0u, std::memory_order_release);
    }

    uint32_t GetShmemMappingSize() const {
        return shmemMappingSize_.load(std::memory_order_acquire);
    }
    void SetShmemMappingSize(uint32_t val) {
        shmemMappingSize_.store(val, std::memory_order_release);
    }

    // Cache Invalidation
    std::atomic<uint32_t> configVersion{0};  // Incremented when config changes
};

#pragma pack(pop)
