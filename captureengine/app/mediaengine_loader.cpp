#include "mediaengine_loader.h"
#include <windows.h>
#include <cstring>
#include <filesystem>
#include "common/logging/logging.h"
#include "common/platform/build_identity_check.h"
#include "common/platform/secure_dll_loading.h"

MediaEngine_SetLogCallback_t MediaEngine_SetLogCallback = nullptr;
MediaEngine_Create_t MediaEngine_Create = nullptr;
MediaEngine_Destroy_t MediaEngine_Destroy = nullptr;
MediaEngine_Init_t MediaEngine_Init = nullptr;
MediaEngine_ReloadConfig_t MediaEngine_ReloadConfig = nullptr;
MediaEngine_SetActiveScreenGrab_t MediaEngine_SetActiveScreenGrab = nullptr;
MediaEngine_SetWgcStartupExtraDelayQpc_t MediaEngine_SetWgcStartupExtraDelayQpc = nullptr;
MediaEngine_SetScreenGrabLatencyReductionQpc_t MediaEngine_SetScreenGrabLatencyReductionQpc = nullptr;
MediaEngine_CanRepeatLastFrame_t MediaEngine_CanRepeatLastFrame = nullptr;
MediaEngine_ResetRepeatFrameCache_t MediaEngine_ResetRepeatFrameCache = nullptr;
MediaEngine_PrepareFrameD3D11_t MediaEngine_PrepareFrameD3D11 = nullptr;
MediaEngine_PrepareInjectVideo_t MediaEngine_PrepareInjectVideo = nullptr;
MediaEngine_StartRecording_t MediaEngine_StartRecording = nullptr;
MediaEngine_StopRecording_t MediaEngine_StopRecording = nullptr;
MediaEngine_GetLastOutputDegradedFlags_t MediaEngine_GetLastOutputDegradedFlags = nullptr;
MediaEngine_GetLastOutputPath_t MediaEngine_GetLastOutputPath = nullptr;
MediaEngine_ReleaseEncoderTextures_t MediaEngine_ReleaseEncoderTextures = nullptr;
MediaEngine_GetD3D11Device_t MediaEngine_GetD3D11Device = nullptr;
MediaEngine_ReleaseSharedD3D11Device_t MediaEngine_ReleaseSharedD3D11Device = nullptr;
MediaEngine_CreateSharedCaptureTextures_t MediaEngine_CreateSharedCaptureTextures = nullptr;
MediaEngine_GetLastFrameEncodeTimeUs_t MediaEngine_GetLastFrameEncodeTimeUs = nullptr;
MediaEngine_GetLastFrameFenceWaitUs_t MediaEngine_GetLastFrameFenceWaitUs = nullptr;
MediaEngine_GetMuxFlowSnapshotV1_t MediaEngine_GetMuxFlowSnapshotV1 = nullptr;
MediaEngine_QueryInjectFrameCopyCompletion_t MediaEngine_QueryInjectFrameCopyCompletion = nullptr;
MediaEngine_SetInjectTransportGeneration_t MediaEngine_SetInjectTransportGeneration = nullptr;
MediaEngine_TakeInjectSourceReadToken_t MediaEngine_TakeInjectSourceReadToken = nullptr;
MediaEngine_IsInjectSourceReadComplete_t MediaEngine_IsInjectSourceReadComplete = nullptr;
MediaEngine_SetInjectSourceReadWakeEvent_t MediaEngine_SetInjectSourceReadWakeEvent = nullptr;
MediaEngine_FinishInjectSourceReads_t MediaEngine_FinishInjectSourceReads = nullptr;
MediaEngine_SetSharedMem_t MediaEngine_SetSharedMem = nullptr;
MediaEngine_LockD3D11_t MediaEngine_LockD3D11 = nullptr;
MediaEngine_UnlockD3D11_t MediaEngine_UnlockD3D11 = nullptr;
MediaEngine_SetAudioOnly_t MediaEngine_SetAudioOnly = nullptr;
MediaEngine_SetSourcePrefers10Bit_t MediaEngine_SetSourcePrefers10Bit = nullptr;
MediaEngine_SetCursorCompositionSuppressed_t MediaEngine_SetCursorCompositionSuppressed = nullptr;
MediaEngine_QueryRetainedFrameRoute_t MediaEngine_QueryRetainedFrameRoute = nullptr;
MediaEngine_ConvertToRetainedFrame_t MediaEngine_ConvertToRetainedFrame = nullptr;
MediaEngine_ReleaseRetainedFrameConverter_t MediaEngine_ReleaseRetainedFrameConverter = nullptr;
MediaEngine_ReportCaptureGpuMemory_t MediaEngine_ReportCaptureGpuMemory = nullptr;
MediaEngine_MeasureRenderEndpointLatency_t MediaEngine_MeasureRenderEndpointLatency = nullptr;

MediaEngine_SubmitFrameWithResultV1_t MediaEngine_SubmitFrameWithResultV1 = nullptr;
MediaEngine_SubmitFrameD3D11WithResultV1_t MediaEngine_SubmitFrameD3D11WithResultV1 = nullptr;
MediaEngine_RepeatLastFrameWithResultV1_t MediaEngine_RepeatLastFrameWithResultV1 = nullptr;

// One module serves every media pipeline of the process (DR-34): the first MediaEngine_Load loads it and each
// successful Load takes a reference; the Unload that drops the last reference frees it.
static HMODULE g_MediaEngineModule = nullptr;
static int g_MediaEngineReferences = 0;

template <typename T, typename Resolver>
static bool GetFunc(HMODULE hModule, const char* name, T* outPtr, Resolver resolve) {
    *outPtr = reinterpret_cast<T>(resolve(hModule, name));
    if (!*outPtr) {
        LogError("[MediaEngine] Failed to get function: %s", name);
        return false;
    }
    return true;
}

// The DLL is package-private: it ships and changes together with this executable, and a module from
// another build has other struct layouts and call semantics behind the same export names. Its build identity
// is compared with ours before a single other export is resolved or called.
template <typename Resolver>
static bool MediaEngineBuildIdentityMatches(HMODULE module, Resolver resolve) {
    namespace bid = ce::build_identity;
    const bid::Record host = bid::Current();
    bid::Record moduleIdentity = bid::Request();
    const bid::Verdict verdict = bid::CheckExport(
        reinterpret_cast<bid::ExportFn>(resolve(module, bid::kMediaEngineExportName)), host, &moduleIdentity);
    if (verdict == bid::Verdict::Match) {
        LogInfo("[MediaEngine] mediaengine.dll build identity matches: %s", bid::Describe(host).c_str());
        return true;
    }
    const bool haveModuleIdentity = verdict != bid::Verdict::ExportMissing && verdict != bid::Verdict::ExportRejected;
    LogError("[MediaEngine] Refusing mediaengine.dll: %s (this executable %s, module %s)", bid::VerdictName(verdict),
             bid::Describe(host).c_str(), haveModuleIdentity ? bid::Describe(moduleIdentity).c_str() : "unknown");
    return false;
}

// Frees the module and clears every pointer into it, whatever the reference count says. Only used when the
// module is no longer usable (a failed resolve) or when the last reference goes.
static void ReleaseMediaEngineModule() {
    if (g_MediaEngineModule) {
        FreeLibrary(g_MediaEngineModule);
        g_MediaEngineModule = nullptr;
    }
    g_MediaEngineReferences = 0;

    MediaEngine_SubmitFrameWithResultV1 = nullptr;
    MediaEngine_SubmitFrameD3D11WithResultV1 = nullptr;
    MediaEngine_RepeatLastFrameWithResultV1 = nullptr;
    MediaEngine_SetLogCallback = nullptr;
    MediaEngine_Create = nullptr;
    MediaEngine_Destroy = nullptr;
    MediaEngine_Init = nullptr;
    MediaEngine_ReloadConfig = nullptr;
    MediaEngine_SetActiveScreenGrab = nullptr;
    MediaEngine_SetWgcStartupExtraDelayQpc = nullptr;
    MediaEngine_SetScreenGrabLatencyReductionQpc = nullptr;
    MediaEngine_CanRepeatLastFrame = nullptr;
    MediaEngine_ResetRepeatFrameCache = nullptr;
    MediaEngine_PrepareFrameD3D11 = nullptr;
    MediaEngine_PrepareInjectVideo = nullptr;
    MediaEngine_StartRecording = nullptr;
    MediaEngine_StopRecording = nullptr;
    MediaEngine_GetLastOutputDegradedFlags = nullptr;
    MediaEngine_GetLastOutputPath = nullptr;
    MediaEngine_ReleaseEncoderTextures = nullptr;
    MediaEngine_GetD3D11Device = nullptr;
    MediaEngine_ReleaseSharedD3D11Device = nullptr;
    MediaEngine_CreateSharedCaptureTextures = nullptr;
    MediaEngine_GetLastFrameEncodeTimeUs = nullptr;
    MediaEngine_GetLastFrameFenceWaitUs = nullptr;
    MediaEngine_GetMuxFlowSnapshotV1 = nullptr;
    MediaEngine_QueryInjectFrameCopyCompletion = nullptr;
    MediaEngine_SetInjectTransportGeneration = nullptr;
    MediaEngine_TakeInjectSourceReadToken = nullptr;
    MediaEngine_IsInjectSourceReadComplete = nullptr;
    MediaEngine_SetInjectSourceReadWakeEvent = nullptr;
    MediaEngine_FinishInjectSourceReads = nullptr;
    MediaEngine_SetSharedMem = nullptr;
    MediaEngine_LockD3D11 = nullptr;
    MediaEngine_UnlockD3D11 = nullptr;
    MediaEngine_SetAudioOnly = nullptr;
    MediaEngine_SetSourcePrefers10Bit = nullptr;
    MediaEngine_SetCursorCompositionSuppressed = nullptr;
    MediaEngine_QueryRetainedFrameRoute = nullptr;
    MediaEngine_ConvertToRetainedFrame = nullptr;
    MediaEngine_ReleaseRetainedFrameConverter = nullptr;
    MediaEngine_ReportCaptureGpuMemory = nullptr;
    MediaEngine_MeasureRenderEndpointLatency = nullptr;
}

template <typename Resolver>
static bool ResolveMediaEngineExports(HMODULE module, Resolver resolve) {
    if (!MediaEngineBuildIdentityMatches(module, resolve)) {
        ReleaseMediaEngineModule();  // nothing was resolved, but the module and its handle go
        return false;
    }
    bool success = true;
    success &= GetFunc(module, "MediaEngine_SubmitFrameWithResultV1", &MediaEngine_SubmitFrameWithResultV1, resolve);
    success &= GetFunc(module, "MediaEngine_SubmitFrameD3D11WithResultV1", &MediaEngine_SubmitFrameD3D11WithResultV1, resolve);
    success &= GetFunc(module, "MediaEngine_RepeatLastFrameWithResultV1", &MediaEngine_RepeatLastFrameWithResultV1, resolve);
    success &= GetFunc(module, "MediaEngine_SetLogCallback", &MediaEngine_SetLogCallback, resolve);
    success &= GetFunc(module, "MediaEngine_Create", &MediaEngine_Create, resolve);
    success &= GetFunc(module, "MediaEngine_Destroy", &MediaEngine_Destroy, resolve);
    success &= GetFunc(module, "MediaEngine_Init", &MediaEngine_Init, resolve);
    success &= GetFunc(module, "MediaEngine_ReloadConfig", &MediaEngine_ReloadConfig, resolve);
    success &= GetFunc(module, "MediaEngine_SetActiveScreenGrab", &MediaEngine_SetActiveScreenGrab, resolve);
    success &=
        GetFunc(module, "MediaEngine_SetWgcStartupExtraDelayQpc", &MediaEngine_SetWgcStartupExtraDelayQpc, resolve);
    success &= GetFunc(module, "MediaEngine_SetScreenGrabLatencyReductionQpc",
                       &MediaEngine_SetScreenGrabLatencyReductionQpc, resolve);
    success &= GetFunc(module, "MediaEngine_CanRepeatLastFrame", &MediaEngine_CanRepeatLastFrame, resolve);
    success &= GetFunc(module, "MediaEngine_ResetRepeatFrameCache", &MediaEngine_ResetRepeatFrameCache, resolve);
    success &= GetFunc(module, "MediaEngine_PrepareFrameD3D11", &MediaEngine_PrepareFrameD3D11, resolve);
    success &= GetFunc(module, "MediaEngine_PrepareInjectVideo", &MediaEngine_PrepareInjectVideo, resolve);
    success &= GetFunc(module, "MediaEngine_StartRecording", &MediaEngine_StartRecording, resolve);
    success &= GetFunc(module, "MediaEngine_StopRecording", &MediaEngine_StopRecording, resolve);
    success &= GetFunc(module, "MediaEngine_GetLastOutputDegradedFlags",
                       &MediaEngine_GetLastOutputDegradedFlags, resolve);
    success &= GetFunc(module, "MediaEngine_GetLastOutputPath", &MediaEngine_GetLastOutputPath, resolve);
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
    success &= GetFunc(module, "MediaEngine_GetMuxFlowSnapshotV1", &MediaEngine_GetMuxFlowSnapshotV1, resolve);
    success &= GetFunc(module, "MediaEngine_QueryInjectFrameCopyCompletion",
                       &MediaEngine_QueryInjectFrameCopyCompletion, resolve);
    success &= GetFunc(module, "MediaEngine_SetInjectTransportGeneration",
                       &MediaEngine_SetInjectTransportGeneration, resolve);
    success &= GetFunc(module, "MediaEngine_TakeInjectSourceReadToken",
                       &MediaEngine_TakeInjectSourceReadToken, resolve);
    success &= GetFunc(module, "MediaEngine_IsInjectSourceReadComplete",
                       &MediaEngine_IsInjectSourceReadComplete, resolve);
    success &= GetFunc(module, "MediaEngine_SetInjectSourceReadWakeEvent",
                       &MediaEngine_SetInjectSourceReadWakeEvent, resolve);
    success &= GetFunc(module, "MediaEngine_FinishInjectSourceReads", &MediaEngine_FinishInjectSourceReads,
                       resolve);
    success &= GetFunc(module, "MediaEngine_SetSharedMem", &MediaEngine_SetSharedMem, resolve);
    success &= GetFunc(module, "MediaEngine_LockD3D11", &MediaEngine_LockD3D11, resolve);
    success &= GetFunc(module, "MediaEngine_UnlockD3D11", &MediaEngine_UnlockD3D11, resolve);
    success &= GetFunc(module, "MediaEngine_SetSourcePrefers10Bit", &MediaEngine_SetSourcePrefers10Bit, resolve);
    success &= GetFunc(module, "MediaEngine_SetCursorCompositionSuppressed",
                       &MediaEngine_SetCursorCompositionSuppressed, resolve);
    success &= GetFunc(module, "MediaEngine_QueryRetainedFrameRoute", &MediaEngine_QueryRetainedFrameRoute, resolve);
    success &= GetFunc(module, "MediaEngine_ConvertToRetainedFrame", &MediaEngine_ConvertToRetainedFrame, resolve);
    success &= GetFunc(module, "MediaEngine_ReleaseRetainedFrameConverter",
                       &MediaEngine_ReleaseRetainedFrameConverter, resolve);
    success &= GetFunc(module, "MediaEngine_ReportCaptureGpuMemory", &MediaEngine_ReportCaptureGpuMemory, resolve);
    success &= GetFunc(module, "MediaEngine_SetAudioOnly", &MediaEngine_SetAudioOnly, resolve);
    success &= GetFunc(module, "MediaEngine_MeasureRenderEndpointLatency",
                       &MediaEngine_MeasureRenderEndpointLatency, resolve);

    if (!success) {
        LogError("[MediaEngine] Incompatible mediaengine.dll: required exports are missing");
        ReleaseMediaEngineModule();  // clear every pointer into the rejected module, not just its handle
        return false;
    }

    LogInfo("[MediaEngine] All function pointers resolved");
    return true;
}

bool MediaEngine_Load(const char* exeDir) {
    if (g_MediaEngineModule) {
        ++g_MediaEngineReferences;
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

    if (!ResolveMediaEngineExports(g_MediaEngineModule, &GetProcAddress))
        return false;
    g_MediaEngineReferences = 1;
    return true;
}

void MediaEngine_Unload() {
    // An unbalanced call (no reference held) changes nothing: another pipeline may own the module.
    if (g_MediaEngineReferences == 0)
        return;
    if (--g_MediaEngineReferences > 0)
        return;
    ReleaseMediaEngineModule();
}
