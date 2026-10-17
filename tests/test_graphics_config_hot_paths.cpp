#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "source_fragment_reader.h"

// GetActiveGraphicsConfig() takes a process-wide mutex and returns the whole GraphicsConfig - dozens
// of std::string members - by value. Paths a game drives every frame, or per API call, read the
// thread-cached copy instead (GetActiveGraphicsConfigCached), which refreshes through the same
// function on every config version change, so the published override atomics keep their timing.

namespace {

std::string ReadHookSource(const char* fileName) {
    const std::string source = ce::test_source::ReadLogicalSource(ce::test_source::FindSource("hook", fileName));
    EXPECT_FALSE(source.empty()) << fileName;
    return source;
}

// The body of the first definition of `signature` (a declaration ends in ';' before any '{'),
// up to its column-0 closing brace.
std::string DefinitionBody(const std::string& source, const char* signature) {
    for (size_t begin = source.find(signature); begin != std::string::npos;
         begin = source.find(signature, begin + 1)) {
        const size_t brace = source.find('{', begin);
        const size_t semicolon = source.find(';', begin);
        if (brace == std::string::npos || (semicolon != std::string::npos && semicolon < brace))
            continue;
        const size_t end = source.find("\n}", brace);
        if (end == std::string::npos)
            break;
        return source.substr(begin, end - begin);
    }
    ADD_FAILURE() << "no definition of " << signature;
    return {};
}

constexpr const char* kByValueAccessor = "GetActiveGraphicsConfig()";
constexpr const char* kCachedAccessor = "GetActiveGraphicsConfigCached()";

TEST(GraphicsConfigHotPathTest, PerFrameFunctionsReadTheThreadCachedConfig) {
    struct HotFunction {
        const char* file;
        const char* signature;
    };
    const std::vector<HotFunction> hot = {
        // Every DXGI/DX12/DX9 present (prerender wait) and every vsync-override decision.
        {"hook_common_graphics_config.cpp", "float GetActivePrerenderLimit()"},
        {"hook_common_vsync.cpp", "VSyncOverride GetVSyncOverride()"},
        {"dx9_hook_present.cpp", "void DX9_PresentBegin("},
        {"dx11_hook_present.cpp", "void ApplyDeferredSamplerOverrides11("},
        {"dxgi_swapchain_wrap_modern.cpp", "CWrapDXGISwapChain::SetMaximumFrameLatency("},
        {"d3d9_device_wrap.cpp", "CWrapD3D9Device::SetMaximumFrameLatency("},
        {"opengl_hook_capture.cpp", "DetourWglSwapIntervalEXT("},
        // Per static sampler of every root signature the wrapper parses.
        {"root_signature_parser.cpp", "bool ApplyStaticSamplerOverrides("},
        // NGX evaluation and Streamline per-frame calls.
        {"nvngx_hook_lifecycle.cpp", "void PublishEvaluatedFeature("},
        {"nvngx_hook_lifecycle.cpp", "NVSDK_NGX_Result ProcessEvaluateFeature("},
        {"remix_hook.cpp", "uint32_t ConfiguredGeneratedFrames()"},
        {"streamline_hook_api.cpp", "slResult Hooked_slEvaluateFeature("},
        {"streamline_hook_dlssg.cpp", "slResult Hooked_slDLSSGSetOptions("},
        {"streamline_hook_state.cpp", "void UpdateViewportRuntimeState("},
    };
    for (const HotFunction& function : hot) {
        const std::string body = DefinitionBody(ReadHookSource(function.file), function.signature);
        EXPECT_EQ(body.find(kByValueAccessor), std::string::npos) << function.file << ": " << function.signature;
    }
    EXPECT_NE(DefinitionBody(ReadHookSource("hook_common_graphics_config.cpp"), "float GetActivePrerenderLimit()")
                  .find(kCachedAccessor),
              std::string::npos);
    EXPECT_NE(DefinitionBody(ReadHookSource("hook_common_vsync.cpp"), "VSyncOverride GetVSyncOverride()")
                  .find(kCachedAccessor),
              std::string::npos);
}

TEST(GraphicsConfigHotPathTest, PerCallUnitsNeverCopyTheWholeConfig) {
    // NGX parameter Set/Get hooks run per parameter, the DXGI flip-queue pacing and the DDraw
    // legacy-overlay route per present: no by-value accessor anywhere in these units.
    for (const char* file : {"nvngx_hook_params.cpp", "nvngx_hook_params_fg_factor.cpp",
                             "dxgi_shared_present_pacing.cpp", "dxgi_swapchain_wrap_frame_latency.cpp",
                             "ddraw_hook_overlay_route.cpp"}) {
        const std::string source = ReadHookSource(file);
        EXPECT_EQ(source.find(kByValueAccessor), std::string::npos) << file;
        EXPECT_NE(source.find(kCachedAccessor), std::string::npos) << file;
    }
}

TEST(GraphicsConfigHotPathTest, DX9PresentKeepsNoPerFrameConfigCopy) {
    // dx9_hook_g_FrameConfig was assigned a full copy every DX9 present and never read.
    for (const char* file : {"dx9_hook_present.cpp", "dx9_hook_internal.h"}) {
        EXPECT_EQ(ReadHookSource(file).find("dx9_hook_g_FrameConfig"), std::string::npos) << file;
    }
}

TEST(GraphicsConfigHotPathTest, RefreshAndExplicitPublishersStillResolveTheFullConfig) {
    // The thread cache refreshes through the by-value function, which also publishes
    // g_GraphicsOverridesActive, g_D3D11SamplerOverridesActive and the DLSS DRS answers;
    // explicit publishers must keep calling it rather than a thread cache that may be current.
    const std::string config = ReadHookSource("hook_common_graphics_config.cpp");
    const std::string refresh = DefinitionBody(config, "ThreadConfigCache& RefreshedThreadConfig()");
    EXPECT_NE(refresh.find("cache.config = GetActiveGraphicsConfig();"), std::string::npos);
    const std::string resolve = DefinitionBody(config, "GraphicsConfig GetActiveGraphicsConfig()");
    EXPECT_NE(resolve.find("g_GraphicsOverridesActive.store("), std::string::npos);
    EXPECT_NE(resolve.find("ce::ngx_drs::SetConfiguredOverrides("), std::string::npos);

    EXPECT_NE(DefinitionBody(ReadHookSource("main_overlay_detect.cpp"), "void ArmNgxDrsOverridesIfConfigured(")
                  .find("GetActiveGraphicsConfig();"),
              std::string::npos);
    EXPECT_NE(DefinitionBody(ReadHookSource("dx11_hook_af_report.cpp"), "void CEFlow_RefreshGraphicsConfig()")
                  .find("GetActiveGraphicsConfig();"),
              std::string::npos);
}

}  // namespace
