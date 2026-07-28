#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "source_fragment_reader.h"

namespace {

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

std::string FunctionBody(const std::string& source, const std::string& signature, const std::string& nextSignature) {
    const size_t begin = source.find(signature);
    if (begin == std::string::npos)
        return {};
    const size_t end = source.find(nextSignature, begin + signature.size());
    return source.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

}  // namespace

TEST(InjectCaptureSourceTest, VulkanPresentWaitIsRewiredOnlyAfterCaptureSubmission) {
    const std::string source = ReadSource("hook/vulkan_layer/vulkan_layer.cpp");
    ASSERT_FALSE(source.empty());
    const std::string body = FunctionBody(source, "auto doCapture = [&]()", "auto doScreenshot = [&]()");
    ASSERT_FALSE(body.empty());

    EXPECT_NE(body.find("const bool captureSubmitted"), std::string::npos);
    EXPECT_NE(body.find("if (captureSubmitted && captureDone != VK_NULL_HANDLE)"), std::string::npos);
    EXPECT_EQ(body.find("if (captureDone != VK_NULL_HANDLE)"), std::string::npos);
}

TEST(InjectCaptureSourceTest, VulkanCaptureFailureCannotPoisonFenceOrPublishUnsubmittedFrame) {
    const std::string source = ReadSource("hook/vulkan_layer/layer_capture.cpp");
    ASSERT_FALSE(source.empty());
    const std::string body = FunctionBody(source, "bool CaptureFrame(", "// ---- Vulkan Screenshot ----");
    ASSERT_FALSE(body.empty());

    const size_t endCommandBuffer = body.find("fp_vkEndCommandBuffer");
    const size_t resetCommandBuffer = body.find("fp_vkResetCommandBuffer");
    const size_t beginCommandBuffer = body.find("fp_vkBeginCommandBuffer", resetCommandBuffer);
    const size_t resetFence = body.find("fp_vkResetFences", endCommandBuffer);
    const size_t queueSubmit = body.find("const VkResult submitResult", resetFence);
    const size_t publish = body.find("LayerIPC_SignalFrameReady", queueSubmit);
    ASSERT_NE(endCommandBuffer, std::string::npos);
    ASSERT_NE(resetCommandBuffer, std::string::npos);
    ASSERT_NE(beginCommandBuffer, std::string::npos);
    ASSERT_NE(resetFence, std::string::npos);
    ASSERT_NE(queueSubmit, std::string::npos);
    ASSERT_NE(publish, std::string::npos);
    EXPECT_LT(resetCommandBuffer, beginCommandBuffer);
    EXPECT_LT(endCommandBuffer, resetFence);
    EXPECT_LT(resetFence, queueSubmit);
    EXPECT_LT(queueSubmit, publish);
    EXPECT_NE(body.find("Capture queue submit failed"), std::string::npos);
    EXPECT_NE(body.find("recoveryFence"), std::string::npos);
    EXPECT_NE(body.find("IsCaptureTextureSlotOutstanding"), std::string::npos);
}

TEST(InjectCaptureSourceTest, VulkanQueueFamilyChangesDoNotWaitForTheWholeDevice) {
    const std::string source = ReadSource("hook/vulkan_layer/layer_capture.cpp");
    ASSERT_FALSE(source.empty());
    const std::string body = FunctionBody(source,
                                          "static VulkanCaptureState::CommandResources* "
                                          "EnsureCaptureCommandResources(",
                                          "// Helper to get LUID from Vulkan Physical Device");
    ASSERT_FALSE(body.empty());

    EXPECT_EQ(body.find("fp_vkDeviceWaitIdle"), std::string::npos);
    EXPECT_NE(body.find("commandResourcesByQueueFamily"), std::string::npos);
}

TEST(InjectCaptureSourceTest, VulkanSwapchainInitIsNonPollingAndGenerationScoped) {
    const std::string source = ReadSource("hook/vulkan_layer/layer_capture.cpp");
    ASSERT_FALSE(source.empty());
    const std::string body = FunctionBody(source, "void InitializeCapture(", "void CleanupCapture(");
    ASSERT_FALSE(body.empty());

    EXPECT_EQ(body.find("Sleep("), std::string::npos);
    EXPECT_NE(body.find("it->second.swapchain == swapchain"), std::string::npos);
    EXPECT_NE(body.find("captureWidth == extent.width"), std::string::npos);
    EXPECT_NE(body.find("Failed to create capture fence"), std::string::npos);
    EXPECT_NE(body.find("Failed to create capture signal semaphore"), std::string::npos);
    EXPECT_NE(body.find("requires an exportable timeline fence"), std::string::npos);
}

TEST(InjectCaptureSourceTest, VulkanCaptureUsesTheFullSharedTextureLeaseSpace) {
    const std::string source = ReadSource("hook/vulkan_layer/layer_capture.cpp");
    ASSERT_FALSE(source.empty());

    EXPECT_EQ(source.find("kTextureCount = 4"), std::string::npos);
    EXPECT_NE(source.find("kTextureCount = SHARED_TEXTURE_SLOT_COUNT"), std::string::npos);
    EXPECT_EQ(source.find("HANDLE kmtHandles[4]"), std::string::npos);
    EXPECT_NE(source.find("HANDLE kmtHandles[ENCODER_TEXTURE_SLOT_COUNT]"), std::string::npos);
}

TEST(InjectCaptureSourceTest, OpenGLFallbackCleansPartialInteropAndTracksResizeAndFenceCompletion) {
    const std::string source = ReadSource("hook/apis/opengl_hook.cpp");
    ASSERT_FALSE(source.empty());
    const std::string initBody = FunctionBody(source, "void Init(HDC hDC)", "void CaptureFrame(HDC hDC)");
    const std::string captureBody = FunctionBody(source, "void CaptureFrame(HDC hDC)", "static GLsizei ParseGLMSAA");
    ASSERT_FALSE(initBody.empty());
    ASSERT_FALSE(captureBody.empty());

    const size_t fallback = initBody.find("captureReady = InitPBOFallback()");
    const size_t cleanup = initBody.rfind("CleanupTransportResources()", fallback);
    ASSERT_NE(fallback, std::string::npos);
    ASSERT_NE(cleanup, std::string::npos);
    EXPECT_LT(cleanup, fallback);
    EXPECT_NE(captureBody.find("Capture resize detected"), std::string::npos);
    EXPECT_NE(captureBody.find("if (wglDXUnlockObjectsNV"), std::string::npos);
    EXPECT_NE(captureBody.find("context4->Signal(fence"), std::string::npos);
    EXPECT_NE(captureBody.find("GL_READ_FRAMEBUFFER_BINDING"), std::string::npos);
    EXPECT_NE(captureBody.find("previousPixelPackBuffer"), std::string::npos);
    EXPECT_NE(captureBody.find("FindAvailableCaptureTextureSlot"), std::string::npos);
    EXPECT_NE(captureBody.find("currentContext != g_CaptureContext"), std::string::npos);
}

TEST(InjectCaptureSourceTest, D3D11CleanupOwnsImmediateContextAndSerializesCapture) {
    const std::string source = ReadSource("hook/apis/dx11_hook.cpp");
    ASSERT_FALSE(source.empty());
    const std::string cleanup = FunctionBody(source, "void Cleanup() override", "void CreateSharedResources(");
    const std::string capture = FunctionBody(source, "bool CaptureFrame(IDXGISwapChain*", "static DX11Capture");
    ASSERT_FALSE(cleanup.empty());
    ASSERT_FALSE(capture.empty());

    EXPECT_NE(cleanup.find("g_DeferredRelease.Queue(cachedContext)"), std::string::npos);
    EXPECT_NE(capture.find("std::try_to_lock"), std::string::npos);
    EXPECT_NE(capture.find("static std::atomic<int> s_captureFrameCount"), std::string::npos);
    EXPECT_NE(capture.find("FindAvailableCaptureTextureSlot"), std::string::npos);
}

TEST(InjectCaptureSourceTest, D3D12SharedCaptureInitializationAndSubmissionAreTransactional) {
    const std::string source = ReadSource("hook/capture/shared_capture_d3d12.cpp");
    ASSERT_FALSE(source.empty());
    const std::string init =
        FunctionBody(source, "bool SharedCaptureD3D12::Initialize(", "bool SharedCaptureD3D12::CreateSharedResources(");
    const std::string reset = FunctionBody(source, "bool SharedCaptureD3D12::Reset(bool force)",
                                           "bool SharedCaptureD3D12::IsInitializedFor(");
    const std::string capture =
        FunctionBody(source, "bool SharedCaptureD3D12::CaptureFrame(", "bool SharedCaptureD3D12::GetCurrentFrame(");
    ASSERT_FALSE(init.empty());
    ASSERT_FALSE(reset.empty());
    ASSERT_FALSE(capture.empty());

    EXPECT_NE(init.find("Reset();"), std::string::npos);
    EXPECT_NE(init.find("pSwapChain->GetBuffer(0"), std::string::npos);
    EXPECT_NE(init.find("Initial command-list Close failed"), std::string::npos);
    EXPECT_EQ(reset.find("WaitForSingleObject"), std::string::npos);
    EXPECT_NE(reset.find("m_RetiredGenerations.emplace_back"), std::string::npos);
    EXPECT_NE(init.find("kMaxRetiredGenerations"), std::string::npos);
    EXPECT_NE(capture.find("pCommandQueue->GetDevice"), std::string::npos);
    EXPECT_NE(capture.find("FindAvailableCaptureTextureSlot"), std::string::npos);
    EXPECT_NE(capture.find("Allocator Reset failed"), std::string::npos);
    EXPECT_NE(capture.find("Command-list Close failed"), std::string::npos);
    EXPECT_NE(capture.find("Queue Signal failed"), std::string::npos);
}

TEST(InjectCaptureSourceTest, SharedCaptureManagerOutlivesStaticCaptureTargets) {
    const std::string source = ReadSource("hook/capture/shared_capture.cpp");
    ASSERT_FALSE(source.empty());
    const std::string getManager =
        FunctionBody(source, "CaptureManager& CaptureManager::Get()", "void CaptureManager::RegisterCaptureTarget(");
    ASSERT_FALSE(getManager.empty());

    EXPECT_NE(getManager.find("static CaptureManager* const instance = new CaptureManager();"), std::string::npos);
    EXPECT_EQ(getManager.find("static CaptureManager instance;"), std::string::npos);
}

TEST(InjectCaptureSourceTest, LegacyInjectProducersDoNotOverwriteOutstandingSlots) {
    for (const char* path : {"hook/apis/dx8_hook.cpp", "hook/apis/dx9_hook.cpp", "hook/apis/ddraw_hook.cpp"}) {
        const std::string source = ReadSource(path);
        ASSERT_FALSE(source.empty()) << path;
        EXPECT_NE(source.find("FindAvailableCaptureTextureSlot"), std::string::npos) << path;
        EXPECT_NE(source.find("publishedFenceValue"), std::string::npos) << path;
    }

    const std::string dx9 = ReadSource("hook/apis/dx9_hook.cpp");
    EXPECT_NE(dx9.find("IsCaptureTextureSlotOutstanding(sharedMem, idx)"), std::string::npos);
    EXPECT_EQ(dx9.find("while (completionQuery->GetData"), std::string::npos);
}

TEST(InjectCaptureSourceTest, ClassicD3D9CapturePreservesDeviceTypeAndUsesSharedHelperRing) {
    const std::string source = ReadSource("hook/apis/dx9_hook.cpp");
    ASSERT_FALSE(source.empty());

    const std::string createDevice =
        FunctionBody(source, "static HRESULT STDMETHODCALLTYPE DetourCreateDevice(", "// Hook: Direct3DCreate9");
    const std::string sharedSetup =
        FunctionBody(source, "bool SetupDirectD3D9SharedRing(", "bool HasPublishedGeneration() const");
    ASSERT_FALSE(createDevice.empty());
    ASSERT_FALSE(sharedSetup.empty());

    EXPECT_NE(createDevice.find("ShouldPromoteClassicD3D9Device"), std::string::npos);
    EXPECT_NE(createDevice.find("oCreateDevice(self"), std::string::npos);
    EXPECT_EQ(createDevice.find("CreateDeviceEx"), std::string::npos);
    EXPECT_EQ(source.find("ManagedPoolFix"), std::string::npos);
    EXPECT_EQ(source.find("MirrorsEdge.exe"), std::string::npos);

    const size_t nativeProbe = sharedSetup.find("ProbeDirectD3D9SharedTexture(device, \"game device\")");
    const size_t helperProbe = sharedSetup.find("EnsureDirectD3D9ExProducerDevice");
    ASSERT_NE(nativeProbe, std::string::npos);
    ASSERT_NE(helperProbe, std::string::npos);
    EXPECT_LT(nativeProbe, helperProbe);
    EXPECT_NE(sharedSetup.find("skipping helper producers"), std::string::npos);
    EXPECT_NE(sharedSetup.find("EnsureDirectD3D9ExProducerDevice"), std::string::npos);
    EXPECT_NE(sharedSetup.find("TrySetupDirectD3D9SharedRingWithProducer"), std::string::npos);
    EXPECT_NE(source.find("gameDevice->CreateTexture"), std::string::npos);
    EXPECT_NE(source.find("d3d11Device->OpenSharedResource"), std::string::npos);
    EXPECT_NE(source.find("Direct D3D9 shared ring zero-copy path active"), std::string::npos);
}

TEST(InjectCaptureSourceTest, ForcedAFPathsAvoidDrawTimeReplacementAndCoverMutableLegacyState) {
    const std::string dx10 = ReadSource("hook/apis/dx11_hook.cpp");
    const std::string dx9 = ReadSource("hook/apis/dx9_hook.cpp");
    const std::string dx9State = ReadSource("hook/apis/dx9_sampler_state.cpp");
    const std::string legacyState = ReadSource("hook/apis/legacy_d3d_sampler_state.cpp");
    ASSERT_FALSE(dx10.empty());
    ASSERT_FALSE(dx9.empty());
    ASSERT_FALSE(dx9State.empty());
    ASSERT_FALSE(legacyState.empty());

    EXPECT_EQ(dx10.find("DetourPSSetSamplers10"), std::string::npos);
    EXPECT_EQ(dx10.find("g_SamplerCache10"), std::string::npos);
    EXPECT_NE(dx10.find("Modified sampler rejected; retrying original descriptor"), std::string::npos);

    EXPECT_NE(dx9.find("D3D9SamplerVTableRecord"), std::string::npos);
    EXPECT_NE(dx9.find("InstallD3D9SamplerHooks(vtable)"), std::string::npos);
    EXPECT_NE(dx9.find("DetourStateBlockApply"), std::string::npos);
    EXPECT_NE(dx9.find("ReconcileAfterExternalStateChange"), std::string::npos);
    EXPECT_NE(dx9State.find("texture->GetLevelCount()"), std::string::npos);
    EXPECT_NE(dx9State.find("D3DUSAGE_AUTOGENMIPMAP"), std::string::npos);
    EXPECT_NE(dx9State.find("D3DPTFILTERCAPS_MINFANISOTROPIC"), std::string::npos);
    EXPECT_NE(dx9State.find("bootstrapAttempted"), std::string::npos);
    EXPECT_EQ(dx9State.find("DrawPrimitive"), std::string::npos);

    EXPECT_NE(legacyState.find("traits.anisotropicMag = 5"), std::string::npos);
    EXPECT_NE(legacyState.find("combinedAddress"), std::string::npos);
    EXPECT_NE(legacyState.find("bootstrapSweepPending"), std::string::npos);
    EXPECT_EQ(legacyState.find("DrawPrimitive"), std::string::npos);

    const std::string ddraw = ReadSource("hook/apis/ddraw_hook.cpp");
    EXPECT_NE(ddraw.find("#define D3D7_VTABLE_GETTEXTURESTAGESTATE 36"), std::string::npos);
    EXPECT_NE(ddraw.find("#define D3D7_VTABLE_SETTEXTURESTAGESTATE 37"), std::string::npos);
    EXPECT_NE(ddraw.find("#define D3D7_VTABLE_SETRENDERSTATE 20"), std::string::npos);
    EXPECT_NE(ddraw.find("InstallLegacyD3DDeviceHooks"), std::string::npos);
    EXPECT_NE(ddraw.find("D3D7_VTABLE_APPLYSTATEBLOCK 39"), std::string::npos);

    const std::string dx8 = ReadSource("hook/apis/dx8_hook.cpp");
    EXPECT_NE(dx8.find("D3D8_VTABLE_APPLYSTATEBLOCK 54"), std::string::npos);
    EXPECT_NE(dx8.find("D3D8SamplerVTableRecord"), std::string::npos);
    EXPECT_NE(dx8.find("ReconcileAfterExternalStateChange"), std::string::npos);
}

TEST(InjectCaptureSourceTest, OpenGLSamplerOverridesCoverParameterAllocationAndBindEvents) {
    const std::string sampler = ReadSource("hook/apis/opengl_sampler_override.cpp");
    const std::string storage = ReadSource("hook/apis/opengl_texture_storage_override.cpp");
    ASSERT_FALSE(sampler.empty());
    ASSERT_FALSE(storage.empty());

    EXPECT_NE(sampler.find("GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT"), std::string::npos);
    EXPECT_NE(sampler.find("glSamplerParameteriv"), std::string::npos);
    EXPECT_NE(sampler.find("glTextureParameterfEXT"), std::string::npos);
    EXPECT_NE(sampler.find("glGetTextureLevelParameteriv"), std::string::npos);
    EXPECT_NE(sampler.find("glBindTexture"), std::string::npos);
    EXPECT_NE(sampler.find("glBindSampler"), std::string::npos);
    EXPECT_NE(sampler.find("glDeleteTextures"), std::string::npos);
    EXPECT_NE(sampler.find("glDeleteSamplers"), std::string::npos);
    EXPECT_NE(sampler.find("ApplyOpenGLMinFilter"), std::string::npos);
    EXPECT_NE(sampler.find("OverrideFloatValue"), std::string::npos);

    EXPECT_NE(storage.find("glCompressedTexImage2D"), std::string::npos);
    EXPECT_NE(storage.find("glCopyTexImage2D"), std::string::npos);
    EXPECT_NE(storage.find("glTexStorage2D"), std::string::npos);
    EXPECT_NE(storage.find("glGenerateTextureMipmap"), std::string::npos);
    EXPECT_NE(storage.find("glTextureView"), std::string::npos);
    EXPECT_EQ(storage.find("glBindTexture"), std::string::npos);
}
