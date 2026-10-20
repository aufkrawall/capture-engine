#pragma once

// The state of one media pipeline that the C exports and the pipeline's threads share (DR-42, M16a).
//
// Every per-pipeline global of mediaengine.dll lives in one MediaEngineInstance. The exports reach it
// through DefaultMediaEngineInstance(), the single process-wide lookup left in the DLL: M16b replaces that
// lookup with a handle table and gives each handle its own instance. Nothing below the exports reads a
// process global for per-pipeline state; objects receive the instance (or the part they use) explicitly.

#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include "common/ipc/av_sync_latency_channel.h"
#include "mediaengine/video/encoder_auto_probe.h"
#include "mediaengine/video/gpu_memory_breakdown.h"
#include "mediaengine/video/retained_yuv_converter.h"
#include "mediaengine/video/source_read_markers.h"

class MediaEngine;

class MediaEngineInstance {
public:
    MediaEngineInstance();
    // Members are released in reverse declaration order: the engine first (its encoder still
    // references the device lock and the source-read sequencer), then the retained converter. The
    // shared device is not released here; Shutdown and ReleaseSharedDevice are the release points.
    ~MediaEngineInstance();
    MediaEngineInstance(const MediaEngineInstance&) = delete;
    MediaEngineInstance& operator=(const MediaEngineInstance&) = delete;

    // Taken by every export and entry point that touches the engine. Held across a whole encode.
    std::recursive_mutex apiMutex;

    // Serializes the immediate context and the encoder device. Lock order: this, then retainedConverterMutex.
    std::recursive_mutex d3d11Mutex;

    // Shared D3D11 device and immediate context of the screen-grab and framegrab paths. They exist before
    // Init (MediaEngine_GetD3D11Device), so they belong to the instance, not to the engine.
    ID3D11Device* sharedDevice = nullptr;
    ID3D11DeviceContext* sharedContext = nullptr;

    // Ingress converter of captured frames into the retained reservoir (mediaengine_retained_frame.cpp).
    // It holds a device reference, which is why it is released before the shared device.
    std::mutex retainedConverterMutex;
    uint64_t retainedConversionFailures = 0;
    RetainedYuvConverter retainedConverter;

    // Last capture pool report (MediaEngine_ReportCaptureGpuMemory), read by each encoder's GPU memory log.
    ce::gpu_memory::CaptureGpuGauges captureGauges;

    // Completion of the encoder's GPU reads of inject source slots (source_read_markers.h).
    ce::media::SourceReadSequencer injectSourceReads;

    // Controller-owned session latency channel, attached before the probe runs (MediaEngine_SetRenderLatencyChannel).
    std::atomic<ce::av_sync::LatencyChannelBlock*> renderLatencyChannel{nullptr};

    // encoder=auto resolutions remembered for this pipeline's provisional names.
    ce::media::AutoEncoderLastResolutions autoEncoderLastResolutions;

    // The pipeline. Created by Init, reset by Shutdown. Declared last so it is destroyed first.
    std::unique_ptr<MediaEngine> engine;

    // SetAudioOnly before Init latches here; Init applies it to the engine it creates.
    bool pendingAudioOnly = false;

    // Returns the shared device, creating it on first use (adapter choice follows the engine's config
    // when one exists). Null when creation fails.
    ID3D11Device* GetSharedDevice();

    // Releases the retained converter (under the D3D11 lock), then the shared device and context.
    void ReleaseSharedDevice();

    // Releases the retained converter and its failure count; the next conversion re-creates its kernels.
    void ReleaseRetainedFrameConverter();
};

// The instance the C exports operate on. M16b replaces this accessor with a handle lookup.
MediaEngineInstance& DefaultMediaEngineInstance();
