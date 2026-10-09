#include "mediaengine_loader.h"
#include <windows.h>
#include <cstring>
#include <filesystem>
#include "common/logging/logging.h"
#include "common/platform/secure_dll_loading.h"

MediaEngine_SetLogCallback_t MediaEngine_SetLogCallback = nullptr;
DLL_Log_t DLL_Log = nullptr;
MediaEngine_Init_t MediaEngine_Init = nullptr;
MediaEngine_ReloadConfig_t MediaEngine_ReloadConfig = nullptr;
MediaEngine_SetActiveScreenGrab_t MediaEngine_SetActiveScreenGrab = nullptr;
MediaEngine_SetWgcStartupExtraDelayQpc_t MediaEngine_SetWgcStartupExtraDelayQpc = nullptr;
MediaEngine_SetScreenGrabLatencyReductionQpc_t MediaEngine_SetScreenGrabLatencyReductionQpc = nullptr;
MediaEngine_ProcessFrame_t MediaEngine_ProcessFrame = nullptr;
MediaEngine_RepeatLastFrame_t MediaEngine_RepeatLastFrame = nullptr;
MediaEngine_RepeatLastFrameWithTimeline_t MediaEngine_RepeatLastFrameWithTimeline = nullptr;
MediaEngine_CanRepeatLastFrame_t MediaEngine_CanRepeatLastFrame = nullptr;
MediaEngine_ResetRepeatFrameCache_t MediaEngine_ResetRepeatFrameCache = nullptr;
MediaEngine_PrepareFrameD3D11_t MediaEngine_PrepareFrameD3D11 = nullptr;
MediaEngineProcessFrameD3D11Caller MediaEngine_ProcessFrameD3D11;
MediaEngine_StartRecording_t MediaEngine_StartRecording = nullptr;
MediaEngine_StopRecording_t MediaEngine_StopRecording = nullptr;
MediaEngine_GetLastOutputDegradedFlags_t MediaEngine_GetLastOutputDegradedFlags = nullptr;
MediaEngine_ReleaseEncoderTextures_t MediaEngine_ReleaseEncoderTextures = nullptr;
MediaEngine_GetD3D11Device_t MediaEngine_GetD3D11Device = nullptr;
MediaEngine_ReleaseSharedD3D11Device_t MediaEngine_ReleaseSharedD3D11Device = nullptr;
MediaEngine_CreateSharedCaptureTextures_t MediaEngine_CreateSharedCaptureTextures = nullptr;
MediaEngine_GetLastFrameEncodeTimeUs_t MediaEngine_GetLastFrameEncodeTimeUs = nullptr;
MediaEngine_GetLastFrameFenceWaitUs_t MediaEngine_GetLastFrameFenceWaitUs = nullptr;
MediaEngine_WasLastFrameDeferred_t MediaEngine_WasLastFrameDeferred = nullptr;
MediaEngine_GetMuxFlowSnapshotV1_t MediaEngine_GetMuxFlowSnapshotV1 = nullptr;
MediaEngine_QueryInjectFrameCopyCompletion_t MediaEngine_QueryInjectFrameCopyCompletion = nullptr;
MediaEngine_SetInjectTransportGeneration_t MediaEngine_SetInjectTransportGeneration = nullptr;
MediaEngine_Shutdown_t MediaEngine_Shutdown = nullptr;
MediaEngine_SetSharedMem_t MediaEngine_SetSharedMem = nullptr;
MediaEngine_LockD3D11_t MediaEngine_LockD3D11 = nullptr;
MediaEngine_UnlockD3D11_t MediaEngine_UnlockD3D11 = nullptr;
MediaEngine_SetAudioOnly_t MediaEngine_SetAudioOnly = nullptr;
MediaEngine_SetSourcePrefers10Bit_t MediaEngine_SetSourcePrefers10Bit = nullptr;
MediaEngine_SetCursorCompositionSuppressed_t MediaEngine_SetCursorCompositionSuppressed = nullptr;
MediaEngine_MeasureRenderEndpointLatency_t MediaEngine_MeasureRenderEndpointLatency = nullptr;
MediaEngine_SetRenderLatencyChannel_t MediaEngine_SetRenderLatencyChannel = nullptr;

MediaEngine_SubmitFrameWithResultV1_t MediaEngine_SubmitFrameWithResultV1 = nullptr;
MediaEngine_SubmitFrameD3D11WithResultV1_t MediaEngine_SubmitFrameD3D11WithResultV1 = nullptr;
MediaEngine_RepeatLastFrameWithResultV1_t MediaEngine_RepeatLastFrameWithResultV1 = nullptr;

static HMODULE g_MediaEngineModule = nullptr;

template <typename T, typename Resolver>
static bool GetFunc(HMODULE hModule, const char* name, T* outPtr, Resolver resolve) {
    *outPtr = reinterpret_cast<T>(resolve(hModule, name));
    if (!*outPtr) {
        LogError("[MediaEngine] Failed to get function: %s", name);
        return false;
    }
    return true;
}

template <typename Resolver>
static bool ResolveMediaEngineExports(HMODULE module, Resolver resolve) {
    bool success = true;
    success &= GetFunc(module, "MediaEngine_SubmitFrameWithResultV1", &MediaEngine_SubmitFrameWithResultV1, resolve);
    success &= GetFunc(module, "MediaEngine_SubmitFrameD3D11WithResultV1", &MediaEngine_SubmitFrameD3D11WithResultV1, resolve);
    success &= GetFunc(module, "MediaEngine_RepeatLastFrameWithResultV1", &MediaEngine_RepeatLastFrameWithResultV1, resolve);
    success &= GetFunc(module, "MediaEngine_SetLogCallback", &MediaEngine_SetLogCallback, resolve);
    success &= GetFunc(module, "DLL_Log", &DLL_Log, resolve);
    success &= GetFunc(module, "MediaEngine_Init", &MediaEngine_Init, resolve);
    success &= GetFunc(module, "MediaEngine_ReloadConfig", &MediaEngine_ReloadConfig, resolve);
    success &= GetFunc(module, "MediaEngine_SetActiveScreenGrab", &MediaEngine_SetActiveScreenGrab, resolve);
    success &=
        GetFunc(module, "MediaEngine_SetWgcStartupExtraDelayQpc", &MediaEngine_SetWgcStartupExtraDelayQpc, resolve);
    success &= GetFunc(module, "MediaEngine_SetScreenGrabLatencyReductionQpc",
                       &MediaEngine_SetScreenGrabLatencyReductionQpc, resolve);
    success &= GetFunc(module, "MediaEngine_SubmitFrame", &MediaEngine_ProcessFrame, resolve);
    success &= GetFunc(module, "MediaEngine_RepeatLastFrame", &MediaEngine_RepeatLastFrame, resolve);
    success &= GetFunc(module, "MediaEngine_RepeatLastFrameWithTimeline",
                       &MediaEngine_RepeatLastFrameWithTimeline, resolve);
    success &= GetFunc(module, "MediaEngine_CanRepeatLastFrame", &MediaEngine_CanRepeatLastFrame, resolve);
    success &= GetFunc(module, "MediaEngine_ResetRepeatFrameCache", &MediaEngine_ResetRepeatFrameCache, resolve);
    success &= GetFunc(module, "MediaEngine_PrepareFrameD3D11", &MediaEngine_PrepareFrameD3D11, resolve);
    success &= GetFunc(module, "MediaEngine_SubmitFrameD3D11", &MediaEngine_ProcessFrameD3D11.raw, resolve);
    success &= GetFunc(module, "MediaEngine_StartRecording", &MediaEngine_StartRecording, resolve);
    success &= GetFunc(module, "MediaEngine_StopRecording", &MediaEngine_StopRecording, resolve);
    success &= GetFunc(module, "MediaEngine_GetLastOutputDegradedFlags",
                       &MediaEngine_GetLastOutputDegradedFlags, resolve);
    success &= GetFunc(module, "MediaEngine_ReleaseEncoderTextures", &MediaEngine_ReleaseEncoderTextures, resolve);
    success &= GetFunc(module, "MediaEngine_GetD3D11Device", &MediaEngine_GetD3D11Device, resolve);
    success &=
        GetFunc(module, "MediaEngine_ReleaseSharedD3D11Device", &MediaEngine_ReleaseSharedD3D11Device, resolve);
    success &= GetFunc(module, "MediaEngine_CreateSharedCaptureTextures",
                       &MediaEngine_CreateSharedCaptureTextures, resolve);
    success &=
        GetFunc(module, "MediaEngine_GetLastFrameEncodeTimeUs", &MediaEngine_GetLastFrameEncodeTimeUs, resolve);
    success &=
        GetFunc(module, "MediaEngine_GetLastFrameFenceWaitUs", &MediaEngine_GetLastFrameFenceWaitUs, resolve);
    success &= GetFunc(module, "MediaEngine_WasLastFrameDeferred", &MediaEngine_WasLastFrameDeferred, resolve);
    success &= GetFunc(module, "MediaEngine_GetMuxFlowSnapshotV1", &MediaEngine_GetMuxFlowSnapshotV1, resolve);
    success &= GetFunc(module, "MediaEngine_QueryInjectFrameCopyCompletion",
                       &MediaEngine_QueryInjectFrameCopyCompletion, resolve);
    success &= GetFunc(module, "MediaEngine_SetInjectTransportGeneration",
                       &MediaEngine_SetInjectTransportGeneration, resolve);
    success &= GetFunc(module, "MediaEngine_Shutdown", &MediaEngine_Shutdown, resolve);
    success &= GetFunc(module, "MediaEngine_SetSharedMem", &MediaEngine_SetSharedMem, resolve);
    success &= GetFunc(module, "MediaEngine_LockD3D11", &MediaEngine_LockD3D11, resolve);
    success &= GetFunc(module, "MediaEngine_UnlockD3D11", &MediaEngine_UnlockD3D11, resolve);
    success &= GetFunc(module, "MediaEngine_SetSourcePrefers10Bit", &MediaEngine_SetSourcePrefers10Bit, resolve);
    success &= GetFunc(module, "MediaEngine_SetCursorCompositionSuppressed",
                       &MediaEngine_SetCursorCompositionSuppressed, resolve);
    success &= GetFunc(module, "MediaEngine_SetAudioOnly", &MediaEngine_SetAudioOnly, resolve);
    success &= GetFunc(module, "MediaEngine_MeasureRenderEndpointLatency",
                       &MediaEngine_MeasureRenderEndpointLatency, resolve);
    success &= GetFunc(module, "MediaEngine_SetRenderLatencyChannel",
                       &MediaEngine_SetRenderLatencyChannel, resolve);

    if (!success) {
        LogError("[MediaEngine] Incompatible mediaengine.dll: required exports are missing");
        MediaEngine_Unload();  // clear every pointer into the rejected module, not just its handle
        return false;
    }

    LogInfo("[MediaEngine] All function pointers resolved");
    return true;
}

bool MediaEngine_Load(const char* exeDir) {
    if (g_MediaEngineModule) {
        return true;
    }

    const std::filesystem::path executableDirectory = std::filesystem::absolute(exeDir).lexically_normal();
    const std::filesystem::path ffmpegDirectory = executableDirectory / L"ffmpeg";
    DWORD loadError = ERROR_SUCCESS;
    if (!ce::security::EnsureSecureDllSearchDirectory(ffmpegDirectory, &loadError)) {
        LogError("[MediaEngine] Failed to register secure FFmpeg directory: error %lu", loadError);
        return false;
    }
    LogInfo("[MediaEngine] Registered restricted FFmpeg dependency directory");

    const std::filesystem::path dllPath = executableDirectory / L"mediaengine.dll";
    LogInfo("[MediaEngine] Loading mediaengine.dll from the executable directory");
    g_MediaEngineModule = ce::security::LoadLibraryFromSecurePath(dllPath, &loadError);
    if (!g_MediaEngineModule) {
        LogError("[MediaEngine] Failed to load mediaengine.dll: error %lu", loadError);
        return false;
    }
    LogInfo("[MediaEngine] Loaded successfully");

    return ResolveMediaEngineExports(g_MediaEngineModule, &GetProcAddress);
}

void MediaEngine_Unload() {
    if (g_MediaEngineModule) {
        FreeLibrary(g_MediaEngineModule);
        g_MediaEngineModule = nullptr;
    }

    MediaEngine_SubmitFrameWithResultV1 = nullptr;
    MediaEngine_SubmitFrameD3D11WithResultV1 = nullptr;
    MediaEngine_RepeatLastFrameWithResultV1 = nullptr;
    MediaEngine_SetLogCallback = nullptr;
    DLL_Log = nullptr;
    MediaEngine_Init = nullptr;
    MediaEngine_ReloadConfig = nullptr;
    MediaEngine_SetActiveScreenGrab = nullptr;
    MediaEngine_SetWgcStartupExtraDelayQpc = nullptr;
    MediaEngine_SetScreenGrabLatencyReductionQpc = nullptr;
    MediaEngine_ProcessFrame = nullptr;
    MediaEngine_RepeatLastFrame = nullptr;
    MediaEngine_RepeatLastFrameWithTimeline = nullptr;
    MediaEngine_CanRepeatLastFrame = nullptr;
    MediaEngine_ResetRepeatFrameCache = nullptr;
    MediaEngine_PrepareFrameD3D11 = nullptr;
    MediaEngine_ProcessFrameD3D11 = nullptr;
    MediaEngine_StartRecording = nullptr;
    MediaEngine_StopRecording = nullptr;
    MediaEngine_GetLastOutputDegradedFlags = nullptr;
    MediaEngine_ReleaseEncoderTextures = nullptr;
    MediaEngine_GetD3D11Device = nullptr;
    MediaEngine_ReleaseSharedD3D11Device = nullptr;
    MediaEngine_CreateSharedCaptureTextures = nullptr;
    MediaEngine_GetLastFrameEncodeTimeUs = nullptr;
    MediaEngine_GetLastFrameFenceWaitUs = nullptr;
    MediaEngine_WasLastFrameDeferred = nullptr;
    MediaEngine_GetMuxFlowSnapshotV1 = nullptr;
    MediaEngine_QueryInjectFrameCopyCompletion = nullptr;
    MediaEngine_SetInjectTransportGeneration = nullptr;
    MediaEngine_Shutdown = nullptr;
    MediaEngine_SetSharedMem = nullptr;
    MediaEngine_LockD3D11 = nullptr;
    MediaEngine_UnlockD3D11 = nullptr;
    MediaEngine_SetAudioOnly = nullptr;
    MediaEngine_SetSourcePrefers10Bit = nullptr;
    MediaEngine_SetCursorCompositionSuppressed = nullptr;
    MediaEngine_MeasureRenderEndpointLatency = nullptr;
    MediaEngine_SetRenderLatencyChannel = nullptr;
}
