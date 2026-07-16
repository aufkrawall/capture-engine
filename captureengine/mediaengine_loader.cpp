#include "mediaengine_loader.h"
#include <windows.h>
#include <cstring>
#include <filesystem>
#include "../common/logging.h"
#include "../common/secure_dll_loading.h"

MediaEngine_SetLogCallback_t MediaEngine_SetLogCallback = nullptr;
DLL_Log_t DLL_Log = nullptr;
MediaEngine_Init_t MediaEngine_Init = nullptr;
MediaEngine_ReloadConfig_t MediaEngine_ReloadConfig = nullptr;
MediaEngine_SetActiveScreenGrab_t MediaEngine_SetActiveScreenGrab = nullptr;
MediaEngine_SetWgcStartupExtraDelayQpc_t MediaEngine_SetWgcStartupExtraDelayQpc = nullptr;
MediaEngine_ProcessFrame_t MediaEngine_ProcessFrame = nullptr;
MediaEngine_RepeatLastFrame_t MediaEngine_RepeatLastFrame = nullptr;
MediaEngine_RepeatLastFrameWithTimeline_t MediaEngine_RepeatLastFrameWithTimeline = nullptr;
MediaEngine_CanRepeatLastFrame_t MediaEngine_CanRepeatLastFrame = nullptr;
MediaEngine_ResetRepeatFrameCache_t MediaEngine_ResetRepeatFrameCache = nullptr;
MediaEngine_PrepareFrameD3D11_t MediaEngine_PrepareFrameD3D11 = nullptr;
MediaEngine_ProcessFrameD3D11_t MediaEngine_ProcessFrameD3D11 = nullptr;
MediaEngine_StartRecording_t MediaEngine_StartRecording = nullptr;
MediaEngine_StopRecording_t MediaEngine_StopRecording = nullptr;
MediaEngine_ReleaseEncoderTextures_t MediaEngine_ReleaseEncoderTextures = nullptr;
MediaEngine_GetD3D11Device_t MediaEngine_GetD3D11Device = nullptr;
MediaEngine_ReleaseSharedD3D11Device_t MediaEngine_ReleaseSharedD3D11Device = nullptr;
MediaEngine_CreateSharedCaptureTextures_t MediaEngine_CreateSharedCaptureTextures = nullptr;
MediaEngine_GetLastFrameEncodeTimeUs_t MediaEngine_GetLastFrameEncodeTimeUs = nullptr;
MediaEngine_GetLastFrameFenceWaitUs_t MediaEngine_GetLastFrameFenceWaitUs = nullptr;
MediaEngine_WasLastFrameDeferred_t MediaEngine_WasLastFrameDeferred = nullptr;
MediaEngine_Shutdown_t MediaEngine_Shutdown = nullptr;
MediaEngine_SetSharedMem_t MediaEngine_SetSharedMem = nullptr;
MediaEngine_LockD3D11_t MediaEngine_LockD3D11 = nullptr;
MediaEngine_UnlockD3D11_t MediaEngine_UnlockD3D11 = nullptr;
MediaEngine_SetAudioOnly_t MediaEngine_SetAudioOnly = nullptr;
MediaEngine_SetSourcePrefers10Bit_t MediaEngine_SetSourcePrefers10Bit = nullptr;
MediaEngine_SetCursorCompositionSuppressed_t MediaEngine_SetCursorCompositionSuppressed = nullptr;
MediaEngine_MeasureRenderEndpointLatency_t MediaEngine_MeasureRenderEndpointLatency = nullptr;

static HMODULE g_MediaEngineModule = nullptr;

template <typename T>
static bool GetFunc(HMODULE hModule, const char* name, T* outPtr) {
    *outPtr = (T)GetProcAddress(hModule, name);
    if (!*outPtr) {
        LogError("[MediaEngine] Failed to get function: %s", name);
        return false;
    }
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

    bool success = true;
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_SetLogCallback", &MediaEngine_SetLogCallback);
    success &= GetFunc(g_MediaEngineModule, "DLL_Log", &DLL_Log);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_Init", &MediaEngine_Init);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_ReloadConfig", &MediaEngine_ReloadConfig);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_SetActiveScreenGrab", &MediaEngine_SetActiveScreenGrab);
    success &=
        GetFunc(g_MediaEngineModule, "MediaEngine_SetWgcStartupExtraDelayQpc", &MediaEngine_SetWgcStartupExtraDelayQpc);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_ProcessFrame", &MediaEngine_ProcessFrame);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_RepeatLastFrame", &MediaEngine_RepeatLastFrame);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_RepeatLastFrameWithTimeline",
                       &MediaEngine_RepeatLastFrameWithTimeline);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_CanRepeatLastFrame", &MediaEngine_CanRepeatLastFrame);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_ResetRepeatFrameCache", &MediaEngine_ResetRepeatFrameCache);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_PrepareFrameD3D11", &MediaEngine_PrepareFrameD3D11);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_ProcessFrameD3D11", &MediaEngine_ProcessFrameD3D11);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_StartRecording", &MediaEngine_StartRecording);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_StopRecording", &MediaEngine_StopRecording);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_ReleaseEncoderTextures", &MediaEngine_ReleaseEncoderTextures);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_GetD3D11Device", &MediaEngine_GetD3D11Device);
    success &=
        GetFunc(g_MediaEngineModule, "MediaEngine_ReleaseSharedD3D11Device", &MediaEngine_ReleaseSharedD3D11Device);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_CreateSharedCaptureTextures",
                       &MediaEngine_CreateSharedCaptureTextures);
    success &=
        GetFunc(g_MediaEngineModule, "MediaEngine_GetLastFrameEncodeTimeUs", &MediaEngine_GetLastFrameEncodeTimeUs);
    success &=
        GetFunc(g_MediaEngineModule, "MediaEngine_GetLastFrameFenceWaitUs", &MediaEngine_GetLastFrameFenceWaitUs);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_WasLastFrameDeferred", &MediaEngine_WasLastFrameDeferred);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_Shutdown", &MediaEngine_Shutdown);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_SetSharedMem", &MediaEngine_SetSharedMem);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_LockD3D11", &MediaEngine_LockD3D11);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_UnlockD3D11", &MediaEngine_UnlockD3D11);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_SetSourcePrefers10Bit", &MediaEngine_SetSourcePrefers10Bit);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_SetCursorCompositionSuppressed",
                       &MediaEngine_SetCursorCompositionSuppressed);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_SetAudioOnly", &MediaEngine_SetAudioOnly);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_MeasureRenderEndpointLatency",
                       &MediaEngine_MeasureRenderEndpointLatency);

    if (!success) {
        LogError("[MediaEngine] Failed to get all function pointers");
        FreeLibrary(g_MediaEngineModule);
        g_MediaEngineModule = nullptr;
        return false;
    }

    LogInfo("[MediaEngine] All function pointers resolved");
    return true;
}

void MediaEngine_Unload() {
    if (g_MediaEngineModule) {
        FreeLibrary(g_MediaEngineModule);
        g_MediaEngineModule = nullptr;
    }

    MediaEngine_SetLogCallback = nullptr;
    DLL_Log = nullptr;
    MediaEngine_Init = nullptr;
    MediaEngine_ReloadConfig = nullptr;
    MediaEngine_SetActiveScreenGrab = nullptr;
    MediaEngine_SetWgcStartupExtraDelayQpc = nullptr;
    MediaEngine_ProcessFrame = nullptr;
    MediaEngine_RepeatLastFrame = nullptr;
    MediaEngine_RepeatLastFrameWithTimeline = nullptr;
    MediaEngine_CanRepeatLastFrame = nullptr;
    MediaEngine_ResetRepeatFrameCache = nullptr;
    MediaEngine_PrepareFrameD3D11 = nullptr;
    MediaEngine_ProcessFrameD3D11 = nullptr;
    MediaEngine_StartRecording = nullptr;
    MediaEngine_StopRecording = nullptr;
    MediaEngine_ReleaseEncoderTextures = nullptr;
    MediaEngine_GetD3D11Device = nullptr;
    MediaEngine_ReleaseSharedD3D11Device = nullptr;
    MediaEngine_CreateSharedCaptureTextures = nullptr;
    MediaEngine_GetLastFrameEncodeTimeUs = nullptr;
    MediaEngine_GetLastFrameFenceWaitUs = nullptr;
    MediaEngine_WasLastFrameDeferred = nullptr;
    MediaEngine_Shutdown = nullptr;
    MediaEngine_SetSharedMem = nullptr;
    MediaEngine_LockD3D11 = nullptr;
    MediaEngine_UnlockD3D11 = nullptr;
    MediaEngine_SetAudioOnly = nullptr;
    MediaEngine_SetSourcePrefers10Bit = nullptr;
    MediaEngine_SetCursorCompositionSuppressed = nullptr;
    MediaEngine_MeasureRenderEndpointLatency = nullptr;
}
