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
void ReconcileAfterExternalStateChange(Api api, void* device, SetTextureStageStateFn setState,
                                       GetTextureStageStateFn getState, QueryMaxAnisotropyFn queryMaxAnisotropy);
void ResetDevice(Api api, void* device);
void LogSummary(Api api);

}  // namespace ce::legacy_d3d_sampler_state
