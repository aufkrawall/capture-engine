#include "wgc_capture_internal.h"


D3D11ContextStateGuard::D3D11ContextStateGuard(ID3D11DeviceContext* context) : context_(context) {


        if (!context_) {
            return;
        }
        context_->AddRef();
        context_->OMGetRenderTargets(1, &rtv_, &dsv_);
        viewportCount_ = 1;
        context_->RSGetViewports(&viewportCount_, &viewport_);
        context_->VSGetShader(&vs_, nullptr, nullptr);
        context_->PSGetShader(&ps_, nullptr, nullptr);
        context_->PSGetShaderResources(0, 1, &srv_);
        context_->PSGetSamplers(0, 1, &sampler_);
        context_->PSGetConstantBuffers(0, 1, &constantBuffer_);
        context_->IAGetPrimitiveTopology(&topology_);
        context_->IAGetInputLayout(&inputLayout_);

}

D3D11ContextStateGuard::~D3D11ContextStateGuard() {


        if (!context_) {
            return;
        }
        context_->OMSetRenderTargets(1, &rtv_, dsv_);
        if (viewportCount_ > 0) {
            context_->RSSetViewports(viewportCount_, &viewport_);
        }
        context_->VSSetShader(vs_, nullptr, 0);
        context_->PSSetShader(ps_, nullptr, 0);
        context_->PSSetShaderResources(0, 1, &srv_);
        context_->PSSetSamplers(0, 1, &sampler_);
        context_->PSSetConstantBuffers(0, 1, &constantBuffer_);
        context_->IASetPrimitiveTopology(topology_);
        context_->IASetInputLayout(inputLayout_);

        SafeRelease(inputLayout_);
        SafeRelease(constantBuffer_);
        SafeRelease(sampler_);
        SafeRelease(srv_);
        SafeRelease(ps_);
        SafeRelease(vs_);
        SafeRelease(dsv_);
        SafeRelease(rtv_);
        SafeRelease(context_);

}

WgcCallbackThreadQoS::~WgcCallbackThreadQoS() {


        if (mmcssHandle_) {
            if (!AvRevertMmThreadCharacteristics(mmcssHandle_)) {
                LogWarn("[WGC] Failed to revert callback MMCSS registration (tid=%lu, err=%lu)", GetCurrentThreadId(),
                        GetLastError());
            }
        }

}
