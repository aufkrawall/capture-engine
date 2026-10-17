#include "dx11_hook_internal.h"

#include "d3d10_vtable_slots.h"


namespace {

// The slots CE intercepts on every context it hooks, with their detours (the same set the slot
// hooks below install one by one).
size_t ContextDetours11(bool context1, ce::d3d11_dispatch::Detour* out) {
    namespace slots = ce::d3d11_slots;
    const ce::d3d11_dispatch::Detour detours[] = {
        {slots::kPSSetShaderResources, reinterpret_cast<void*>(&DetourPSSetShaderResources11)},
        {slots::kPSSetShader, reinterpret_cast<void*>(&DetourPSSetShader11)},
        {slots::kPSSetSamplers, reinterpret_cast<void*>(&DetourPSSetSamplers11)},
        {slots::kDrawIndexed, reinterpret_cast<void*>(&DetourDrawIndexed11)},
        {slots::kDraw, reinterpret_cast<void*>(&DetourDraw11)},
        {slots::kDrawIndexedInstanced, reinterpret_cast<void*>(&DetourDrawIndexedInstanced11)},
        {slots::kDrawInstanced, reinterpret_cast<void*>(&DetourDrawInstanced11)},
        {slots::kDrawAuto, reinterpret_cast<void*>(&DetourDrawAuto11)},
        {slots::kDrawIndexedInstancedIndirect, reinterpret_cast<void*>(&DetourDrawIndexedInstancedIndirect11)},
        {slots::kDrawInstancedIndirect, reinterpret_cast<void*>(&DetourDrawInstancedIndirect11)},
        {slots::kExecuteCommandList, reinterpret_cast<void*>(&DetourExecuteCommandList11)},
        {slots::kPSGetSamplers, reinterpret_cast<void*>(&DetourPSGetSamplers11)},
        {slots::kClearState, reinterpret_cast<void*>(&DetourClearState11)},
        {slots::kFinishCommandList, reinterpret_cast<void*>(&DetourFinishCommandList11)},
        {slots::kSwapDeviceContextState, reinterpret_cast<void*>(&DetourSwapDeviceContextState11)},
    };
    constexpr size_t kAll = sizeof(detours) / sizeof(detours[0]);
    const size_t count = context1 ? kAll : kAll - 1;  // SwapDeviceContextState is ID3D11DeviceContext1's
    for (size_t i = 0; i < count; ++i) {
        out[i] = detours[i];
    }
    return count;
}

}  // namespace

bool ContextVTableHooksActive11(ID3D11DeviceContext* context) {
    return (context && ce::d3d11_dispatch::RuntimeTable(context)) ||
           (dx11_hook_oPSSetSamplers11 && dx11_hook_oPSSetShaderResources11);
}

void InstallContextVTableHooks11(ID3D11DeviceContext* context,  const char* source) {


    if (!context) {
        return;
    }

    // The slot exists only on an ID3D11DeviceContext1 vtable.
    bool context1 = false;
    {
        ID3D11DeviceContext1* probe = nullptr;
        D3D11InternalIdentityProbeScope identityProbeScope;
        if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&probe))) && probe) {
            probe->Release();
            context1 = true;
        }
    }

    // A function table the runtime rewrites (d3d11.dll's own contexts) gets a CE table instead
    // of slot hooks the runtime would overwrite (d3d11_context_dispatch.h).
    ce::d3d11_dispatch::Detour detours[16];
    const size_t detourCount = ContextDetours11(context1, detours);
    const ce::d3d11_dispatch::AdoptResult adopted = ce::d3d11_dispatch::Adopt(context, detours, detourCount, source);
    if (adopted == ce::d3d11_dispatch::AdoptResult::kAdopted ||
        adopted == ce::d3d11_dispatch::AdoptResult::kAlreadyAdopted) {
        return;
    }

    void** pContextVTable = *(void***)context;

    // Forced AF only ever replaces pixel-shader samplers, so only the pixel
    // stage's bindings are tracked. Other stages' SRV/sampler binds stay
    // unhooked: every game state change there would otherwise pay a detour,
    // the global state lock and reference counting for state no one reads.
    InstallContextVTableHookSlot11(pContextVTable, 8, (LPVOID)&DetourPSSetShaderResources11, dx11_hook_oPSSetShaderResources11,
                                   &D3D11ContextVTableOriginals::psSetShaderResources, "PSSetShaderResources", source);
    InstallContextVTableHookSlot11(pContextVTable, 9, (LPVOID)&DetourPSSetShader11, dx11_hook_oPSSetShader11,
                                   &D3D11ContextVTableOriginals::psSetShader, "PSSetShader", source);
    InstallContextVTableHookSlot11(pContextVTable, 10, (LPVOID)&DetourPSSetSamplers11, dx11_hook_oPSSetSamplers11,
                                   &D3D11ContextVTableOriginals::psSetSamplers, "PSSetSamplers", source);
    InstallContextVTableHookSlot11(pContextVTable, 12, (LPVOID)&DetourDrawIndexed11, dx11_hook_oDrawIndexed11,
                                   &D3D11ContextVTableOriginals::drawIndexed, "DrawIndexed", source);
    InstallContextVTableHookSlot11(pContextVTable, 13, (LPVOID)&DetourDraw11, dx11_hook_oDraw11,
                                   &D3D11ContextVTableOriginals::draw, "Draw", source);
    InstallContextVTableHookSlot11(pContextVTable, 20, (LPVOID)&DetourDrawIndexedInstanced11, dx11_hook_oDrawIndexedInstanced11,
                                   &D3D11ContextVTableOriginals::drawIndexedInstanced, "DrawIndexedInstanced", source);
    InstallContextVTableHookSlot11(pContextVTable, 21, (LPVOID)&DetourDrawInstanced11, dx11_hook_oDrawInstanced11,
                                   &D3D11ContextVTableOriginals::drawInstanced, "DrawInstanced", source);
    InstallContextVTableHookSlot11(pContextVTable, 38, (LPVOID)&DetourDrawAuto11, dx11_hook_oDrawAuto11,
                                   &D3D11ContextVTableOriginals::drawAuto, "DrawAuto", source);
    InstallContextVTableHookSlot11(
        pContextVTable, 39, (LPVOID)&DetourDrawIndexedInstancedIndirect11, dx11_hook_oDrawIndexedInstancedIndirect11,
        &D3D11ContextVTableOriginals::drawIndexedInstancedIndirect, "DrawIndexedInstancedIndirect", source);
    InstallContextVTableHookSlot11(pContextVTable, 40, (LPVOID)&DetourDrawInstancedIndirect11, dx11_hook_oDrawInstancedIndirect11,
                                   &D3D11ContextVTableOriginals::drawInstancedIndirect, "DrawInstancedIndirect",
                                   source);
    InstallContextVTableHookSlot11(pContextVTable, 58, (LPVOID)&DetourExecuteCommandList11, dx11_hook_oExecuteCommandList11,
                                   &D3D11ContextVTableOriginals::executeCommandList, "ExecuteCommandList", source);
    // State resets and the read-back the shared AF engine keeps consistent, as the wrapper does.
    InstallContextVTableHookSlot11(pContextVTable, ce::d3d11_slots::kPSGetSamplers, (LPVOID)&DetourPSGetSamplers11,
                                   dx11_hook_oPSGetSamplers11, &D3D11ContextVTableOriginals::psGetSamplers,
                                   "PSGetSamplers", source);
    InstallContextVTableHookSlot11(pContextVTable, ce::d3d11_slots::kClearState, (LPVOID)&DetourClearState11,
                                   dx11_hook_oClearState11, &D3D11ContextVTableOriginals::clearState, "ClearState",
                                   source);
    InstallContextVTableHookSlot11(pContextVTable, ce::d3d11_slots::kFinishCommandList,
                                   (LPVOID)&DetourFinishCommandList11, dx11_hook_oFinishCommandList11,
                                   &D3D11ContextVTableOriginals::finishCommandList, "FinishCommandList", source);
    if (context1) {
        InstallContextVTableHookSlot11(pContextVTable, ce::d3d11_slots::kSwapDeviceContextState,
                                       (LPVOID)&DetourSwapDeviceContextState11, dx11_hook_oSwapDeviceContextState11,
                                       &D3D11ContextVTableOriginals::swapDeviceContextState, "SwapDeviceContextState",
                                       source);
    }

}
void InstallVTableHooks(ID3D11Device* pDevice,  ID3D11DeviceContext* pContext,  IDXGISwapChain* pSwapChain) {


    // Hook D3D11 Device methods
    if (pDevice) {
        DX11Hook_RegisterDeviceIdentity(pDevice, "D3D11 device hook installation");
        InstallD3D11IdentityQueryHook(pDevice, "device");
        void** pDeviceVTable = *(void***)pDevice;
        EnsureVTableHookSlot11(pDeviceVTable, 15, (LPVOID)&DetourCreatePixelShader11, dx11_hook_oCreatePixelShader11,
                               "CreatePixelShader");
        // Index 23 is CreateSamplerState for D3D11
        EnsureVTableHookSlot11(pDeviceVTable, 23, (LPVOID)&DetourCreateSamplerState, dx11_hook_oCreateSamplerState,
                               "CreateSamplerState");
        EnsureVTableHookSlot11(pDeviceVTable, 27, (LPVOID)&DetourCreateDeferredContext11, dx11_hook_oCreateDeferredContext11,
                               "CreateDeferredContext");
    }

    InstallD3D11IdentityQueryHook(pContext, "context");
    InstallContextVTableHooks11(pContext, "immediate");

    // Some DX11 implementations expose D3D10 compatibility interfaces too.
    // Only install the D3D10 runtime hooks when the swapchain actually belongs
    // to a D3D10 device.
    if (pSwapChain && DetectSwapChainAPITypeForDX11Hook(pSwapChain) == DXGIShared::APIType::D3D10) {
        ID3D10Device* pDevice10 = nullptr;
        HRESULT hr = pSwapChain->GetDevice(__uuidof(ID3D10Device), (void**)&pDevice10);
        if (SUCCEEDED(hr) && pDevice10) {
            void** pDeviceVTable = *(void***)pDevice10;

            if (dx11_hook_oCreateSamplerState10 == NULL) {
                if (VTableHook::Create(
                        reinterpret_cast<void*>(&pDeviceVTable[ce::d3d10_vtable_slots::kDeviceCreateSamplerState]),
                        (LPVOID)&DetourCreateSamplerState10,
                        (LPVOID*)&dx11_hook_oCreateSamplerState10) == VTableHook::Success) {
                    HookLog("DX10: CreateSamplerState hook installed (slot=%u)",
                            ce::d3d10_vtable_slots::kDeviceCreateSamplerState);
                }
            }
            pDevice10->Release();
        }
    }

}
