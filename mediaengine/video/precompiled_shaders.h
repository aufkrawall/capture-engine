#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Shader bytecode the build compiled from the media engine's HLSL
// (tools/build/build_shaders.py -> <obj>/generated/ce_precompiled_shaders.h),
// so recording start never runs the HLSL compiler: compiling the overlay
// patch alone took ~0.65 s of CPU at the first recording of every app start.
//
// A request is identified by its exact inputs: FNV-1a 64 over the source text,
// the entry point, the target and the D3DCOMPILE flags. A source edited without
// a rebuild, a request the build manifest lacks, or a build host without
// d3dcompiler_47 (Linux) finds nothing, and the caller compiles at runtime as
// before (PrepareShaderBytecode logs it). PrecompiledShaderTest asserts that a
// Windows build answers every recording request.

namespace ce::precompiled_shaders {

struct Bytecode {
    const uint8_t* data = nullptr;
    size_t size = 0;
    explicit operator bool() const { return data && size; }
};

uint64_t RequestKey(std::string_view source, const char* entry, const char* target, uint32_t flags);
Bytecode Find(std::string_view source, const char* entry, const char* target, uint32_t flags);
size_t EntryCount();

}  // namespace ce::precompiled_shaders
