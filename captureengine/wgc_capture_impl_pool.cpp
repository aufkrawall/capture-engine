#include "wgc_capture_internal.h"


#if HAS_WGC

void WGCCapture::Impl::ReleasePoolConversionResources() {


        for (auto* rtv : poolRenderTargetViews_) {
            SafeRelease(rtv);
        }
        poolRenderTargetViews_.clear();
        SafeRelease(poolCopyStagingSrv_);
        SafeRelease(poolCopyStagingTexture_);
        poolCopyStagingWidth_ = 0;
        poolCopyStagingHeight_ = 0;
        poolCopyStagingFormat_ = DXGI_FORMAT_UNKNOWN;
        SafeRelease(poolCopyCB_);
        SafeRelease(poolCopySampler_);
        SafeRelease(poolCopyPS_);
        SafeRelease(poolCopyVS_);
        lastPoolConvertUs_.store(0, std::memory_order_relaxed);

}

bool WGCCapture::Impl::EnsurePoolCopyShader() {


        if (poolCopyVS_ && poolCopyPS_ && poolCopySampler_ && poolCopyCB_) {
            return true;
        }
        if (!d3dDevice_) {
            return false;
        }

        HMODULE d3dCompiler = ce::security::LoadSystemLibrary(L"d3dcompiler_47.dll");
        if (!d3dCompiler) {
            LogError("[WGC] Failed to load d3dcompiler_47.dll for retained-copy conversion");
            return false;
        }

        typedef HRESULT(WINAPI * PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                                 LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        auto d3dCompile = reinterpret_cast<PFN_D3DCompile>(GetProcAddress(d3dCompiler, "D3DCompile"));
        if (!d3dCompile) {
            LogError("[WGC] Failed to resolve D3DCompile for retained-copy conversion");
            FreeLibrary(d3dCompiler);
            return false;
        }

        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* psBlob = nullptr;
        ID3DBlob* errBlob = nullptr;
        HRESULT hr = d3dCompile(WGC_POOL_COPY_SHADER_SRC, strlen(WGC_POOL_COPY_SHADER_SRC), nullptr, nullptr, nullptr,
                                "VS_Main", "vs_4_0", 0, 0, &vsBlob, &errBlob);
        if (FAILED(hr)) {
            if (errBlob) {
                LogError("[WGC] Retained-copy VS compile failed: %s",
                         static_cast<const char*>(errBlob->GetBufferPointer()));
                errBlob->Release();
            }
            FreeLibrary(d3dCompiler);
            return false;
        }
        SafeRelease(errBlob);

        hr = d3dCompile(WGC_POOL_COPY_SHADER_SRC, strlen(WGC_POOL_COPY_SHADER_SRC), nullptr, nullptr, nullptr,
                        "PS_Main", "ps_4_0", 0, 0, &psBlob, &errBlob);
        if (FAILED(hr)) {
            if (errBlob) {
                LogError("[WGC] Retained-copy PS compile failed: %s",
                         static_cast<const char*>(errBlob->GetBufferPointer()));
                errBlob->Release();
            }
            SafeRelease(vsBlob);
            FreeLibrary(d3dCompiler);
            return false;
        }
        SafeRelease(errBlob);

        hr = d3dDevice_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &poolCopyVS_);
        SafeRelease(vsBlob);
        if (FAILED(hr)) {
            LogError("[WGC] Retained-copy CreateVertexShader failed: 0x%08lX", (unsigned long)hr);
            SafeRelease(psBlob);
            FreeLibrary(d3dCompiler);
            return false;
        }

        hr = d3dDevice_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &poolCopyPS_);
        SafeRelease(psBlob);
        if (FAILED(hr)) {
            LogError("[WGC] Retained-copy CreatePixelShader failed: 0x%08lX", (unsigned long)hr);
            SafeRelease(poolCopyVS_);
            FreeLibrary(d3dCompiler);
            return false;
        }
        FreeLibrary(d3dCompiler);

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        hr = d3dDevice_->CreateSamplerState(&samplerDesc, &poolCopySampler_);
        if (FAILED(hr)) {
            LogError("[WGC] Retained-copy CreateSamplerState failed: 0x%08lX", (unsigned long)hr);
            SafeRelease(poolCopyPS_);
            SafeRelease(poolCopyVS_);
            return false;
        }

        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = 16;
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = d3dDevice_->CreateBuffer(&cbDesc, nullptr, &poolCopyCB_);
        if (FAILED(hr)) {
            LogError("[WGC] Retained-copy CreateBuffer failed: 0x%08lX", (unsigned long)hr);
            SafeRelease(poolCopySampler_);
            SafeRelease(poolCopyPS_);
            SafeRelease(poolCopyVS_);
            return false;
        }

        LogInfo("[WGC] Retained-copy conversion shader created");
        return true;

}

#endif
