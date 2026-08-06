#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "source_fragment_reader.h"

namespace {

std::string ReadProjectFile(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

void ExpectContains(const std::string& text, const char* needle) {
    EXPECT_NE(text.find(needle), std::string::npos) << "missing policy marker: " << needle;
}

TEST(VulkanFgBuildPolicyTest, PinsSignedFidelityFx114SeparatelyFromDx12Sdk220) {
    const std::string build = ReadProjectFile("build.py");
    ExpectContains(build, "FidelityFX-Samples-v2.2.0-prebuilt.zip");
    ExpectContains(build, "FidelityFX-SDK-v1.1.4.zip");
    ExpectContains(build, "0216556bfb0e243cec30004a2a98d38f4e3f7406cb7938e3c1b85c758e95d952");
    ExpectContains(build, "fidelityfx_vk_v1_1_4");
    ExpectContains(build, "PrebuiltSignedDLL/amd_fidelityfx_vk.dll");
    ExpectContains(build, "pe_has_authenticode_certificate(ffx_vk_dll)");
    ExpectContains(build, "FidelityFX-SDK-v1.1.4-LICENSE.txt");
}

TEST(VulkanFgBuildPolicyTest, CompilesValidatesAndEmbedsAllRuntimeShaders) {
    const std::string build = ReadProjectFile("build.py");
    ExpectContains(build, "glslangValidator.exe");
    ExpectContains(build, "spirv-val.exe");
    ExpectContains(build, "vulkan_fg_fullscreen.vert");
    ExpectContains(build, "vulkan_fg_scene.frag");
    ExpectContains(build, "vulkan_fg_taa.frag");
    ExpectContains(build, "vulkan_fg_ui.frag");
    ExpectContains(build, "vulkan_fg_compose.frag");
    ExpectContains(build, "vulkan_fg_present.frag");
    ExpectContains(build, "vulkan_fg_shaders.h");
    ExpectContains(build, "Vulkan FG runtime shader sidecars are forbidden");
}

TEST(VulkanFgBuildPolicyTest, EnforcesX64PdbAndOptInRuntimeMatrix) {
    const std::string build = ReadProjectFile("build.py");
    ExpectContains(build, "Skipping vulkan_fg_switch_test.exe (x86)");
    ExpectContains(build, "vulkan_fg_switch_test.pdb was not produced");

    const std::string runner = ReadProjectFile("testapp/run_tests.py");
    ExpectContains(runner, "OPT_IN_APIS = [\"vulkan_fg\"]");
    ExpectContains(runner, "apis_to_test = list(DEFAULT_APIS)");
    ExpectContains(runner, "--api vulkan_fg is x64-only");
    ExpectContains(runner, "vulkan_fg_switch_test.exe");
    ExpectContains(runner, "if api in {\"vulkan\", \"vulkan_fg\"}:");
}

TEST(VulkanFgBuildPolicyTest, RuntimeKeepsOwnerRoutingAndDiagnosticsExplicit) {
    const std::string common = ReadProjectFile("testapp/vulkan_fg_switch_common.h");
    const std::string wsi = ReadProjectFile("testapp/vulkan_fg_switch_wsi.cpp");
    const std::string diagnostics = ReadProjectFile("testapp/vulkan_fg_switch_diagnostics.cpp");
    const std::string app = ReadProjectFile("testapp/vulkan_fg_switch_test.cpp");
    ExpectContains(common, "struct VulkanWsiDispatch");
    ExpectContains(common, "SwapchainOwner owner");
    ExpectContains(wsi, "IsOwnerDispatchPairValid");
    ExpectContains(wsi, "swapchain-commit owner=%s route=%s");
    ExpectContains(diagnostics, "[FG-TRANSITION]");
    ExpectContains(diagnostics, "VK_EXT_device_fault");
    ExpectContains(diagnostics, "FFX_API_RETURN_NO_PROVIDER");
    ExpectContains(wsi, "ShouldForwardOldSwapchain(old.owner, owner)");
    ExpectContains(wsi, "forwardOldSwapchain ? old.handle : VK_NULL_HANDLE");
    ExpectContains(wsi, "FidelityFX loader bridge create");
    ExpectContains(wsi, "bridgeInfo.oldSwapchain");
    ExpectContains(wsi, "FidelityFX same-owner proxy retired before recreation");
    ExpectContains(wsi, "FidelityFX proxy retired after drained passthrough for owner");
    const std::string fidelityfx = ReadProjectFile("testapp/vulkan_fg_switch_fidelityfx.cpp");
    const std::string fidelityfxFrame =
        ReadProjectFile("testapp/vulkan_fg_switch_fidelityfx_frame.cpp");
    const std::string allFidelityFx = fidelityfx + fidelityfxFrame;
    ExpectContains(allFidelityFx, "presentationAlreadyRetired");
    ExpectContains(allFidelityFx, "skipping configure/wait");
    ExpectContains(wsi, "replacement.wsi = DispatchForOwner(owner)");
    ExpectContains(wsi, "DestroyFailedFfxTakeover");
    ExpectContains(wsi, "cleaning failed FFX takeover with owning proxy handle");
    ExpectContains(app, "void RequestManualMode");
    ExpectContains(app, "Keep the title and rendered status text synchronized");
}

TEST(VulkanFgBuildPolicyTest, ProvisionsFidelityFxPromotedMemoryRequirementsEntryPoint) {
    const std::string device = ReadProjectFile("testapp/vulkan_fg_switch_device.cpp");
    const std::string fidelityfx = ReadProjectFile("testapp/vulkan_fg_switch_fidelityfx.cpp");
    ExpectContains(device, "VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME");
    ExpectContains(device, "VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME");
    ExpectContains(fidelityfx, "FidelityFxGetDeviceProcAddr");
    ExpectContains(fidelityfx, "vkGetBufferMemoryRequirements2KHR");
    ExpectContains(fidelityfx, "vkGetBufferMemoryRequirements2\"");
    ExpectContains(fidelityfx, "FidelityFX Vulkan proc preflight");
}

TEST(VulkanFgBuildPolicyTest, FidelityFxHudlessUsesMatchingEightBitPresentationTarget) {
    const std::string common = ReadProjectFile("testapp/vulkan_fg_switch_common.h");
    // The resource helpers hoisted into the shared internal header; read the
    // logical translation unit so the header content is included.
    const std::string resources = ReadProjectFile("testapp/vulkan_fg_switch_test.cpp");
    const std::string fidelityfx = ReadProjectFile("testapp/vulkan_fg_switch_fidelityfx.cpp");
    ExpectContains(common, "ImageResource presentationColor");
    ExpectContains(resources, "g_App.swapchain.format, sampledColor");
    ExpectContains(fidelityfx, "ffxApiGetSurfaceFormatVK(g_App.swapchain.format)");
    ExpectContains(fidelityfx, "resources.presentationColor");
    ExpectContains(fidelityfx, "FidelityFX HUDless format preflight");
    ExpectContains(fidelityfx, "FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT");
}

TEST(VulkanFgBuildPolicyTest, VisualShadersMirrorDx12SceneAndPixelHud) {
    const std::string scene = ReadProjectFile("testapp/shaders/vulkan_fg_scene.frag");
    const std::string ui = ReadProjectFile("testapp/shaders/vulkan_fg_ui.frag");
    ExpectContains(scene, "const vec3 kEye = vec3(0.0, 2.4, -5.5)");
    ExpectContains(scene, "vec3(0.95, 0.55, 0.22)");
    ExpectContains(scene, "floor(floorPosition.xz * 0.5)");
    ExpectContains(scene, "previousWorldHit");
    EXPECT_EQ(scene.find("sparkle"), std::string::npos);

    ExpectContains(ui, "FG: DLSS SUSPENDED");
    ExpectContains(ui, "FG: FSR SUSPENDED");
    ExpectContains(ui, "1 OFF  2 DLSS  3 FSR");
    ExpectContains(ui, "vec2(30.0, 30.0), 4.0");
    ExpectContains(ui, "vec2(30.0, 76.0), 3.0");
    ExpectContains(ui, "hudSweep = sin(pc.timeSeconds * 1.2)");
    ExpectContains(ui, "hudX + 232.0");
    EXPECT_EQ(ui.find("digitMask"), std::string::npos);
}

TEST(VulkanFgBuildPolicyTest, StreamlineTagsBackbufferUiAndExplicitExtents) {
    const std::string streamline = ReadProjectFile("testapp/vulkan_fg_switch_streamline.cpp");
    ExpectContains(streamline, "sl::kBufferTypeUIColorAndAlpha");
    ExpectContains(streamline, "sl::kBufferTypeBackbuffer");
    ExpectContains(streamline, "&renderExtent");
    ExpectContains(streamline, "&displayExtent");
    ExpectContains(streamline, "VK_IMAGE_LAYOUT_PRESENT_SRC_KHR");
}

TEST(VulkanFgBuildPolicyTest, ReflexKeepsAutomaticDriverPacingUnmodified) {
    const std::string streamline = ReadProjectFile("testapp/vulkan_fg_switch_streamline.cpp");
    const std::string streamlineShutdown =
        ReadProjectFile("testapp/vulkan_fg_switch_streamline_shutdown.cpp");
    const std::string renderer = ReadProjectFile("testapp/vulkan_fg_switch_renderer.cpp");
    const std::string allStreamline = streamline + streamlineShutdown;
    ExpectContains(allStreamline, "slReflexGetState");
    ExpectContains(allStreamline, "bIsVsyncSupportAvailable");
    ExpectContains(allStreamline, "frameLimitUs=0");
    ExpectContains(allStreamline, "automaticDriverPacing=unmodified");
    ExpectContains(streamline, "sl::DLSSGFlags::eRetainResourcesWhenOff");
    ExpectContains(streamline, "slow slDLSSGSetOptions");
    EXPECT_EQ(allStreamline.find("frameLimitUs ="), std::string::npos)
        << "the Vulkan test must never install an explicit Reflex frame cap";
    EXPECT_EQ(allStreamline.find("g_App.sl.reflexSleep && g_App.sl.reflexActive"), std::string::npos)
        << "slReflexSleep is required even while Reflex mode is off";
    EXPECT_LT(renderer.find("BeginStreamlineFrame()"), renderer.find("vkWaitForFences(frame)"));
}

TEST(VulkanFgBuildPolicyTest, AsyncPresentUsesValidQueueAndPerImageSemaphoreOwnership) {
    const std::string app = ReadProjectFile("testapp/vulkan_fg_switch_test.cpp");
    const std::string device = ReadProjectFile("testapp/vulkan_fg_switch_device.cpp");
    const std::string fidelityfx = ReadProjectFile("testapp/vulkan_fg_switch_fidelityfx.cpp");
    const std::string wsi = ReadProjectFile("testapp/vulkan_fg_switch_wsi.cpp");
    const std::string renderer = ReadProjectFile("testapp/vulkan_fg_switch_renderer.cpp");
    ExpectContains(app, "--vk-async-present");
    ExpectContains(app, "--no-vk-async-present");
    ExpectContains(app, "--vk-no-vsync");
    ExpectContains(app, "GetPrivateProfileIntA(\"Vulkan\", \"debug\"");
    ExpectContains(device, "app-async-present");
    ExpectContains(device, "ApplicationPresentQueue()");
    ExpectContains(fidelityfx, "queueInfo(ApplicationPresentQueue(), ApplicationPresentQueueRef())");
    ExpectContains(wsi, "queuePresent(ApplicationPresentQueue(), &presentInfo)");
    ExpectContains(wsi, "presentReadySemaphores.resize(imageCount");
    ExpectContains(wsi, "presentReadySemaphores[imageIndex]");
    ExpectContains(renderer, "pSignalSemaphores = &g_App.swapchain.presentReadySemaphores[imageIndex]");
}

}  // namespace
