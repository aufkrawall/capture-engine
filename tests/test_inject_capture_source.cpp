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
    // The internal header now carries declarations only; anchor on the
    // out-of-line definition in the layer_capture_impl units (rfind: the
    // header prototype would otherwise match first).
    const size_t begin = source.rfind("VulkanCaptureState::CommandResources* EnsureCaptureCommandResources(");
    const size_t end = source.find("bool GetLUIDFromPhysicalDevice(", begin);
    ASSERT_NE(begin, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    const std::string body = source.substr(begin, end - begin);
    ASSERT_FALSE(body.empty());

    EXPECT_EQ(body.find("fp_vkDeviceWaitIdle"), std::string::npos);
    EXPECT_NE(body.find("commandResourcesByQueueFamily"), std::string::npos);
}

TEST(InjectCaptureSourceTest, VulkanSwapchainInitIsNonPollingAndGenerationScoped) {
    const std::string source = ReadSource("hook/vulkan_layer/layer_capture.cpp");
    ASSERT_FALSE(source.empty());
    const size_t initBegin = source.rfind("void InitializeCapture(");
    const size_t cleanupBegin = source.find("void CleanupCapture(VkDevice device) {", initBegin);
    ASSERT_NE(initBegin, std::string::npos);
    ASSERT_NE(cleanupBegin, std::string::npos);
    const std::string body = source.substr(initBegin, cleanupBegin - initBegin);
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
    // The internal header now carries declarations only; anchor on the
    // out-of-line definitions in the opengl_hook_capture_impl units.
    const std::string initBody =
        FunctionBody(source, "void OpenGLCapture::Init(HDC hDC)", "void OpenGLCapture::CaptureFrame(HDC hDC)");
    const std::string captureBody = FunctionBody(source, "void OpenGLCapture::CaptureFrame(HDC hDC)",
                                                 "// End of the OpenGLCapture logical source");
    ASSERT_FALSE(initBody.empty());
    ASSERT_FALSE(captureBody.empty());

    const size_t fallback = initBody.find("captureReady = InitPBOFallback()");
    const size_t cleanup = initBody.rfind("CleanupTransportResources()", fallback);
    ASSERT_NE(fallback, std::string::npos);
    ASSERT_NE(cleanup, std::string::npos);
    EXPECT_LT(cleanup, fallback);
    EXPECT_NE(captureBody.find("Capture resize detected"), std::string::npos);
    EXPECT_NE(captureBody.find("if (opengl_hook_wglDXUnlockObjectsNV"), std::string::npos);
    EXPECT_NE(captureBody.find("context4->Signal(fence"), std::string::npos);
    EXPECT_NE(captureBody.find("GL_READ_FRAMEBUFFER_BINDING"), std::string::npos);
    EXPECT_NE(captureBody.find("previousPixelPackBuffer"), std::string::npos);
    EXPECT_NE(captureBody.find("FindAvailableCaptureTextureSlot"), std::string::npos);
    EXPECT_NE(captureBody.find("currentContext != opengl_hook_g_CaptureContext"), std::string::npos);
}

TEST(InjectCaptureSourceTest, D3D11CleanupOwnsImmediateContextAndSerializesCapture) {
    const std::string source = ReadSource("hook/apis/dx11_hook.cpp");
    ASSERT_FALSE(source.empty());
    // The internal header now carries declarations only; anchor on the
    // out-of-line definitions in the dx11_hook_capture_impl units.
    const std::string cleanup =
        FunctionBody(source, "void DX11Capture::Cleanup()", "bool DX11Capture::CreateSharedResources(");
    const std::string capture =
        FunctionBody(source, "bool DX11Capture::CaptureFrame(IDXGISwapChain*", "static DX11Capture");
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

    // The internal header carries prototypes; anchor on the out-of-line
    // definition in the dx9_hook_device unit (rfind).
    const size_t createDeviceBegin = source.rfind("HRESULT STDMETHODCALLTYPE DetourCreateDevice(");
    const size_t createDeviceEnd = source.find("IDirect3D9* WINAPI DetourDirect3DCreate9(", createDeviceBegin);
    ASSERT_NE(createDeviceBegin, std::string::npos);
    ASSERT_NE(createDeviceEnd, std::string::npos);
    const std::string createDevice = source.substr(createDeviceBegin, createDeviceEnd - createDeviceBegin);
    const std::string sharedSetup =
        // The internal header carries prototypes; anchor on the out-of-line
        // definition in the dx9_hook_capture_direct_ring unit (rfind).
        FunctionBody(source, "bool DX9Capture::SetupDirectD3D9SharedRing(", "bool HasPublishedGeneration() const");
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

TEST(InjectLifecycleSourceTest, HostShutdownUsesDormantHandshakeInsteadOfRemoteUnload) {
    const std::string injection = ReadSource("captureengine/injection_inject.cpp");
    ASSERT_FALSE(injection.empty());
    const size_t ejectAllBegin = injection.find("void InjectionManager::EjectAll()");
    const size_t ejectBegin = injection.find("void InjectionManager::Eject(DWORD pid)");
    ASSERT_NE(ejectAllBegin, std::string::npos);
    ASSERT_NE(ejectBegin, std::string::npos);
    const std::string ejectAll = injection.substr(ejectAllBegin, ejectBegin - ejectAllBegin);
    const std::string eject = injection.substr(ejectBegin);

    const size_t sharedDeadline = ejectAll.find("const ULONGLONG deadline = GetTickCount64() + 5000");
    const size_t perProcessWait = ejectAll.find("EjectWithDeadline(pid, deadline)");
    ASSERT_NE(sharedDeadline, std::string::npos);
    ASSERT_NE(perProcessWait, std::string::npos);
    EXPECT_LT(sharedDeadline, perProcessWait);
    EXPECT_NE(eject.find("GenerateInjectDormantEventName"), std::string::npos);
    EXPECT_NE(eject.find("GenerateVulkanDormantEventName"), std::string::npos);
    EXPECT_EQ(eject.find("CreateRemoteThread"), std::string::npos);
    EXPECT_EQ(eject.find("\"FreeLibrary\""), std::string::npos);
}

TEST(InjectLifecycleSourceTest, ShutdownNeverDetachesWorkersThatRetainManagerStorage) {
    const std::string injection = ReadSource("captureengine/injection_inject.cpp");
    ASSERT_FALSE(injection.empty());
    const size_t waitBegin = injection.find("void InjectionManager::WaitForInjectionThreads");
    const size_t nextFunction = injection.find("bool InjectionManager::HasActiveInjections", waitBegin);
    ASSERT_NE(waitBegin, std::string::npos);
    ASSERT_NE(nextFunction, std::string::npos);
    const std::string wait = injection.substr(waitBegin, nextFunction - waitBegin);

    EXPECT_NE(wait.find("t.join()"), std::string::npos);
    EXPECT_EQ(wait.find("t.detach()"), std::string::npos);
}

TEST(InjectLifecycleSourceTest, DynamicDetachDoesNoLoaderLockCleanup) {
    const std::string source = ReadSource("hook/main_dllmain.cpp");
    ASSERT_FALSE(source.empty());
    const size_t detachBegin = source.find("DLL_PROCESS_DETACH");
    ASSERT_NE(detachBegin, std::string::npos);
    const std::string detach = source.substr(detachBegin);

    EXPECT_NE(detach.find("RequestHookShutdown()"), std::string::npos);
    EXPECT_EQ(detach.find("InlineHook::RemoveAll()"), std::string::npos);
    EXPECT_EQ(detach.find("SafeShutdownHook("), std::string::npos);
    EXPECT_EQ(detach.find("InputManager::Get().Shutdown()"), std::string::npos);
}

TEST(InjectLifecycleSourceTest, VulkanProcAddressesRemainStableWhileDormant) {
    const std::string source = ReadSource("hook/vulkan_layer/layer_main.cpp");
    const std::string lifecycle = ReadSource("hook/vulkan_layer/layer_ipc.cpp");
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(lifecycle.empty());

    EXPECT_NE(source.find("Always expose forwarding hooks"), std::string::npos);
    EXPECT_EQ(source.find("if (whitelisted)"), std::string::npos);
    EXPECT_NE(source.find("Capture_vkQueuePresentKHR"), std::string::npos);
    EXPECT_NE(source.find("GET_MODULE_HANDLE_EX_FLAG_PIN"), std::string::npos);
    EXPECT_NE(lifecycle.find("if (!hostProcess)"), std::string::npos);
    EXPECT_NE(lifecycle.find("host process inaccessible"), std::string::npos);
}

TEST(InjectLifecycleSourceTest, DormantPresentPathsForwardToTheirSavedPredecessors) {
    const std::string dxgiPresent = ReadSource("hook/common/dxgi_shared_present.cpp");
    const std::string dx11Present = ReadSource("hook/apis/dx11_hook_present.cpp");
    const std::string dx9Present = ReadSource("hook/apis/dx9_hook_present_detours.cpp");
    const std::string dx8Present = ReadSource("hook/apis/dx8_hook_detours.cpp");
    const std::string openGLPresent = ReadSource("hook/apis/opengl_hook_capture.cpp");
    ASSERT_FALSE(dxgiPresent.empty());
    ASSERT_FALSE(dx11Present.empty());
    ASSERT_FALSE(dx9Present.empty());
    ASSERT_FALSE(dx8Present.empty());
    ASSERT_FALSE(openGLPresent.empty());

    EXPECT_NE(dxgiPresent.find("return CallOriginalPresent(pSwapChain, SyncInterval, Flags)"), std::string::npos);
    EXPECT_NE(dx11Present.find("return CallOriginalPresent(pSwapChain, SyncInterval, Flags)"), std::string::npos);
    EXPECT_NE(dx9Present.find("return dx9_hook_oPresent ? dx9_hook_oPresent"), std::string::npos);
    EXPECT_NE(dx8Present.find("return dx8_hook_oD3D8Present"), std::string::npos);
    EXPECT_NE(openGLPresent.find("return opengl_hook_oSwapBuffers ? opengl_hook_oSwapBuffers"), std::string::npos);
}

TEST(InjectLifecycleSourceTest, DormantLoaderAndVendorHooksAreExactPassThrough) {
    const std::string loader = ReadSource("hook/main_loadlibrary.cpp");
    const std::string getProc = ReadSource("hook/wrappers/iat_hook_init.cpp");
    const std::string ngxParams = ReadSource("hook/apis/nvngx_hook_params.cpp");
    const std::string ngxFeature = ReadSource("hook/apis/nvngx_hook_feature.cpp");
    const std::string ngxLifecycle = ReadSource("hook/apis/nvngx_hook_lifecycle.cpp");
    const std::string streamline = ReadSource("hook/apis/streamline_hook_api.cpp");
    const std::string streamlineDlssg = ReadSource("hook/apis/streamline_hook_dlssg.cpp");
    const std::string ffx = ReadSource("hook/apis/ffx_hook_context.cpp");
    ASSERT_FALSE(loader.empty());
    ASSERT_FALSE(getProc.empty());
    ASSERT_FALSE(ngxParams.empty());
    ASSERT_FALSE(ngxFeature.empty());
    ASSERT_FALSE(ngxLifecycle.empty());
    ASSERT_FALSE(streamline.empty());
    ASSERT_FALSE(streamlineDlssg.empty());
    ASSERT_FALSE(ffx.empty());

    const std::string loadA = FunctionBody(loader, "HMODULE WINAPI HookedLoadLibraryA", "HMODULE WINAPI HookedLoadLibraryW");
    EXPECT_NE(loadA.find("if (HookIsShuttingDown())"), std::string::npos);
    EXPECT_NE(loadA.find("return original(lpLibFileName)"), std::string::npos);

    const size_t realGetProc = getProc.find("FARPROC proc = ::GetProcAddress(hModule, lpProcName)");
    const size_t dormantGetProc = getProc.find("if (HookIsShuttingDown())", realGetProc);
    const size_t dynamicMap = getProc.find("g_DynamicHookLock", dormantGetProc);
    ASSERT_NE(realGetProc, std::string::npos);
    ASSERT_NE(dormantGetProc, std::string::npos);
    ASSERT_NE(dynamicMap, std::string::npos);
    EXPECT_LT(realGetProc, dormantGetProc);
    EXPECT_LT(dormantGetProc, dynamicMap);

    EXPECT_NE(ngxParams.find("if (HookIsShuttingDown())"), std::string::npos);
    EXPECT_NE(ngxFeature.find("if (HookIsShuttingDown())\n        return create();"), std::string::npos);
    EXPECT_NE(ngxLifecycle.find("return original(ctx, handle, params, callback)"), std::string::npos);
    EXPECT_NE(ngxLifecycle.find("return original(handle)"), std::string::npos);
    EXPECT_NE(streamline.find("return originalSetTag(viewport, tags, numTags, streamline_hook_commandBuffer)"),
              std::string::npos);
    EXPECT_NE(streamline.find("return originalEvaluateFeature(feature, streamline_hook_frame, inputs, numInputs"),
              std::string::npos);
    EXPECT_NE(streamlineDlssg.find("return originalGetState(viewport, state, streamline_hook_options)"),
              std::string::npos);
    EXPECT_NE(streamlineDlssg.find("return originalSetOptions(viewport, streamline_hook_options)"),
              std::string::npos);
    EXPECT_NE(ffx.find("return ffx_hook_g_Original_ffxCreateContext(ffx_hook_context, ffx_hook_desc, memCb)"),
              std::string::npos);
    EXPECT_NE(ffx.find("return CallFfxConfigureOriginalGuarded(originalConfigure, ffx_hook_context, ffx_hook_desc)"),
              std::string::npos);
}

TEST(InjectLifecycleSourceTest, DormantReflexKeepsResidentPredecessorsAndForwardsCalls) {
    const std::string reflex = ReadSource("hook/common/reflex_limiter.h");
    const std::string pacing = ReadSource("hook/common/reflex_limiter_detail/pacing.h");
    ASSERT_FALSE(reflex.empty());
    ASSERT_FALSE(pacing.empty());

    EXPECT_NE(reflex.find("return queryInterface(functionId)"), std::string::npos);
    EXPECT_NE(reflex.find("return forwardSetSleepMode(pDev, pParams)"), std::string::npos);
    EXPECT_NE(reflex.find("return forwardSleep(pDev)"), std::string::npos);
    const size_t shutdownBegin = pacing.find("inline void ReflexLimiter::Shutdown()");
    ASSERT_NE(shutdownBegin, std::string::npos);
    const std::string shutdown = pacing.substr(shutdownBegin);
    EXPECT_EQ(shutdown.find("origQueryInterface_ = nullptr"), std::string::npos);
    EXPECT_EQ(shutdown.find("directQueryInterfaceTrampoline_ = nullptr"), std::string::npos);
    EXPECT_NE(pacing.find("Preserve every predecessor/trampoline"), std::string::npos);
}

TEST(InjectLifecycleSourceTest, OpenGLExtensionHooksStayStableAndDormantOverridesPassThrough) {
    const std::string capture = ReadSource("hook/apis/opengl_hook_capture.cpp");
    const std::string sampler = ReadSource("hook/apis/opengl_sampler_override.cpp");
    const std::string storage = ReadSource("hook/apis/opengl_texture_storage_override.cpp");
    ASSERT_FALSE(capture.empty());
    ASSERT_FALSE(sampler.empty());
    ASSERT_FALSE(storage.empty());

    const size_t getProcBegin = capture.find("PROC WINAPI DetourWglGetProcAddress");
    const size_t swapInterval = capture.find("wglSwapIntervalEXT", getProcBegin);
    ASSERT_NE(getProcBegin, std::string::npos);
    ASSERT_NE(swapInterval, std::string::npos);
    const std::string getProcPrefix = capture.substr(getProcBegin, swapInterval - getProcBegin);
    EXPECT_EQ(getProcPrefix.find("return opengl_hook_oWglGetProcAddress"), std::string::npos);

    EXPECT_NE(sampler.find("if (HookIsShuttingDown())"), std::string::npos);
    EXPECT_NE(sampler.find("if (!HookIsShuttingDown() && count > 0 && textures)"), std::string::npos);
    EXPECT_NE(storage.find("if (!HookIsShuttingDown() && levels > 1)"), std::string::npos);
    EXPECT_NE(storage.find("if (HookIsShuttingDown())\n        return;"), std::string::npos);
}

TEST(InjectLifecycleSourceTest, TransientSwapchainLossDoesNotPermanentlyDormantTheRuntime) {
    const std::string dx11Overlay = ReadSource("hook/apis/dx11_hook_overlay.cpp");
    const std::string dx11Present = ReadSource("hook/apis/dx11_hook_present.cpp");
    const std::string dxgiPresent = ReadSource("hook/common/dxgi_shared_present_routing.cpp");
    const std::string dxgiPresent1 = ReadSource("hook/common/dxgi_shared_present1.cpp");
    ASSERT_FALSE(dx11Overlay.empty());
    ASSERT_FALSE(dx11Present.empty());
    ASSERT_FALSE(dxgiPresent.empty());
    ASSERT_FALSE(dxgiPresent1.empty());

    EXPECT_EQ(dx11Overlay.find("RequestHookShutdown()"), std::string::npos);
    EXPECT_EQ(dx11Present.find("RequestHookShutdown()"), std::string::npos);
    EXPECT_EQ(dxgiPresent.find("RequestHookShutdown()"), std::string::npos);
    EXPECT_EQ(dxgiPresent1.find("RequestHookShutdown()"), std::string::npos);
    EXPECT_NE(dx11Overlay.find("dx11_hook_g_UnsafeSwapChainObserved = true"), std::string::npos);
}

TEST(InjectLifecycleSourceTest, InitialLateConnectionDefersGraphicsReactivationUntilBootstrapCompletes) {
    const std::string lifecycle = ReadSource("hook/main_host_lifecycle.cpp");
    const std::string hookThread = ReadSource("hook/main_hookthread.cpp");
    ASSERT_FALSE(lifecycle.empty());
    ASSERT_FALSE(hookThread.empty());

    EXPECT_NE(lifecycle.find("g_HookLifecycleBootstrapComplete.load"), std::string::npos);
    EXPECT_NE(hookThread.find("MarkHookLifecycleBootstrapComplete()"), std::string::npos);
}

// Fast-app coverage (session 20260812_044326, dx12_fg_switch_test via Steam + RTSS, no FG):
// the game created its D3D12 swapchain before the HookThread reached the DX12 hook init, so in
// the leave-the-entry mode (two foreign overlays) CE never wrapped it and the overlay never
// appeared. The DXGI factory + CreateSwapChainForHwnd hooks must be the HookThread's first
// action — before module scans and IPC waits — and only a completed install may latch the
// retry flag.
TEST(InjectLifecycleSourceTest, GlobalFactorySwapchainHooksInstallBeforeAnyHookThreadDelay) {
    const std::string hookThread = ReadSource("hook/main_hookthread.cpp");
    const std::string installSource = ReadSource("hook/apis/dx12_hook_hook_install.cpp");
    ASSERT_FALSE(hookThread.empty());
    ASSERT_FALSE(installSource.empty());

    const size_t threadStart = hookThread.find("DWORD WINAPI HookThread(LPVOID lpParam)");
    ASSERT_NE(threadStart, std::string::npos);
    const size_t earlyFactoryHooks = hookThread.find("InstallGlobalVTableHooks();", threadStart);
    ASSERT_NE(earlyFactoryHooks, std::string::npos);
    const size_t iatHooks = hookThread.find("HookThread: IAT hooks installed", threadStart);
    ASSERT_NE(iatHooks, std::string::npos);
    const size_t checkAndInstall = hookThread.find("CheckAndInstallHooks();", threadStart);
    ASSERT_NE(checkAndInstall, std::string::npos);
    // The factory/swapchain-creation hooks run before the IAT work and before the main
    // graphics-hook install pass.
    EXPECT_LT(earlyFactoryHooks, iatHooks);
    EXPECT_LT(earlyFactoryHooks, checkAndInstall);

    // Only a completed install latches the retry flag; a missing dxgi.dll leaves it clear so
    // DX12Hook::Init can retry.
    const size_t onceFlag = installSource.find("static std::atomic<bool> s_installed{false};");
    ASSERT_NE(onceFlag, std::string::npos);
    EXPECT_NE(installSource.find("s_installed.store(true", onceFlag), std::string::npos);
    EXPECT_NE(installSource.find("s_installed.load(std::memory_order_acquire)"), std::string::npos);
}

TEST(InjectLifecycleSourceTest, ReactivationConsumesWakeupBeforeDiscoveryToPreserveNewerHostSignals) {
    const std::string lifecycle = ReadSource("hook/main_host_lifecycle.cpp");
    const std::string vulkan = ReadSource("hook/vulkan_layer/layer_ipc.cpp");
    ASSERT_FALSE(lifecycle.empty());
    ASSERT_FALSE(vulkan.empty());

    const size_t hookAttempt = lifecycle.find("bool TryReactivateHookRuntime(bool launcherOnly)");
    const size_t hookReset = lifecycle.find("ResetEvent(g_InjectReactivateEvent)", hookAttempt);
    const size_t hookReconnect = lifecycle.find("g_IPC->Reconnect()", hookAttempt);
    ASSERT_NE(hookReset, std::string::npos);
    ASSERT_NE(hookReconnect, std::string::npos);
    EXPECT_LT(hookReset, hookReconnect);

    const size_t layerAttempt = vulkan.find("bool TryReactivateLayer()");
    const size_t layerReset = vulkan.find("ResetEvent(g_LayerReactivateEvent)", layerAttempt);
    const size_t layerConnect = vulkan.find("LayerIPC_Init()", layerAttempt);
    ASSERT_NE(layerReset, std::string::npos);
    ASSERT_NE(layerConnect, std::string::npos);
    EXPECT_LT(layerReset, layerConnect);
}

TEST(InjectLifecycleSourceTest, TargetWakeupsExistBeforeRemoteOrApcLoadCanStart) {
    const std::string injection = ReadSource("captureengine/injection_inject.cpp");
    ASSERT_FALSE(injection.empty());

    const size_t remoteFunction = injection.find("bool InjectionManager::Inject(DWORD pid");
    const size_t remoteWakeup = injection.find("CreateTargetReactivationEvents(pid, true, true", remoteFunction);
    const size_t remoteLoad = injection.find("CreateRemoteThread", remoteFunction);
    ASSERT_NE(remoteWakeup, std::string::npos);
    ASSERT_NE(remoteLoad, std::string::npos);
    EXPECT_LT(remoteWakeup, remoteLoad);

    const size_t apcFunction = injection.find("bool InjectionManager::InjectEarly(");
    const size_t apcWakeup = injection.find("CreateTargetReactivationEvents(pid, true, true", apcFunction);
    const size_t apcLoad = injection.find("QueueUserAPC", apcFunction);
    ASSERT_NE(apcWakeup, std::string::npos);
    ASSERT_NE(apcLoad, std::string::npos);
    EXPECT_LT(apcWakeup, apcLoad);
}

TEST(InjectLifecycleSourceTest, FailedReactivationWaitsForAnotherTargetSignal) {
    const std::string lifecycle = ReadSource("hook/main_host_lifecycle.cpp");
    const std::string vulkan = ReadSource("hook/vulkan_layer/layer_ipc.cpp");
    ASSERT_FALSE(lifecycle.empty());
    ASSERT_FALSE(vulkan.empty());

    EXPECT_EQ(lifecycle.find("WaitForAdvertisedHostToEnd"), std::string::npos);
    EXPECT_EQ(vulkan.find("WaitForAdvertisedHostToEnd"), std::string::npos);
    EXPECT_NE(lifecycle.find("waiting for the next target signal"), std::string::npos);
    EXPECT_NE(vulkan.find("waiting for the next target signal"), std::string::npos);
}

TEST(InjectLifecycleSourceTest, LauncherHooksBecomeExactPassThroughAndWaitOnHostEvents) {
    const std::string hookThread = ReadSource("hook/main_hookthread.cpp");
    const std::string injection = ReadSource("hook/main_injection.cpp");
    const std::string lifecycle = ReadSource("hook/main_host_lifecycle.cpp");
    ASSERT_FALSE(hookThread.empty());
    ASSERT_FALSE(injection.empty());
    ASSERT_FALSE(lifecycle.empty());

    const size_t launcherBegin = hookThread.find("main_g_ProcessCategory == ProcessCategory::Launcher");
    const size_t launcherEnd = hookThread.find("// POTENTIAL GAMES", launcherBegin);
    ASSERT_NE(launcherBegin, std::string::npos);
    ASSERT_NE(launcherEnd, std::string::npos);
    const std::string launcher = hookThread.substr(launcherBegin, launcherEnd - launcherBegin);
    EXPECT_NE(launcher.find("RunLauncherHookLifecycle()"), std::string::npos);
    EXPECT_EQ(launcher.find("Sleep("), std::string::npos);

    const size_t createA = injection.find("BOOL WINAPI HookedCreateProcessA");
    const size_t createW = injection.find("BOOL WINAPI HookedCreateProcessW");
    ASSERT_NE(createA, std::string::npos);
    ASSERT_NE(createW, std::string::npos);
    const size_t dormantA = injection.find("HookIsShuttingDown()", createA);
    const size_t whitelistA = injection.find("ShouldInjectChild", createA);
    const size_t dormantW = injection.find("HookIsShuttingDown()", createW);
    const size_t whitelistW = injection.find("ShouldInjectChild", createW);
    ASSERT_NE(dormantA, std::string::npos);
    ASSERT_NE(whitelistA, std::string::npos);
    ASSERT_NE(dormantW, std::string::npos);
    ASSERT_NE(whitelistW, std::string::npos);
    EXPECT_LT(dormantA, whitelistA);
    EXPECT_LT(dormantW, whitelistW);

    EXPECT_NE(lifecycle.find("void RunLauncherHookLifecycle()"), std::string::npos);
    EXPECT_NE(lifecycle.find("WaitForMultipleObjects(waitCount, waits, FALSE, INFINITE)"), std::string::npos);
    EXPECT_NE(lifecycle.find("launcherOnly"), std::string::npos);
}

TEST(InjectLifecycleSourceTest, HookRestorationRejectsReusedModuleStorage) {
    const std::string iat = ReadSource("hook/wrappers/iat_hook.cpp");
    const std::string vtable = ReadSource("hook/wrappers/vtable_hook.cpp");
    ASSERT_FALSE(iat.empty());
    ASSERT_FALSE(vtable.empty());

    EXPECT_NE(iat.find("memory.Type != MEM_IMAGE"), std::string::npos);
    EXPECT_NE(iat.find("memory.AllocationBase != it->targetModule"), std::string::npos);
    EXPECT_NE(vtable.find("slotMemory.AllocationBase != ownership->second.allocationBase"), std::string::npos);
    const size_t publication = vtable.find("MemoryBarrier()");
    const size_t claim = vtable.find("InterlockedCompareExchangePointer", publication);
    ASSERT_NE(publication, std::string::npos);
    ASSERT_NE(claim, std::string::npos);
    EXPECT_LT(publication, claim);
}

TEST(InjectLifecycleSourceTest, PublishedHookRollbackRetainsCallablePredecessorStorage) {
    const std::string inlineHook = ReadSource("hook/wrappers/inline_hook.cpp");
    const std::string deepHook = ReadSource("hook/wrappers/inline_hook_deep.cpp");
    const std::string vtable = ReadSource("hook/wrappers/vtable_hook.cpp");
    ASSERT_FALSE(inlineHook.empty());
    ASSERT_FALSE(deepHook.empty());
    ASSERT_FALSE(vtable.empty());

    EXPECT_NE(inlineHook.find("Retaining rolled-back published trampoline"), std::string::npos);
    EXPECT_NE(deepHook.find("Retaining rolled-back published trampoline"), std::string::npos);
    EXPECT_NE(vtable.find("previousCallerOriginal"), std::string::npos);
    EXPECT_NE(vtable.find("*ppOriginal = previousCallerOriginal"), std::string::npos);
    EXPECT_NE(vtable.find("could create CE -> foreign -> CE"), std::string::npos);
}

TEST(InjectLifecycleSourceTest, IATPatchingPreservesPreexistingForeignOwners) {
    const std::string iat = ReadSource("hook/wrappers/iat_hook.cpp");
    ASSERT_FALSE(iat.empty());

    const size_t ownerCheck = iat.find("IsForeignIATOwner(currentFunction, sourceModule, functionName)");
    const size_t slotClaim = iat.find("InterlockedCompareExchangePointer", ownerCheck);
    ASSERT_NE(ownerCheck, std::string::npos);
    ASSERT_NE(slotClaim, std::string::npos);
    EXPECT_LT(ownerCheck, slotClaim);
    EXPECT_NE(iat.find("Preserving foreign owner"), std::string::npos);
    EXPECT_NE(iat.find("through export/vtable routes"), std::string::npos);
}
