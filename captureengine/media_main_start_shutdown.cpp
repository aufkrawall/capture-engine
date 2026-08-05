#include "media_main_internal.h"

void MediaProcessSession::Shutdown() {
    if (auto capture = media_main_g_WgcCap.LockExclusive()) {
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

    // Release every remaining metadata/resource lease while the cross-process
    // mapping is still valid. Normal recording stop already does this, but the
    // process-exit path also covers partial startup failures and shutdowns that
    // occur before a recording becomes active.
    media_main_g_FrameQueue.Clear();
    ClearStandbyWgcHandoffFrame();
    ResetLastQueuedFrameCache();

    if (media_main_g_InjectFrameReadyEvent) {
        CloseHandle(media_main_g_InjectFrameReadyEvent);
        media_main_g_InjectFrameReadyEvent = NULL;
    }
    if (media_main_g_InjectCaptureShutdownEvent) {
        CloseHandle(media_main_g_InjectCaptureShutdownEvent);
        media_main_g_InjectCaptureShutdownEvent = NULL;
    }

    if (media_main_g_pShmem)
        UnmapViewOfFile(media_main_g_pShmem);
    if (media_main_g_hMapShmem)
        CloseHandle(media_main_g_hMapShmem);

    if (media_main_g_pSharedMem)
        UnmapViewOfFile(media_main_g_pSharedMem);
    if (media_main_g_hMapFile)
        CloseHandle(media_main_g_hMapFile);

    LogInfo("[Media] Process exiting");
}

