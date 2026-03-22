#include "mediaengine_loader.h"
#include <windows.h>
#include <cstring>
#include "../common/logging.h"

MediaEngine_SetLogCallback_t MediaEngine_SetLogCallback = nullptr;
DLL_Log_t DLL_Log = nullptr;
MediaEngine_Init_t MediaEngine_Init = nullptr;
MediaEngine_ReloadConfig_t MediaEngine_ReloadConfig = nullptr;
MediaEngine_ProcessFrame_t MediaEngine_ProcessFrame = nullptr;
MediaEngine_RepeatLastFrame_t MediaEngine_RepeatLastFrame = nullptr;
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
MediaEngine_SetSourcePrefers10Bit_t MediaEngine_SetSourcePrefers10Bit = nullptr;

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

    char ffmpegDir[MAX_PATH];
    snprintf(ffmpegDir, sizeof(ffmpegDir), "%s\\ffmpeg", exeDir);

    if (GetFileAttributesA(ffmpegDir) == INVALID_FILE_ATTRIBUTES) {
        LogError("[MediaEngine] FFmpeg folder not found: %s", ffmpegDir);
        return false;
    }

    if (!SetDllDirectoryA(ffmpegDir)) {
        LogError("[MediaEngine] Failed to set DLL directory: %s", ffmpegDir);
        return false;
    }
    LogInfo("[MediaEngine] Set DLL search path to: %s", ffmpegDir);

    char dllPath[MAX_PATH];
    snprintf(dllPath, sizeof(dllPath), "%s\\mediaengine.dll", exeDir);

    LogInfo("[MediaEngine] Loading: %s", dllPath);
    g_MediaEngineModule = LoadLibraryA(dllPath);
    if (!g_MediaEngineModule) {
        DWORD err = GetLastError();
        LogError("[MediaEngine] Failed to load mediaengine.dll: error %lu", err);
        SetDllDirectoryA(nullptr);
        return false;
    }
    LogInfo("[MediaEngine] Loaded successfully");

    bool success = true;
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_SetLogCallback", &MediaEngine_SetLogCallback);
    success &= GetFunc(g_MediaEngineModule, "DLL_Log", &DLL_Log);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_Init", &MediaEngine_Init);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_ReloadConfig", &MediaEngine_ReloadConfig);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_ProcessFrame", &MediaEngine_ProcessFrame);
    success &= GetFunc(g_MediaEngineModule, "MediaEngine_RepeatLastFrame", &MediaEngine_RepeatLastFrame);
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

    if (!success) {
        LogError("[MediaEngine] Failed to get all function pointers");
        FreeLibrary(g_MediaEngineModule);
        g_MediaEngineModule = nullptr;
        SetDllDirectoryA(nullptr);
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
    MediaEngine_ProcessFrame = nullptr;
    MediaEngine_RepeatLastFrame = nullptr;
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
    MediaEngine_SetSourcePrefers10Bit = nullptr;

    SetDllDirectoryA(nullptr);
}
