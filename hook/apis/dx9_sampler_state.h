#pragma once

#include <d3d9.h>

namespace ce::dx9_sampler_state {

using SetSamplerStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
using GetSamplerStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD*);
using SetTextureFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);

void RegisterDevice(IDirect3DDevice9* device, bool newDevice);
HRESULT SetSamplerState(IDirect3DDevice9* device, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value,
                        SetSamplerStateFn setState, GetSamplerStateFn getState);
HRESULT GetSamplerState(IDirect3DDevice9* device, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD* value,
                        GetSamplerStateFn getState, SetSamplerStateFn setState);
HRESULT SetTexture(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture, SetTextureFn setTexture,
                   SetSamplerStateFn setState, GetSamplerStateFn getState);
void RefreshConfiguration(IDirect3DDevice9* device, SetSamplerStateFn setState, GetSamplerStateFn getState);
void ReconcileAfterExternalStateChange(IDirect3DDevice9* device, SetSamplerStateFn setState,
                                       GetSamplerStateFn getState);
void InvalidateDevice(IDirect3DDevice9* device);
void ResetDevice(IDirect3DDevice9* device);
void LogSummary();

}  // namespace ce::dx9_sampler_state
