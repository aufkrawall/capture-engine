#pragma once

#include <windows.h>

// Which runtime a COM object belongs to. The Vulkan layer's capture interop creates a D3D11 device
// with System32's d3d11.dll, which reads private DXGI state through the adapter it is given: an
// adapter from a translation layer (DXVK's dxgi.dll) crashed GTA V Enhanced inside
// d3d11!CCreateDeviceCache::CAdapterCache::GetWddmVersion (session 20261026_165613). The object's
// vtable lives in the image that implements it; wrappers and translation layers bring their own.
namespace ce::vulkan_native_interop {

inline HMODULE ImplementingModule(const void* comObject) {
    if (!comObject) {
        return nullptr;
    }
    const void* vtable = *static_cast<const void* const*>(comObject);
    HMODULE owner = nullptr;
    if (!vtable ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(vtable), &owner)) {
        return nullptr;
    }
    return owner;
}

inline bool IsImplementedBy(const void* comObject, HMODULE module) {
    return module && ImplementingModule(comObject) == module;
}

}  // namespace ce::vulkan_native_interop
