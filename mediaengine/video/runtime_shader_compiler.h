#pragma once

#include <d3dcommon.h>
#include <cstddef>
#include <functional>

namespace ce::video_color {

struct RuntimeShaderEntry {
    const char* entry = nullptr;
    const char* target = nullptr;  // vs_5_0 / ps_5_0 ...
};

// Compiles `count` entry points of one HLSL source with d3dcompiler_47 and
// hands the bytecode blobs (same order as `entries`) to `create`, which builds
// the device objects. The blobs are released before the compiler module is
// unloaded: ID3DBlob's Release lives in d3dcompiler_47.dll (wgc-capture.md).
bool CompileRuntimeShaders(const char* source, size_t sourceLength, const RuntimeShaderEntry* entries, size_t count,
                           const char* logTag, const std::function<bool(ID3DBlob* const* blobs)>& create);

}  // namespace ce::video_color
