#include "runtime_shader_compiler.h"

#include <windows.h>

#include <vector>

#include "common/platform/raii_helpers.h"
#include "common/platform/secure_dll_loading.h"
#include "mediaengine/engine/mediaengine.h"

namespace ce::video_color {

bool CompileRuntimeShaders(const char* source, size_t sourceLength, const RuntimeShaderEntry* entries, size_t count,
                           const char* logTag, const std::function<bool(ID3DBlob* const* blobs)>& create) {
    if (!source || !entries || count == 0 || !create)
        return false;
    // Declared first, destroyed last: every blob below is released before the
    // module that implements ID3DBlob unloads.
    ce::ModuleGuard d3dCompiler(ce::security::LoadSystemLibrary(L"d3dcompiler_47.dll"));
    if (!d3dCompiler) {
        DLL_Log("[%s] Failed to load d3dcompiler_47.dll", logTag);
        return false;
    }
    typedef HRESULT(WINAPI * pD3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                          LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    const auto d3dCompile = reinterpret_cast<pD3DCompile>(GetProcAddress(d3dCompiler.get(), "D3DCompile"));
    if (!d3dCompile) {
        DLL_Log("[%s] Failed to get D3DCompile", logTag);
        return false;
    }

    std::vector<ce::ComGuard<ID3DBlob>> blobs(count);
    std::vector<ID3DBlob*> raw(count, nullptr);
    for (size_t i = 0; i < count; ++i) {
        ce::ComGuard<ID3DBlob> errors;
        const HRESULT hr = d3dCompile(source, sourceLength, nullptr, nullptr, nullptr, entries[i].entry,
                                      entries[i].target, 0, 0, blobs[i].addressof(), errors.addressof());
        if (errors) {
            DLL_Log("[%s] %s/%s compiler output: %s", logTag, entries[i].entry, entries[i].target,
                    static_cast<const char*>(errors->GetBufferPointer()));
        }
        if (FAILED(hr) || !blobs[i]) {
            DLL_Log("[%s] Runtime shader compilation failed: %s HR=%x", logTag, entries[i].entry, hr);
            return false;
        }
        raw[i] = blobs[i].get();
    }
    return create(raw.data());
}

}  // namespace ce::video_color
