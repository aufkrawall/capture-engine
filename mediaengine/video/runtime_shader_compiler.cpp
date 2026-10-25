#include "runtime_shader_compiler.h"

#include <windows.h>
#include <d3dcommon.h>

#include <atomic>
#include <string_view>
#include <vector>

#include "common/platform/raii_helpers.h"
#include "common/platform/secure_dll_loading.h"
#include "mediaengine/engine/mediaengine.h"
#include "precompiled_shaders.h"

namespace ce::video_color {

namespace {

std::atomic<uint64_t> g_runtimeCompiled{0};

}  // namespace

uint64_t RuntimeCompiledShaderCount() {
    return g_runtimeCompiled.load(std::memory_order_relaxed);
}

bool PrepareShaderBytecode(const char* source, size_t sourceLength, const RuntimeShaderEntry* entries, size_t count,
                           const char* logTag, const std::function<bool(const ShaderBytecodeView* shaders)>& create) {
    if (!source || !entries || count == 0 || !create)
        return false;
    const ULONGLONG start = GetTickCount64();
    const std::string_view text(source, sourceLength);
    std::vector<ShaderBytecodeView> views(count);
    size_t missing = 0;
    const char* firstMissing = nullptr;
    for (size_t i = 0; i < count; ++i) {
        const auto bytecode = ce::precompiled_shaders::Find(text, entries[i].entry, entries[i].target, entries[i].flags);
        if (bytecode) {
            views[i] = {bytecode.data, bytecode.size};
        } else if (missing++ == 0) {
            firstMissing = entries[i].entry;
        }
    }

    // Declared first, destroyed last: every blob below is released before the
    // module that implements ID3DBlob unloads.
    ce::ModuleGuard d3dCompiler;
    std::vector<ce::ComGuard<ID3DBlob>> blobs(missing > 0 ? count : 0);
    if (missing > 0) {
        d3dCompiler.reset(ce::security::LoadSystemLibrary(L"d3dcompiler_47.dll"));
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
        for (size_t i = 0; i < count; ++i) {
            if (views[i].data)
                continue;
            ce::ComGuard<ID3DBlob> errors;
            const HRESULT hr = d3dCompile(source, sourceLength, nullptr, nullptr, nullptr, entries[i].entry,
                                          entries[i].target, entries[i].flags, 0, blobs[i].addressof(),
                                          errors.addressof());
            if (errors) {
                DLL_Log("[%s] %s/%s compiler output: %s", logTag, entries[i].entry, entries[i].target,
                        static_cast<const char*>(errors->GetBufferPointer()));
            }
            if (FAILED(hr) || !blobs[i]) {
                DLL_Log("[%s] Runtime shader compilation failed: %s HR=%x", logTag, entries[i].entry, hr);
                return false;
            }
            views[i] = {blobs[i]->GetBufferPointer(), blobs[i]->GetBufferSize()};
        }
        g_runtimeCompiled.fetch_add(missing, std::memory_order_relaxed);
    }
    // Once per preparation (per module, or per device for device-bound kernels), never per frame.
    DLL_Log("[%s] Shader bytecode: precompiled=%zu runtimeCompiled=%zu%s%s elapsed=%llums (table=%zu)", logTag,
            count - missing, missing, firstMissing ? " first=" : "", firstMissing ? firstMissing : "",
            static_cast<unsigned long long>(GetTickCount64() - start), ce::precompiled_shaders::EntryCount());
    return create(views.data());
}

}  // namespace ce::video_color
