#include "dx12_fsr_fg_test_internal.h"

#include <cstdio>
#include <cstdlib>

ffxReturnCode_t TestPresentCallback(ffxCallbackDescFrameGenerationPresent* params, void*) {
    uint64_t callbackIndex = ++g_FsrPresentCallbackCount;
    if (params) {
        auto* cmdList = static_cast<ID3D12GraphicsCommandList*>(params->commandList);
        testapp::dx12fg::CopyFfxPresentSourceToOutput(cmdList, params);
        if (callbackIndex <= 5 || (callbackIndex % 120) == 0) {
            testapp::Log("[FG-DIAG] FSR present callback #%llu frameID=%llu generated=%d backbuffer=%p output=%p\n",
                         static_cast<unsigned long long>(callbackIndex),
                         static_cast<unsigned long long>(params->frameID), params->isGeneratedFrame ? 1 : 0,
                         params->currentBackBuffer.resource, params->outputSwapChainBuffer.resource);
        }
    }
    return FFX_API_RETURN_OK;
}
ffxReturnCode_t TestFrameGenerationCallback(ffxDispatchDescFrameGeneration* params, void* pUserCtx) {
    if (!params || !pUserCtx || !g_FfxDispatch) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    ffxContext* context = reinterpret_cast<ffxContext*>(pUserCtx);
    ffxReturnCode_t ret = g_FfxDispatch(context, &params->header);
    uint64_t callbackIndex = ++g_FsrFrameGenerationCallbackCount;
    if (ret != FFX_API_RETURN_OK || callbackIndex <= 5 || (callbackIndex % 120) == 0) {
        testapp::Log("[FG-DIAG] FSR frame-generation callback #%llu frameID=%llu result=%u present=%p output0=%p\n",
                     static_cast<unsigned long long>(callbackIndex), static_cast<unsigned long long>(params->frameID),
                     ret, params->presentColor.resource, params->outputs[0].resource);
    }
    return ret;
}
const char* FfxReturnName(ffxReturnCode_t code) {
    switch (code) {
        case FFX_API_RETURN_OK:
            return "OK";
        case FFX_API_RETURN_ERROR:
            return "ERROR";
        case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE:
            return "UNKNOWN_DESCTYPE";
        case FFX_API_RETURN_ERROR_RUNTIME_ERROR:
            return "RUNTIME_ERROR";
        case FFX_API_RETURN_NO_PROVIDER:
            return "NO_PROVIDER";
        case FFX_API_RETURN_ERROR_MEMORY:
            return "MEMORY";
        case FFX_API_RETURN_ERROR_PARAMETER:
            return "PARAMETER";
        case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE:
            return "PROVIDER_NO_SUPPORT_NEW_DESCTYPE";
        default:
            return "unknown";
    }
}
void PreloadAmdCompanionDlls() {
    const wchar_t* companionDlls[] = {L"amd_ags_x64.dll", L"amd_acs_x64.dll"};
    for (const wchar_t* dllName : companionDlls) {
        HMODULE companion = LoadLibraryW(dllName);
        if (companion) {
            testapp::Log("  Preloaded AMD companion: %S\n", dllName);
        } else {
            testapp::Log("  Failed to preload AMD companion %S (err=%lu)\n", dllName, GetLastError());
        }
    }
}
FsrInitResult LoadFSRRuntime() {
    if (g_FfxModule)
        return kFsrOk;

    PreloadAmdCompanionDlls();

    // Prefer the per-effect FG DLL directly (the loader DLL's ffxCreateContext
    // delegates to this, but going direct avoids any loader-side issues).
    const wchar_t* dllNames[] = {
        L"amd_fidelityfx_framegeneration_dx12.dll",
        L"amd_fidelityfx_loader_dx12.dll",
        L"amd_fidelityfx_dx12.dll",
        L"ffx_framegeneration.dll",
    };
    for (auto dllName : dllNames) {
        g_FfxModule = LoadLibraryW(dllName);
        if (g_FfxModule) {
            testapp::Log("  Loaded FSR runtime: %S\n", dllName);
            break;
        }
    }
    if (!g_FfxModule)
        return kFsrNoDll;

    g_FfxCreateContext = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(g_FfxModule, "ffxCreateContext"));
    g_FfxConfigure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(g_FfxModule, "ffxConfigure"));
    g_FfxDispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(g_FfxModule, "ffxDispatch"));
    g_FfxDestroyContext = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(g_FfxModule, "ffxDestroyContext"));
    if (!g_FfxCreateContext || !g_FfxConfigure || !g_FfxDispatch || !g_FfxDestroyContext) {
        testapp::Log("  FSR DLL missing ffxCreateContext/ffxConfigure/ffxDispatch/ffxDestroyContext exports\n");
        FreeLibrary(g_FfxModule);
        g_FfxModule = nullptr;
        return kFsrNoExports;
    }
    return kFsrOk;
}
void UnloadFSRRuntime() {
    if (g_FfxModule) {

        FreeLibrary(g_FfxModule);
        g_FfxModule = nullptr;
    }
    g_FfxCreateContext = nullptr;
    g_FfxConfigure = nullptr;
    g_FfxDispatch = nullptr;
    g_FfxDestroyContext = nullptr;
}
