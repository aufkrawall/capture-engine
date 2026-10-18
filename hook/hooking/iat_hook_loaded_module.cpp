#include "iat_hook.h"
#include "hook/d3d11/dx11_hook.h"
#include "hook/overlay/overlay_compat.h"
#include "hook/wrappers/wrapper_hooks.h"
#include "common/logging/log_meter.h"

namespace IATHook {

bool IsGraphicsImportHookTargetModulePath(const wchar_t* path) {
    return path && !wcsstr(path, L"capture_hook") && !wcsstr(path, L"d3d12_wrappers") &&
           !ce::overlay_compat::IsThirdPartyOverlayModulePath(path) &&
           !IsNonSystemGraphicsProxyModulePath(path) &&
           !ce::overlay_compat::IsStreamlineFrameGenerationModulePath(path);
}

void PatchDynamicGraphicsImportsForLoadedModule(HMODULE module) {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = module ? GetModuleFileNameW(module, path, MAX_PATH) : 0;
    if (!length || length >= MAX_PATH || !IsGraphicsImportHookTargetModulePath(path)) return;

    // Offer each new importer its own pass; post-load observation retries slots
    // that the loader had not bound at notification time.
    // Repeated LoadLibrary on an existing runtime must never rescan the process.
    void* original = nullptr;
    const bool resolver = PatchIAT(module, "kernel32.dll", "GetProcAddress", (void*)DetourGetProcAddress, &original, true);
    // d3d11.dll can map as this module's dependency without a load CE observes (d3d10_1.dll
    // creates its devices through D3D11CreateDeviceAndSwapChain), before anything resolved the
    // wrappers' originals; a wrapper without one fails every call (D3D10.1 device creation returned
    // E_FAIL). Resolve them first and leave the imports alone without them.
    const bool originalsBefore = ::oD3D11CreateDevice && ::oD3D11CreateDeviceAndSwapChain;
    const bool originals = ResolveD3D11Originals();
    const bool device = originals && PatchIAT(module, "d3d11.dll", "D3D11CreateDevice",
                                              (void*)::Wrapped_D3D11CreateDevice, &original, true);
    const bool swapchain = originals && PatchIAT(module, "d3d11.dll", "D3D11CreateDeviceAndSwapChain",
                                                 (void*)::Wrapped_D3D11CreateDeviceAndSwapChain, &original, true);
    if (originals != originalsBefore) {
        static ce::log_meter::ChangeGate resolvedLog;
        if (const auto verdict = resolvedLog.Observe(ce::log_meter::FieldKey(module))) {
            WrapperLog("IAT: D3D11 originals resolved on the load of module=%p (device=%d swapchain=%d)%s", module,
                       device ? 1 : 0, swapchain ? 1 : 0, ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
    }
    if (resolver || device || swapchain) {
        static ce::log_meter::ChangeGate log;
        const auto verdict = log.Observe(ce::log_meter::FieldKey(module, resolver, device, swapchain));
        if (verdict) {
            WrapperLog("IAT: Loaded-module graphics imports module=%p resolver=%d device=%d swapchain=%d%s",
                       module, resolver ? 1 : 0, device ? 1 : 0, swapchain ? 1 : 0,
                       ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
    }
}

}  // namespace IATHook
