#include <gtest/gtest.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <mmsystem.h>

#include <cstring>
#include <string>
#include <vector>

#include "common/platform/raii_helpers.h"
#include "mediaengine/video/cursor_renderer.h"
#include "mediaengine/video/face_camera_renderer.h"
#include "mediaengine/video/face_camera_shader.h"
#include "mediaengine/video/packed_p010_shader.h"
#include "mediaengine/video/packed_p010_unpacker.h"
#include "mediaengine/video/precompiled_shaders.h"
#include "mediaengine/video/retained_yuv_converter.h"
#include "mediaengine/video/runtime_shader_compiler.h"
#include "mediaengine/video/video_color_conversion_shader.h"
#include "mediaengine/video/yuv_overlay_patch.h"

// Recording start must not run the HLSL compiler (precompiled_shaders.h): the
// build compiles every media engine shader request, and the runtime finds it.

namespace {

namespace ps = ce::precompiled_shaders;

std::vector<uint8_t> CompileNow(const std::string& source, const char* entry, const char* target, UINT flags) {
    ce::ComGuard<ID3DBlob> blob;
    ce::ComGuard<ID3DBlob> errors;
    std::vector<uint8_t> bytes;
    if (SUCCEEDED(D3DCompile(source.c_str(), source.size(), nullptr, nullptr, nullptr, entry, target, flags, 0,
                             blob.addressof(), errors.addressof())) &&
        blob) {
        const auto* data = static_cast<const uint8_t*>(blob->GetBufferPointer());
        bytes.assign(data, data + blob->GetBufferSize());
    }
    return bytes;
}

}  // namespace

TEST(PrecompiledShaderTest, RequestKeyIsTheBuildToolsKey) {
    // tools/tests/test_build_shaders.py pins the same value for build_shaders.request_key.
    EXPECT_EQ(ps::RequestKey("", "", "", 0), 0x778b1a14b6876aa7ull);
    EXPECT_NE(ps::RequestKey("a", "VS_Main", "vs_4_0", 0), ps::RequestKey("a", "VS_Main", "vs_4_0", 1u << 15));
}

TEST(PrecompiledShaderTest, EmbeddedBytecodeIsWhatTheCurrentSourceCompilesTo) {
    // A source edited without a fresh table would miss (and compile at runtime);
    // a hit must be byte-identical to compiling that source now.
    const std::string conversion = ce::video_color::kRgbColorConversionShaderSource;
    const std::string packed = conversion + ce::video_color::kPackedP010ShaderSource;
    struct Request {
        std::string source;
        const char* entry;
        const char* target;
        UINT flags;
    };
    const Request requests[] = {
        {conversion, "PS_P010Y", "ps_4_0", 0},
        {conversion, "PS_YuvUV", "ps_4_0", 0},
        {packed, "PS_PackHdrP010", "ps_4_0", 0},
        {packed, "PS_UnpackP010UV", "ps_4_0", 0},
        {ce::face_camera::kShaderSource, "PS_Main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3},
    };
    for (const Request& request : requests) {
        const ps::Bytecode embedded = ps::Find(request.source, request.entry, request.target, request.flags);
        ASSERT_TRUE(embedded) << request.entry << ": the build did not precompile this request";
        const std::vector<uint8_t> now = CompileNow(request.source, request.entry, request.target, request.flags);
        ASSERT_FALSE(now.empty()) << request.entry;
        EXPECT_EQ(std::vector<uint8_t>(embedded.data, embedded.data + embedded.size), now) << request.entry;
    }
    EXPECT_FALSE(ps::Find(conversion + "\n", "PS_P010Y", "ps_4_0", 0));  // any edit misses
    EXPECT_FALSE(ps::Find(conversion, "PS_P010Y", "ps_5_0", 0));
}

TEST(PrecompiledShaderTest, EveryRecordingShaderPreparationUsesBuildBytecode) {
    ce::ComGuard<ID3D11Device> device;
    ce::ComGuard<ID3D11DeviceContext> context;
    ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                            device.addressof(), nullptr, context.addressof())));
    // The media engine's preparations, as recording start runs them.
    YuvOverlayPatcher patcher;
    ASSERT_TRUE(patcher.PrepareShaders());
    ASSERT_TRUE(PackedP010Unpacker::PrepareShaders());
    RetainedYuvConverter converter;
    ASSERT_TRUE(converter.EnsureKernels(device.get()));
    CursorRenderer cursor;
    ASSERT_TRUE(cursor.Init(device.get(), context.get()));
    FaceCameraRenderer camera(ce::face_camera::Config{});
    ASSERT_TRUE(camera.InitRenderer(device.get(), context.get()));
    // VideoEncoder::EnsureSwapRBShader's request (a private member of a live encoder).
    const ce::video_color::RuntimeShaderEntry encoderEntries[] = {
        {"VS_Main", "vs_4_0"},   {"PS_Main", "ps_4_0"}, {"PS_P010Y", "ps_4_0"},
        {"PS_P010UV", "ps_4_0"}, {"PS_YuvY", "ps_4_0"}, {"PS_YuvUV", "ps_4_0"}};
    ASSERT_TRUE(ce::video_color::PrepareShaderBytecode(
        ce::video_color::kRgbColorConversionShaderSource, std::strlen(ce::video_color::kRgbColorConversionShaderSource),
        encoderEntries, 6, "RGBConvert", [](const ce::video_color::ShaderBytecodeView*) { return true; }));

    // Process-wide: any preparation in this test binary that missed counts.
    EXPECT_EQ(ce::video_color::RuntimeCompiledShaderCount(), 0u);
    EXPECT_GE(ps::EntryCount(), 25u);
}
