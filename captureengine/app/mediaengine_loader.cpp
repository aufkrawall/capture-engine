#include "mediaengine_loader.h"
#include <windows.h>
#include <cstring>
#include <filesystem>
#include "common/logging/logging.h"
#include "common/platform/build_identity_check.h"
#include "common/platform/secure_dll_loading.h"

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

    ce::mediaengine_api::MediaEngine_SubmitFrameWithResultV1 = nullptr;
    ce::mediaengine_api::MediaEngine_SubmitFrameD3D11WithResultV1 = nullptr;
    ce::mediaengine_api::MediaEngine_RepeatLastFrameWithResultV1 = nullptr;
    ce::mediaengine_api::MediaEngine_SetLogCallback = nullptr;
    ce::mediaengine_api::MediaEngine_Create = nullptr;
    ce::mediaengine_api::MediaEngine_Destroy = nullptr;
    ce::mediaengine_api::MediaEngine_Init = nullptr;
    ce::mediaengine_api::MediaEngine_ReloadConfig = nullptr;
    ce::mediaengine_api::MediaEngine_SetActiveScreenGrab = nullptr;
    ce::mediaengine_api::MediaEngine_SetWgcStartupExtraDelayQpc = nullptr;
    ce::mediaengine_api::MediaEngine_SetScreenGrabLatencyReductionQpc = nullptr;
    ce::mediaengine_api::MediaEngine_CanRepeatLastFrame = nullptr;
    ce::mediaengine_api::MediaEngine_ResetRepeatFrameCache = nullptr;
    ce::mediaengine_api::MediaEngine_PrepareFrameD3D11 = nullptr;
    ce::mediaengine_api::MediaEngine_PrepareInjectVideo = nullptr;
    ce::mediaengine_api::MediaEngine_StartRecording = nullptr;
    ce::mediaengine_api::MediaEngine_StopRecording = nullptr;
    ce::mediaengine_api::MediaEngine_GetLastOutputDegradedFlags = nullptr;
    ce::mediaengine_api::MediaEngine_GetLastOutputPath = nullptr;
    ce::mediaengine_api::MediaEngine_ReleaseEncoderTextures = nullptr;
    ce::mediaengine_api::MediaEngine_GetD3D11Device = nullptr;
    ce::mediaengine_api::MediaEngine_ReleaseSharedD3D11Device = nullptr;
    ce::mediaengine_api::MediaEngine_CreateSharedCaptureTextures = nullptr;
    ce::mediaengine_api::MediaEngine_GetLastFrameEncodeTimeUs = nullptr;
    ce::mediaengine_api::MediaEngine_GetLastFrameFenceWaitUs = nullptr;
    ce::mediaengine_api::MediaEngine_GetMuxFlowSnapshotV1 = nullptr;
    ce::mediaengine_api::MediaEngine_QueryInjectFrameCopyCompletion = nullptr;
    ce::mediaengine_api::MediaEngine_SetInjectTransportGeneration = nullptr;
    ce::mediaengine_api::MediaEngine_TakeInjectSourceReadToken = nullptr;
    ce::mediaengine_api::MediaEngine_IsInjectSourceReadComplete = nullptr;
    ce::mediaengine_api::MediaEngine_SetInjectSourceReadWakeEvent = nullptr;
    ce::mediaengine_api::MediaEngine_FinishInjectSourceReads = nullptr;
    ce::mediaengine_api::MediaEngine_SetSharedMem = nullptr;
    ce::mediaengine_api::MediaEngine_LockD3D11 = nullptr;
    ce::mediaengine_api::MediaEngine_UnlockD3D11 = nullptr;
    ce::mediaengine_api::MediaEngine_SetAudioOnly = nullptr;
    ce::mediaengine_api::MediaEngine_SetSourcePrefers10Bit = nullptr;
    ce::mediaengine_api::MediaEngine_SetCursorCompositionSuppressed = nullptr;
    ce::mediaengine_api::MediaEngine_QueryRetainedFrameRoute = nullptr;
    ce::mediaengine_api::MediaEngine_ConvertToRetainedFrame = nullptr;
    ce::mediaengine_api::MediaEngine_ReleaseRetainedFrameConverter = nullptr;
    ce::mediaengine_api::MediaEngine_ReportCaptureGpuMemory = nullptr;
    ce::mediaengine_api::MediaEngine_MeasureRenderEndpointLatency = nullptr;
}

template <typename Resolver>
static bool ResolveMediaEngineExports(HMODULE module, Resolver resolve) {
    if (!MediaEngineBuildIdentityMatches(module, resolve)) {
        ReleaseMediaEngineModule();  // nothing was resolved, but the module and its handle go
        return false;
    }
    bool success = true;
    success &= GetFunc(module, "MediaEngine_SubmitFrameWithResultV1",
                       &ce::mediaengine_api::MediaEngine_SubmitFrameWithResultV1, resolve);
    success &= GetFunc(module, "MediaEngine_SubmitFrameD3D11WithResultV1",
                       &ce::mediaengine_api::MediaEngine_SubmitFrameD3D11WithResultV1, resolve);
    success &= GetFunc(module, "MediaEngine_RepeatLastFrameWithResultV1",
                       &ce::mediaengine_api::MediaEngine_RepeatLastFrameWithResultV1, resolve);
    success &= GetFunc(module, "MediaEngine_SetLogCallback", &ce::mediaengine_api::MediaEngine_SetLogCallback, resolve);
    success &= GetFunc(module, "MediaEngine_Create", &ce::mediaengine_api::MediaEngine_Create, resolve);
    success &= GetFunc(module, "MediaEngine_Destroy", &ce::mediaengine_api::MediaEngine_Destroy, resolve);
    success &= GetFunc(module, "MediaEngine_Init", &ce::mediaengine_api::MediaEngine_Init, resolve);
    success &= GetFunc(module, "MediaEngine_ReloadConfig", &ce::mediaengine_api::MediaEngine_ReloadConfig, resolve);
    success &= GetFunc(module, "MediaEngine_SetActiveScreenGrab", &ce::mediaengine_api::MediaEngine_SetActiveScreenGrab,
                       resolve);
    success &=
        GetFunc(module, "MediaEngine_SetWgcStartupExtraDelayQpc",
                &ce::mediaengine_api::MediaEngine_SetWgcStartupExtraDelayQpc, resolve);
    success &= GetFunc(module, "MediaEngine_SetScreenGrabLatencyReductionQpc",
                       &ce::mediaengine_api::MediaEngine_SetScreenGrabLatencyReductionQpc, resolve);
    success &= GetFunc(module, "MediaEngine_CanRepeatLastFrame", &ce::mediaengine_api::MediaEngine_CanRepeatLastFrame,
                       resolve);
    success &= GetFunc(module, "MediaEngine_ResetRepeatFrameCache",
                       &ce::mediaengine_api::MediaEngine_ResetRepeatFrameCache, resolve);
    success &= GetFunc(module, "MediaEngine_PrepareFrameD3D11", &ce::mediaengine_api::MediaEngine_PrepareFrameD3D11,
                       resolve);
    success &= GetFunc(module, "MediaEngine_PrepareInjectVideo", &ce::mediaengine_api::MediaEngine_PrepareInjectVideo,
                       resolve);
    success &= GetFunc(module, "MediaEngine_StartRecording", &ce::mediaengine_api::MediaEngine_StartRecording, resolve);
    success &= GetFunc(module, "MediaEngine_StopRecording", &ce::mediaengine_api::MediaEngine_StopRecording, resolve);
    success &= GetFunc(module, "MediaEngine_GetLastOutputDegradedFlags",
                       &ce::mediaengine_api::MediaEngine_GetLastOutputDegradedFlags, resolve);
    success &= GetFunc(module, "MediaEngine_GetLastOutputPath", &ce::mediaengine_api::MediaEngine_GetLastOutputPath,
                       resolve);
    success &= GetFunc(module, "MediaEngine_ReleaseEncoderTextures",
                       &ce::mediaengine_api::MediaEngine_ReleaseEncoderTextures, resolve);
    success &= GetFunc(module, "MediaEngine_GetD3D11Device", &ce::mediaengine_api::MediaEngine_GetD3D11Device, resolve);
    success &=
        GetFunc(module, "MediaEngine_ReleaseSharedD3D11Device",
                &ce::mediaengine_api::MediaEngine_ReleaseSharedD3D11Device, resolve);
    success &= GetFunc(module, "MediaEngine_CreateSharedCaptureTextures",
                       &ce::mediaengine_api::MediaEngine_CreateSharedCaptureTextures, resolve);
    success &=
        GetFunc(module, "MediaEngine_GetLastFrameEncodeTimeUs",
                &ce::mediaengine_api::MediaEngine_GetLastFrameEncodeTimeUs, resolve);
    success &=
        GetFunc(module, "MediaEngine_GetLastFrameFenceWaitUs",
                &ce::mediaengine_api::MediaEngine_GetLastFrameFenceWaitUs, resolve);
    success &= GetFunc(module, "MediaEngine_GetMuxFlowSnapshotV1",
                       &ce::mediaengine_api::MediaEngine_GetMuxFlowSnapshotV1, resolve);
    success &= GetFunc(module, "MediaEngine_QueryInjectFrameCopyCompletion",
                       &ce::mediaengine_api::MediaEngine_QueryInjectFrameCopyCompletion, resolve);
    success &= GetFunc(module, "MediaEngine_SetInjectTransportGeneration",
                       &ce::mediaengine_api::MediaEngine_SetInjectTransportGeneration, resolve);
    success &= GetFunc(module, "MediaEngine_TakeInjectSourceReadToken",
                       &ce::mediaengine_api::MediaEngine_TakeInjectSourceReadToken, resolve);
    success &= GetFunc(module, "MediaEngine_IsInjectSourceReadComplete",
                       &ce::mediaengine_api::MediaEngine_IsInjectSourceReadComplete, resolve);
    success &= GetFunc(module, "MediaEngine_SetInjectSourceReadWakeEvent",
                       &ce::mediaengine_api::MediaEngine_SetInjectSourceReadWakeEvent, resolve);
    success &= GetFunc(module, "MediaEngine_FinishInjectSourceReads",
                       &ce::mediaengine_api::MediaEngine_FinishInjectSourceReads,
                       resolve);
    success &= GetFunc(module, "MediaEngine_SetSharedMem", &ce::mediaengine_api::MediaEngine_SetSharedMem, resolve);
    success &= GetFunc(module, "MediaEngine_LockD3D11", &ce::mediaengine_api::MediaEngine_LockD3D11, resolve);
    success &= GetFunc(module, "MediaEngine_UnlockD3D11", &ce::mediaengine_api::MediaEngine_UnlockD3D11, resolve);
    success &= GetFunc(module, "MediaEngine_SetSourcePrefers10Bit",
                       &ce::mediaengine_api::MediaEngine_SetSourcePrefers10Bit, resolve);
    success &= GetFunc(module, "MediaEngine_SetCursorCompositionSuppressed",
                       &ce::mediaengine_api::MediaEngine_SetCursorCompositionSuppressed, resolve);
    success &= GetFunc(module, "MediaEngine_QueryRetainedFrameRoute",
                       &ce::mediaengine_api::MediaEngine_QueryRetainedFrameRoute, resolve);
    success &= GetFunc(module, "MediaEngine_ConvertToRetainedFrame",
                       &ce::mediaengine_api::MediaEngine_ConvertToRetainedFrame, resolve);
    success &= GetFunc(module, "MediaEngine_ReleaseRetainedFrameConverter",
                       &ce::mediaengine_api::MediaEngine_ReleaseRetainedFrameConverter, resolve);
    success &= GetFunc(module, "MediaEngine_ReportCaptureGpuMemory",
                       &ce::mediaengine_api::MediaEngine_ReportCaptureGpuMemory, resolve);
    success &= GetFunc(module, "MediaEngine_SetAudioOnly", &ce::mediaengine_api::MediaEngine_SetAudioOnly, resolve);
    success &= GetFunc(module, "MediaEngine_MeasureRenderEndpointLatency",
                       &ce::mediaengine_api::MediaEngine_MeasureRenderEndpointLatency, resolve);

    if (!success) {
        LogError("[MediaEngine] Incompatible mediaengine.dll: required exports are missing");
        ReleaseMediaEngineModule();  // clear every pointer into the rejected module, not just its handle
        return false;
    }

    LogInfo("[MediaEngine] All function pointers resolved");
    return true;
}

bool MediaEngine_Load(const std::filesystem::path& directory, MediaEngineDllSearch search) {
    if (g_MediaEngineModule) {
        ++g_MediaEngineReferences;
        return true;
    }

    const std::filesystem::path executableDirectory = std::filesystem::absolute(directory).lexically_normal();
    const std::filesystem::path ffmpegDirectory = executableDirectory / L"ffmpeg";
    const std::filesystem::path dllPath = executableDirectory / L"mediaengine.dll";
    DWORD loadError = ERROR_SUCCESS;
    if (search == MediaEngineDllSearch::ProcessDefault) {
        if (!ce::security::EnsureSecureDllSearchDirectory(ffmpegDirectory, &loadError)) {
            LogError("[MediaEngine] Failed to register secure FFmpeg directory: error %lu", loadError);
            return false;
        }
        LogInfo("[MediaEngine] Registered restricted FFmpeg dependency directory");
        LogInfo("[MediaEngine] Loading mediaengine.dll from the package directory");
        g_MediaEngineModule = ce::security::LoadLibraryFromSecurePath(dllPath, &loadError);
    } else {
        LogInfo("[MediaEngine] Loading mediaengine.dll from the package directory (load-scoped FFmpeg directory)");
        g_MediaEngineModule = ce::security::LoadLibraryFromScopedDirectory(dllPath, ffmpegDirectory, &loadError);
    }
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
