#include "dx11_hook_internal.h"


void InstallContextVTableHooks11(ID3D11DeviceContext* context,  const char* source) {


    if (!context) {
        return;
    }

    void** pContextVTable = *(void***)context;

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
    InstallContextVTableHookSlot11(pContextVTable, 25, (LPVOID)&DetourVSSetShaderResources11, dx11_hook_oVSSetShaderResources11,
                                   &D3D11ContextVTableOriginals::vsSetShaderResources, "VSSetShaderResources", source);
    InstallContextVTableHookSlot11(pContextVTable, 26, (LPVOID)&DetourVSSetSamplers11, dx11_hook_oVSSetSamplers11,
                                   &D3D11ContextVTableOriginals::vsSetSamplers, "VSSetSamplers", source);
    InstallContextVTableHookSlot11(pContextVTable, 31, (LPVOID)&DetourGSSetShaderResources11, dx11_hook_oGSSetShaderResources11,
                                   &D3D11ContextVTableOriginals::gsSetShaderResources, "GSSetShaderResources", source);
    InstallContextVTableHookSlot11(pContextVTable, 32, (LPVOID)&DetourGSSetSamplers11, dx11_hook_oGSSetSamplers11,
                                   &D3D11ContextVTableOriginals::gsSetSamplers, "GSSetSamplers", source);
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
    InstallContextVTableHookSlot11(pContextVTable, 59, (LPVOID)&DetourHSSetShaderResources11, dx11_hook_oHSSetShaderResources11,
                                   &D3D11ContextVTableOriginals::hsSetShaderResources, "HSSetShaderResources", source);
    InstallContextVTableHookSlot11(pContextVTable, 61, (LPVOID)&DetourHSSetSamplers11, dx11_hook_oHSSetSamplers11,
                                   &D3D11ContextVTableOriginals::hsSetSamplers, "HSSetSamplers", source);
    InstallContextVTableHookSlot11(pContextVTable, 63, (LPVOID)&DetourDSSetShaderResources11, dx11_hook_oDSSetShaderResources11,
                                   &D3D11ContextVTableOriginals::dsSetShaderResources, "DSSetShaderResources", source);
    InstallContextVTableHookSlot11(pContextVTable, 65, (LPVOID)&DetourDSSetSamplers11, dx11_hook_oDSSetSamplers11,
                                   &D3D11ContextVTableOriginals::dsSetSamplers, "DSSetSamplers", source);
    InstallContextVTableHookSlot11(pContextVTable, 67, (LPVOID)&DetourCSSetShaderResources11, dx11_hook_oCSSetShaderResources11,
                                   &D3D11ContextVTableOriginals::csSetShaderResources, "CSSetShaderResources", source);
    InstallContextVTableHookSlot11(pContextVTable, 70, (LPVOID)&DetourCSSetSamplers11, dx11_hook_oCSSetSamplers11,
                                   &D3D11ContextVTableOriginals::csSetSamplers, "CSSetSamplers", source);

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

            // CreateSamplerState (Index 9)
            if (dx11_hook_oCreateSamplerState10 == NULL) {
                if (VTableHook::Create(reinterpret_cast<void*>(&pDeviceVTable[9]), (LPVOID)&DetourCreateSamplerState10,
                                       (LPVOID*)&dx11_hook_oCreateSamplerState10) == VTableHook::Success) {
                    HookLog("DX10: CreateSamplerState hook installed");
                }
            }
            pDevice10->Release();
        }
    }

}
