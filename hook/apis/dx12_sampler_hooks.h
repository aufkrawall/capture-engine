#pragma once

#include <d3d12.h>

using D3D12CreateDeviceRawPtr = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using D3D12GetInterfacePtr = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
using D3D12SerializeRootSignaturePtr = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
                                                        ID3DBlob**, ID3DBlob**);
using D3D12SerializeVersionedRootSignaturePtr = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*,
                                                                 ID3DBlob**, ID3DBlob**);

extern D3D12CreateDeviceRawPtr oD3D12CreateDeviceRaw;
extern D3D12GetInterfacePtr oD3D12GetInterface;
extern D3D12SerializeRootSignaturePtr oSerializeRootSignature;
extern D3D12SerializeVersionedRootSignaturePtr oSerializeVersionedRootSignature;

HRESULT WINAPI DetourD3D12CreateDeviceRaw(IUnknown* adapter, D3D_FEATURE_LEVEL minimumFeatureLevel, REFIID riid,
                                          void** device);
HRESULT WINAPI DetourD3D12GetInterface(REFCLSID clsid, REFIID riid, void** object);
HRESULT WINAPI DetourSerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* rootSignature,
                                             D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob** blob,
                                             ID3DBlob** errorBlob);
HRESULT WINAPI DetourSerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* rootSignature,
                                                      ID3DBlob** blob, ID3DBlob** errorBlob);

extern "C" BOOL WINAPI ApplyDX12SamplerOverridesCallback(D3D12_SAMPLER_DESC* desc);

namespace ce::dx12_sampler_hooks {

bool HookDevice(ID3D12Device* device);
void LogSummary(const char* reason);

}  // namespace ce::dx12_sampler_hooks
