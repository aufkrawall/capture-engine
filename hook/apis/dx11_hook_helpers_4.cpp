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

OverlayConfig GetActiveDX11OverlayConfig(SharedMemoryLayout* shm) {


    OverlayConfig cfg{};
    cfg.captureIncludeOverlay = true;
    cfg.screenshotIncludeOverlay = true;
    if (shm) {
        cfg = shm->ReadOverlayConfig();
    }
    return cfg;

}

void CaptureDX11Screenshot(IDXGISwapChain* pSwapChain,  SharedMemoryLayout* shm,  uint64_t requestId) {


    bool queued = false;
    ID3D11Texture2D* backbuffer = nullptr;
    UINT bbIdx = ResolveDX11BackBufferIndex(pSwapChain);
    pSwapChain->GetBuffer(bbIdx, IID_PPV_ARGS(&backbuffer));
    if (backbuffer) {
        ID3D11Device* device = nullptr;
        backbuffer->GetDevice(&device);
        if (device) {
            ID3D11DeviceContext* context = nullptr;
            device->GetImmediateContext(&context);
            if (context) {
                D3D11_TEXTURE2D_DESC textureDesc{};
                backbuffer->GetDesc(&textureDesc);
                const auto presentationEncoding =
                    DXGIShared::ResolveSwapChainPresentationEncoding(pSwapChain, textureDesc.Format);
                queued = SaveD3D11TextureAsScreenshotRaw(device, context, backbuffer, shm, requestId,
                                                         presentationEncoding);
                context->Release();
            }
            device->Release();
        }
        backbuffer->Release();
    }
    if (!queued)
        CompleteScreenshotRequest(shm, requestId, ScreenshotRequestStatus::Failed, ERROR_READ_FAULT);

}

void CaptureDX10Screenshot(IDXGISwapChain* pSwapChain,  SharedMemoryLayout* shm,  uint64_t requestId) {


    ID3D10Device* device = nullptr;
    if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D10Device), (void**)&device))) {
        ID3D10Device1* device10_1 = nullptr;
        if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D10Device1), (void**)&device10_1))) {
            CompleteScreenshotRequest(shm, requestId, ScreenshotRequestStatus::Failed, ERROR_NOT_SUPPORTED);
            return;
        }
        device = device10_1;
    }

    bool queued = false;
    ID3D10Texture2D* backbuffer = nullptr;
    UINT bbIdx = ResolveDX11BackBufferIndex(pSwapChain);
    if (SUCCEEDED(pSwapChain->GetBuffer(bbIdx, __uuidof(ID3D10Texture2D), (void**)&backbuffer))) {
        D3D10_TEXTURE2D_DESC bbDesc;
        backbuffer->GetDesc(&bbDesc);

        D3D10_TEXTURE2D_DESC stagingDesc = bbDesc;
        stagingDesc.Usage = D3D10_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;

        ID3D10Texture2D* staging = nullptr;
        if (SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))) {
            device->CopyResource(staging, backbuffer);
            D3D10_MAPPED_TEXTURE2D mapped;
            if (SUCCEEDED(staging->Map(0, D3D10_MAP_READ, 0, &mapped))) {
                ScreenshotPixelFormat pixelFormat = ScreenshotPixelFormat::BGRA8;
                ScreenshotColorEncoding colorEncoding = ScreenshotColorEncoding::SRGB;
                const auto presentationEncoding =
                    DXGIShared::ResolveSwapChainPresentationEncoding(pSwapChain, bbDesc.Format);
                if (bbDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || bbDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                    pixelFormat = ScreenshotPixelFormat::RGBA8;
                } else if (bbDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
                    pixelFormat = ScreenshotPixelFormat::R10G10B10A2;
                    colorEncoding = presentationEncoding == ce::presentation_color::Encoding::Hdr10Pq
                                        ? ScreenshotColorEncoding::BT2020_PQ
                                        : ScreenshotColorEncoding::BT709_G22;
                } else if (bbDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                    pixelFormat = ScreenshotPixelFormat::RGBA16F;
                    colorEncoding = ScreenshotColorEncoding::LinearScRGB;
                }
                if (presentationEncoding != ce::presentation_color::Encoding::Unsupported) {
                    queued = QueueScreenshotPixels(shm, requestId, static_cast<const uint8_t*>(mapped.pData),
                                                   bbDesc.Width, bbDesc.Height, mapped.RowPitch, pixelFormat,
                                                   colorEncoding);
                }
                staging->Unmap(0);
            }
            staging->Release();
        }
        backbuffer->Release();
    }

    device->Release();
    if (!queued)
        CompleteScreenshotRequest(shm, requestId, ScreenshotRequestStatus::Failed, ERROR_READ_FAULT);

}

void CaptureRequestedDX11Screenshot(IDXGISwapChain* pSwapChain,  SharedMemoryLayout* shm,  uint64_t requestId) {


    if (!shm || requestId == 0)
        return;

    const DXGIShared::APIType swapChainApi = DetectSwapChainAPITypeForDX11Hook(pSwapChain);
    if (swapChainApi == DXGIShared::APIType::D3D10) {
        CaptureDX10Screenshot(pSwapChain, shm, requestId);
        return;
    }

    if (swapChainApi != DXGIShared::APIType::D3D11) {
        HookLog("DX11 Screenshot: Unsupported swapchain API %s", GetDX11HookBaseAPIName(swapChainApi));
        CompleteScreenshotRequest(shm, requestId, ScreenshotRequestStatus::Failed, ERROR_NOT_SUPPORTED);
        return;
    }

    CaptureDX11Screenshot(pSwapChain, shm, requestId);

}

void ProcessDX11FrameWithOverlayOrdering(IDXGISwapChain* pSwapChain) {


    if (!pSwapChain)
        return;

    UINT frameBufferIndex = ResolveDX11BackBufferIndex(pSwapChain);
    dx11_hook_g_ForcedCaptureBackBufferIndex = static_cast<int>(frameBufferIndex);
    auto indexGuard = ce::make_scope_guard([]() { dx11_hook_g_ForcedCaptureBackBufferIndex = -1; });

    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    DXGI_SWAP_CHAIN_DESC swapChainDesc{};
    if (shm) {
        auto presentationEncoding = ce::presentation_color::Encoding::Unsupported;
        if (SUCCEEDED(pSwapChain->GetDesc(&swapChainDesc))) {
            presentationEncoding =
                DXGIShared::ResolveSwapChainPresentationEncoding(pSwapChain, swapChainDesc.BufferDesc.Format);
        }
        shm->SetIsHDR(ce::presentation_color::IsHDR(presentationEncoding));
    }
    OverlayConfig overlayCfg = GetActiveDX11OverlayConfig(shm);
    const bool shouldDrawOverlay = shm && overlayCfg.showOverlay;
    const bool captureAfterOverlay = shouldDrawOverlay && overlayCfg.captureIncludeOverlay;
    const uint64_t screenshotRequestId = GetPendingScreenshotRequestId(shm);
    const bool screenshotRequested = screenshotRequestId != 0;
    const bool screenshotAfterOverlay = shouldDrawOverlay && overlayCfg.screenshotIncludeOverlay;

    auto doCapture = [&](bool afterOverlay) {
        if (g_IPC && g_IPC->IsRecording() && !ShouldSkipCaptureForTargetCadence(shm, "DX11")) {
            dx11_hook_g_CaptureUsesOverlayRTV = afterOverlay;
            auto captureGuard = ce::make_scope_guard([]() { dx11_hook_g_CaptureUsesOverlayRTV = false; });
            dx11_hook_g_DX11Capture.CaptureFrame(pSwapChain);
        }
    };

    auto doScreenshot = [&]() {
        if (screenshotRequested) {
            CaptureRequestedDX11Screenshot(pSwapChain, shm, screenshotRequestId);
        }
    };

    if (!captureAfterOverlay) {
        doCapture(false);
    }
    if (screenshotRequested && !screenshotAfterOverlay) {
        doScreenshot();
    }
    if (shouldDrawOverlay) {
        DrawDX11Overlay(pSwapChain);
    }
    if (captureAfterOverlay) {
        doCapture(true);
    }
    if (screenshotRequested && screenshotAfterOverlay) {
        doScreenshot();
    }

}
