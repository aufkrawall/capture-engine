#include "tests/flow/flow_capture_reader.h"
#include "tests/flow/flow_test_support.h"
#include "common/capture/inject_frame_ring_lease.h"

#include <atomic>
#include <psapi.h>
#include <set>

namespace {

using namespace ce::flow;

void SwitchRecording(bool includeOverlay, bool callback) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kStreamline;
    options.fsrUi = callback ? FSRUiResource::kNone : FSRUiResource::kPlaceholder;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(400)) << game.Error();
    auto* memory = game.CaptureMemory();
    memory->BeginWriteOverlayConfig();
    memory->overlayConfig.captureIncludeOverlay = includeOverlay;
    memory->EndWriteOverlayConfig();
    const auto capture = reinterpret_cast<void (*)(bool)>(GetProcAddress(
        GetModuleHandleW(L"capture_hook_x64.dll"), "CEFlow_SetInjectCapture"));
    ASSERT_NE(capture, nullptr);
    capture(true);
    memory->fpsLimiter.SetCaptureFps(0);  // observe every output, independently of the CFR cadence gate
    CaptureReader reader(game);
    auto record = [&](int count) {
        for (int i = 0; i < count; ++i) {
            if (!game.RenderFrame() || !reader.Consume())
                return false;
        }
        return true;
    };
    ASSERT_TRUE(record(70)) << game.Error();
    EXPECT_GT(reader.frames, 40u);
    const uint64_t dlssFrames = reader.frames;
    const uint64_t dlssGammaFrames = reader.gammaFrames;
    const uint64_t dlssOverlay = reader.framesWithOverlay;
    const size_t dlssGenerations = reader.generations.size();
    reader.shades.clear();

    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kFidelityFX)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, callback)) << game.Error();
    ASSERT_TRUE(record(120)) << game.Error();
    EXPECT_GT(reader.frames - dlssFrames, 170u) << "capture must include generated FSR outputs";
    EXPECT_GT(reader.generations.size(), dlssGenerations)
        << "the FSR swapchain must publish a new transport generation";
    const uint64_t fsrFrames = reader.frames;
    EXPECT_GT(reader.gammaFrames - dlssGammaFrames, 170u) << "recording must preserve display gamma";
    if (!includeOverlay)
        EXPECT_EQ(reader.framesWithOverlay, dlssOverlay);
    const auto fsrShades = reader.shades.size();
    ASSERT_TRUE(record(40)) << game.Error();
    EXPECT_GT(reader.frames - fsrFrames, 60u) << "FSR capture must keep progressing";
    EXPECT_GT(reader.shades.size(), 12u) << "published textures must contain advancing pixels";
    EXPECT_GE(reader.shades.size(), fsrShades);

    ASSERT_TRUE(game.SetFSRFrameGeneration(false, callback)) << game.Error();
    const uint64_t beforeSuspend = reader.frames;
    ASSERT_TRUE(record(25)) << game.Error();
    EXPECT_GT(reader.frames - beforeSuspend, 18u) << "FSR suspension must keep capture live";
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, callback)) << game.Error();
    const uint64_t beforeResume = reader.frames;
    ASSERT_TRUE(record(30)) << game.Error();
    EXPECT_GT(reader.frames - beforeResume, 40u);
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, callback)) << game.Error();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kStreamline)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    const uint64_t beforeReturn = reader.frames;
    const size_t beforeReturnGenerations = reader.generations.size();
    ASSERT_TRUE(record(120)) << game.Error();
    EXPECT_GT(reader.frames - beforeReturn, 100u);
    // Also when the allocator places the returning chain at the departing chain's address.
    EXPECT_GT(reader.generations.size(), beforeReturnGenerations)
        << "the returning Streamline swapchain must publish a new transport generation";

    EXPECT_GT(reader.timestamps.size(), reader.frames / 2);
    if (includeOverlay)
        EXPECT_GT(reader.framesWithOverlay, 100u);
    else
        EXPECT_EQ(reader.framesWithOverlay, 0u);
    capture(false);
    ASSERT_TRUE(reader.Consume());
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowInjectFGSwitch, NoCallbackFSRIncludedOverlayKeepsRecording) { SwitchRecording(true, false); }
TEST(FlowInjectFGSwitch, NoCallbackFSRExcludedOverlayKeepsRecording) { SwitchRecording(false, false); }
TEST(FlowInjectFGSwitch, CallbackFSRKeepsRecording) { SwitchRecording(true, true); }

TEST(FlowInjectFGSwitch, CapturePressurePreservesRuntimeBatchAndOverlay) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.swapchain = SwapchainKind::kFidelityFX;
    options.fsrUi = FSRUiResource::kPlaceholder;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(80)) << game.Error();
    auto* memory = game.CaptureMemory();
    const auto capture = reinterpret_cast<void (*)(bool)>(GetProcAddress(
        GetModuleHandleW(L"capture_hook_x64.dll"), "CEFlow_SetInjectCapture"));
    ASSERT_NE(capture, nullptr);
    capture(true);
    memory->fpsLimiter.SetCaptureFps(0);
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();  // deliberately leave every published texture leased
    EXPECT_GT(memory->runtimeState.injectProducerCpuLeaseBusyDrops.load(), 0u);
    const auto blocked = memory->frameRing.writeIndex.load();
    EXPECT_GT(blocked, 0u);
    EXPECT_LE(blocked, SHARED_TEXTURE_SLOT_COUNT);
    CaptureReader reader(game);
    ASSERT_TRUE(reader.Consume());
    for (int i = 0; i < 40; ++i) {
        ASSERT_TRUE(game.RenderFrame()) << game.Error();
        ASSERT_TRUE(reader.Consume());
    }
    EXPECT_GT(memory->frameRing.writeIndex.load() - blocked, 60u);
    EXPECT_GT(reader.shades.size(), 12u);
    capture(false);
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(10)) << game.Error();
    ASSERT_TRUE(reader.Consume());
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}


TEST(FlowInjectFGSwitch, HiddenOverlayKeepsPostProcessedRecording) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.swapchain = SwapchainKind::kFidelityFX;
    options.fsrUi = FSRUiResource::kPlaceholder;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(80)) << game.Error();
    auto* memory = game.CaptureMemory();
    memory->BeginWriteOverlayConfig();
    memory->overlayConfig.showOverlay = false;
    memory->overlayConfig.captureIncludeOverlay = true;  // preference cannot include a hidden overlay
    memory->EndWriteOverlayConfig();
    const auto capture = reinterpret_cast<void (*)(bool)>(GetProcAddress(
        GetModuleHandleW(L"capture_hook_x64.dll"), "CEFlow_SetInjectCapture"));
    ASSERT_NE(capture, nullptr);
    capture(true);
    memory->fpsLimiter.SetCaptureFps(0);
    CaptureReader reader(game);
    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(game.RenderFrame()) << game.Error();
        ASSERT_TRUE(reader.Consume());
    }
    EXPECT_GT(reader.frames, 80u);
    EXPECT_GT(reader.gammaFrames, 80u);
    EXPECT_EQ(reader.framesWithOverlay, 0u);
    const uint64_t beforeSuspend = reader.frames;
    const uint64_t gammaBeforeSuspend = reader.gammaFrames;
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, false)) << game.Error();
    for (int i = 0; i < 25; ++i) {
        ASSERT_TRUE(game.RenderFrame()) << game.Error();
        ASSERT_TRUE(reader.Consume());
    }
    EXPECT_GT(reader.frames - beforeSuspend, 18u);
    EXPECT_GT(reader.gammaFrames - gammaBeforeSuspend, 18u);
    EXPECT_EQ(reader.framesWithOverlay, 0u);
    capture(false);
    ExpectNoDebugLayerErrors();
}

using Signal = HRESULT (STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
Signal originalSignal = nullptr;
ID3D12CommandQueue* gameQueueForSignal = nullptr;
uintptr_t hookStart = 0;
size_t hookSize = 0;
std::atomic<unsigned> ceRuntimeSignals{0};
HRESULT STDMETHODCALLTYPE ObserveSignal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (queue != gameQueueForSignal && caller >= hookStart && caller - hookStart < hookSize)
        ++ceRuntimeSignals;
    return originalSignal(queue, fence, value);
}

TEST(FlowInjectFGSwitch, GammaMustNotSignalTheRuntimePresentationQueue) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    ce::flow::GameOptions options;
    options.swapchain = ce::flow::SwapchainKind::kFidelityFX;
    options.fsrUi = ce::flow::FSRUiResource::kPlaceholder;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(80)) << game.Error();
    auto queue = game.RetainGameQueue();
    gameQueueForSignal = queue.Get();
    MODULEINFO module{};
    ASSERT_TRUE(GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(L"capture_hook_x64.dll"),
                                     &module, sizeof(module)));
    hookStart = reinterpret_cast<uintptr_t>(module.lpBaseOfDll);
    hookSize = module.SizeOfImage;
    void** slot = &(*reinterpret_cast<void***>(queue.Get()))[14];
    originalSignal = reinterpret_cast<Signal>(*slot);
    struct RestoreSignal {
        void** slot;
        ~RestoreSignal() {
            DWORD oldProtection = 0;
            if (VirtualProtect(static_cast<void*>(slot), sizeof(void*), PAGE_READWRITE, &oldProtection)) {
                *slot = reinterpret_cast<void*>(originalSignal);
                DWORD ignored = 0;
                VirtualProtect(static_cast<void*>(slot), sizeof(void*), oldProtection, &ignored);
            } else {
                ADD_FAILURE() << "Could not restore Signal observer";
            }
        }
    } restore{slot};
    DWORD oldProtection = 0;
    ASSERT_TRUE(VirtualProtect(static_cast<void*>(slot), sizeof(void*), PAGE_READWRITE, &oldProtection));
    *slot = reinterpret_cast<void*>(&ObserveSignal);
    DWORD ignored = 0;
    ASSERT_TRUE(VirtualProtect(static_cast<void*>(slot), sizeof(void*), oldProtection, &ignored));
    ceRuntimeSignals = 0;
    const auto before = game.PostProcess();
    const bool rendered = game.RenderFrames(20);
    ASSERT_TRUE(rendered) << game.Error();
    EXPECT_GT(game.PostProcess().routeApplied, before.routeApplied);
    EXPECT_EQ(ceRuntimeSignals.load(), 0u) << "CE added queue Signals on a runtime queue distinct from the game queue";
}

}  // namespace
