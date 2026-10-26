#pragma once

#include <windows.h>

// Which image implements a COM object. System32's D3D10/D3D11 runtimes read private DXGI state
// through the adapter they are given: an adapter from a translation layer (DXVK's dxgi.dll)
// crashed GTA V Enhanced inside d3d11!CCreateDeviceCache::CAdapterCache::GetWddmVersion
// (sessions 20261026_165613 and 20261026_175321). The object's vtable lives in the image that
// implements it; wrappers and translation layers bring their own.
namespace ce::com_object_module {

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

}  // namespace ce::com_object_module
