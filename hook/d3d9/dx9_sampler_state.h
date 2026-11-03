#pragma once

#include <d3d9.h>

namespace ce::dx9_sampler_state {

using SetSamplerStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
using GetSamplerStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD*);
using SetTextureFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);

// `intercepted`: CE sees every SetTexture/SetSamplerState on the device (its
// per-draw slots are armed, dx9_sampler_interception_policy.h).
void RegisterDevice(IDirect3DDevice9* device, bool newDevice, bool intercepted);
HRESULT SetSamplerState(IDirect3DDevice9* device, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value,
                        SetSamplerStateFn setState, GetSamplerStateFn getState);
HRESULT GetSamplerState(IDirect3DDevice9* device, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD* value,
                        GetSamplerStateFn getState, SetSamplerStateFn setState);
HRESULT SetTexture(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture, SetTextureFn setTexture,
                   SetSamplerStateFn setState, GetSamplerStateFn getState);
void RefreshConfiguration(IDirect3DDevice9* device, SetSamplerStateFn setState, GetSamplerStateFn getState);
// After IDirect3DStateBlock9::Apply of `stateBlock` (see
// dx9_state_block_sampler_policy.h): merges the block's snapshot into the
// shadow and reconciles the samplers it covers, or re-reads the device for a
// block CE never saw created.
void ReconcileAfterExternalStateChange(IDirect3DDevice9* device, const void* stateBlock, SetSamplerStateFn setState,
                                       GetSamplerStateFn getState);
// State-block lifecycle, reported by the device and state-block detours after
// the original call succeeded (OnEndStateBlock also on failure, with nullptr).
// `snapshotTrusted`: the recording sees all of its Set* calls.
void OnBeginStateBlock(IDirect3DDevice9* device, bool snapshotTrusted);
void OnEndStateBlock(IDirect3DDevice9* device, const void* stateBlock);
void OnCreateStateBlock(IDirect3DDevice9* device, const void* stateBlock, D3DSTATEBLOCKTYPE type);
void OnCaptureStateBlock(IDirect3DDevice9* device, const void* stateBlock);
void ForgetStateBlock(IDirect3DDevice9* device, const void* stateBlock);
void InvalidateDevice(IDirect3DDevice9* device);
void ResetDevice(IDirect3DDevice9* device, bool intercepted);
void LogSummary();
bool ShouldInterceptFast();
// A sampler override (AF, mip mode or LOD bias) is configured right now.
bool OverrideConfigured();

}  // namespace ce::dx9_sampler_state
