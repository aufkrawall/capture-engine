#pragma once

#include <windows.h>

namespace ce::legacy_d3d_sampler_state {

enum class Api {
    D3D6,
    D3D7,
    D3D8,
};

using SetTextureStageStateFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, DWORD, DWORD);
using GetTextureStageStateFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, DWORD, DWORD*);
using QueryMaxAnisotropyFn = UINT (*)(void*);

void RegisterDevice(Api api, void* device, bool newDevice, QueryMaxAnisotropyFn queryMaxAnisotropy);
HRESULT SetTextureStageState(Api api, void* device, DWORD stage, DWORD type, DWORD value,
                             SetTextureStageStateFn setState, GetTextureStageStateFn getState,
                             QueryMaxAnisotropyFn queryMaxAnisotropy);
HRESULT GetTextureStageState(Api api, void* device, DWORD stage, DWORD type, DWORD* value,
                             GetTextureStageStateFn getState, SetTextureStageStateFn setState,
                             QueryMaxAnisotropyFn queryMaxAnisotropy);
void RefreshConfiguration(Api api, void* device, SetTextureStageStateFn setState, GetTextureStageStateFn getState,
                          QueryMaxAnisotropyFn queryMaxAnisotropy);
// After a successful ApplyStateBlock(blockHandle). A block CE saw created or
// recorded is merged from its snapshot; any other block re-reads the device.
// Writes nothing while no override is active (legacy_d3d_state_block_policy.h).
void ReconcileAfterExternalStateChange(Api api, void* device, DWORD blockHandle, SetTextureStageStateFn setState,
                                       GetTextureStageStateFn getState, QueryMaxAnisotropyFn queryMaxAnisotropy);
// State-block bookkeeping (legacy_d3d_sampler_state_blocks.cpp). Called after the
// runtime call; `handle` is the block the runtime returned or was given.
void OnBeginStateBlock(Api api, void* device);
void OnEndStateBlock(Api api, void* device, bool succeeded, DWORD handle);
void OnCreateStateBlock(Api api, void* device, DWORD type, DWORD handle);
void OnCaptureStateBlock(Api api, void* device, DWORD handle);
void ForgetStateBlock(Api api, void* device, DWORD handle);
void ResetDevice(Api api, void* device);
void LogSummary(Api api);

}  // namespace ce::legacy_d3d_sampler_state
