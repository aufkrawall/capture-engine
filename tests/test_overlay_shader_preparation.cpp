#include <gtest/gtest.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <mmsystem.h>

#include <string>
#include <vector>

#include "common/platform/raii_helpers.h"
#include "mediaengine/engine/mediaengine_handles.h"
#include "mediaengine/video/cursor_renderer.h"
#include "mediaengine/video/yuv_overlay_patch.h"

namespace {

struct ShaderLog {
    std::vector<std::string> lines;
    static void Append(void* context, const char* line) {
        static_cast<ShaderLog*>(context)->lines.emplace_back(line);
    }
    bool HasCompilation() const {
        for (const auto& line : lines) {
            if (line.find("compiler output") != std::string::npos ||
                line.find("Shader preparation") != std::string::npos)
                return true;
        }
        return false;
    }
};

struct OverlayShaderPreparationTest : ::testing::Test {
    ce::ComGuard<ID3D11Device> device;
    ce::ComGuard<ID3D11DeviceContext> context;
    void SetUp() override {
        ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, device.addressof(), nullptr,
                                               context.addressof())));
    }
};

}  // namespace

TEST_F(OverlayShaderPreparationTest, PatchInitRequiresExplicitPreparationBeforeAnyVideoFrame) {
    ShaderLog log;
    MediaEngineLogRouteScope route({&ShaderLog::Append, &log});
    YuvOverlayPatcher patcher;
    EXPECT_FALSE(patcher.Init(device.get(), context.get()));
    EXPECT_FALSE(log.HasCompilation());
    ASSERT_TRUE(patcher.PrepareShaders());
    log.lines.clear();
    ASSERT_TRUE(patcher.Init(device.get(), context.get()));
    EXPECT_FALSE(log.HasCompilation());
    // Device teardown/recreation and repeated setup keep bytecode outside the frame path.
    patcher.Release();
    ASSERT_TRUE(patcher.Init(device.get(), context.get()));
    ASSERT_TRUE(patcher.PrepareShaders());
    EXPECT_FALSE(log.HasCompilation());
}

TEST_F(OverlayShaderPreparationTest, PreparedPatchBytecodeCanBeReusedByAnotherRecording) {
    YuvOverlayPatcher first;
    ASSERT_TRUE(first.PrepareShaders());
    first.Release();
    ShaderLog log;
    MediaEngineLogRouteScope route({&ShaderLog::Append, &log});
    YuvOverlayPatcher next;
    ASSERT_TRUE(next.PrepareShaders());
    ASSERT_TRUE(next.Init(device.get(), context.get()));
    EXPECT_FALSE(log.HasCompilation());
}

TEST_F(OverlayShaderPreparationTest, CursorDeviceResourcesNeverCompileAfterConfiguration) {
    CursorRenderer renderer;
    ShaderLog log;
    MediaEngineLogRouteScope route({&ShaderLog::Append, &log});
    ASSERT_TRUE(renderer.Init(device.get(), context.get()));
    renderer.Cleanup();
    ASSERT_TRUE(renderer.Init(device.get(), context.get()));
    EXPECT_FALSE(log.HasCompilation());
}
