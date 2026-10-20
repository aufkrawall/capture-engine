#include "media_main_internal.h"

void MediaProcessSession::Shutdown() {
    if (auto capture = MediaState().wgcCap.LockExclusive()) {
        capture->StopCapture();
    }
    if (d3dContext)
        d3dContext->Release();
    d3dDevice = nullptr;

    // Shutdown media engine BEFORE unmapping shared memory to avoid use-after-free
    // (VideoEncoder::CleanupResources accesses pSharedMem during destruction)
    if (mediaEngineReady && MediaEngine_Shutdown) {
        MediaEngine_Shutdown();
        mediaEngineReady = false;
    }
    MediaEngine_Unload();

    // Only safe once the engine is unloaded: the probe inside mediaengine.dll holds this view.
    if (latencyChannel) {
        ce::av_sync::UnmapInheritedLatencyChannel(latencyChannel);
        latencyChannel = nullptr;
    }

    // The finalization result was published when the recording stopped; the spawner holds the section,
    // so dropping this process's view loses nothing.
    ce::media_result::UnmapInheritedSlot(MediaState().resultSlot);
    MediaState().resultSlot = nullptr;

    // Release every remaining metadata/resource lease while the cross-process
    // mapping is still valid. Normal recording stop already does this, but the
    // process-exit path also covers partial startup failures and shutdowns that
    // occur before a recording becomes active.
    MediaState().frameQueue.Clear();
    ClearStandbyWgcHandoffFrame();
    ResetLastQueuedFrameCache();

    if (MediaState().injectFrameReadyEvent) {
        CloseHandle(MediaState().injectFrameReadyEvent);
        MediaState().injectFrameReadyEvent = NULL;
    }
    if (MediaState().injectCaptureShutdownEvent) {
        CloseHandle(MediaState().injectCaptureShutdownEvent);
        MediaState().injectCaptureShutdownEvent = NULL;
    }

    if (MediaState().pShmem)
        UnmapViewOfFile(MediaState().pShmem);
    if (MediaState().hMapShmem)
        CloseHandle(MediaState().hMapShmem);

    if (MediaState().pSharedMem)
        UnmapViewOfFile(MediaState().pSharedMem);
    if (MediaState().hMapFile)
        CloseHandle(MediaState().hMapFile);

    LogInfo("[Media] Process exiting");
}

