#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace ce::video_color {

struct RuntimeShaderEntry {
    const char* entry = nullptr;
    const char* target = nullptr;  // vs_5_0 / ps_5_0 ...
    uint32_t flags = 0;            // D3DCOMPILE_* flags
};

struct ShaderBytecodeView {
    const void* data = nullptr;
    size_t size = 0;
};

// Bytecode of `count` entry points of one HLSL source, handed to `create` (same
// order as `entries`), which builds the device objects. Each entry is the
// build's precompiled bytecode (precompiled_shaders.h) when the build compiled
// exactly this request; otherwise it is compiled now with d3dcompiler_47 and
// the line `[<logTag>] Shader bytecode: ... runtimeCompiled=N` says which. The
// compiled blobs are released before the compiler module is unloaded:
// ID3DBlob's Release lives in d3dcompiler_47.dll (wgc-capture.md).
bool PrepareShaderBytecode(const char* source, size_t sourceLength, const RuntimeShaderEntry* entries, size_t count,
                           const char* logTag, const std::function<bool(const ShaderBytecodeView* shaders)>& create);

// Entries PrepareShaderBytecode compiled at runtime in this process, i.e. requests
// the build did not precompile. 0 on a Windows build (PrecompiledShaderTest).
uint64_t RuntimeCompiledShaderCount();

}  // namespace ce::video_color
