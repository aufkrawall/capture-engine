#include "tests/flow/flow_test_support.h"
#include "common/capture/inject_frame_ring_lease.h"

#include <atomic>
#include <psapi.h>
#include <set>

namespace {

using namespace ce::flow;

// The test consumer opens the actual shared textures/fence and reads pixels
// before acknowledging the same metadata leases as the media process.
class CaptureReader {
public:
    explicit CaptureReader(FlowGame& game) : memory(game.CaptureMemory()) {
        game.RetainGameQueue()->GetDevice(IID_PPV_ARGS(&device));
        D3D12_COMMAND_QUEUE_DESC desc{};
        device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue));
        device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&allocator));
        device->CreateCommandList(0, desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
        list->Close();
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done));
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        refresh = reinterpret_cast<void (*)()>(GetProcAddress(GetModuleHandleW(L"capture_hook_x64.dll"),
                                                               "CEFlow_RefreshInjectCapture"));
    }
    ~CaptureReader() { if (event) CloseHandle(event); }

    bool Consume() {
        if (!refresh || !event || !queue || !done)
            return false;
        refresh();
        auto& ring = memory->frameRing;
        uint32_t read = ring.readIndex.load(std::memory_order_acquire);
        const uint32_t write = ring.writeIndex.load(std::memory_order_acquire);
        for (; read != write; ++read) {
            const auto& slot = ring.slots[read % FRAME_RING_SIZE];
            if (!slot.valid.load(std::memory_order_acquire))
                continue;
            const uint32_t current = static_cast<uint32_t>(memory->GetTransportGeneration());
            if (slot.transportGeneration != current) {
                ce::CompleteInjectFrameRingSlot(ring, read);
                continue;
            }
            ComPtr<ID3D12Fence> sharedFence;
            if (FAILED(device->OpenSharedHandle(reinterpret_cast<HANDLE>(memory->GetFenceShareHandle()),
                                                 IID_PPV_ARGS(&sharedFence))))
                return false;
            if (sharedFence->GetCompletedValue() < slot.fenceValue)
                break;
            ComPtr<ID3D12Resource> texture;
            if (FAILED(device->OpenSharedHandle(reinterpret_cast<HANDLE>(memory->GetSharedHandle(slot.textureIndex)),
                                                 IID_PPV_ARGS(&texture))))
                return false;
            const auto desc = texture->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT64 bytes = 0;
            device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
            D3D12_HEAP_PROPERTIES heap{
                D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = bytes;
            buffer.Height = 1;
            buffer.DepthOrArraySize = 1;
            buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> readback;
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&readback))))
                return false;
            if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr)))
                return false;
            // SIMULTANEOUS_ACCESS textures promote from COMMON to COPY_SOURCE.
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = texture.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = readback.Get();
            to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint = footprint;
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            if (FAILED(list->Close()))
                return false;
            ID3D12CommandList* submitted[] = {list.Get()};
            queue->ExecuteCommandLists(1, submitted);
            const uint64_t value = ++completion;
            if (FAILED(queue->Signal(done.Get(), value)) || FAILED(done->SetEventOnCompletion(value, event)) ||
                WaitForSingleObject(event, 10000) != WAIT_OBJECT_0)
                return false;
            void* mapped = nullptr;
            if (FAILED(readback->Map(0, nullptr, &mapped)))
                return false;
            const auto* pixels = static_cast<const uint32_t*>(mapped);
            const size_t stride = footprint.Footprint.RowPitch / sizeof(uint32_t);
            const uint32_t background = pixels[(desc.Height - 1) * stride + desc.Width - 1];
            bool overlay = false;
            for (UINT y = 0; y < desc.Height; ++y)
                for (UINT x = 0; x < desc.Width; ++x)
                    for (unsigned channel = 0; channel < 3; ++channel) {
                        const int sample = static_cast<int>((pixels[y * stride + x] >> (channel * 8)) & 255u);
                        const int base = static_cast<int>((background >> (channel * 8)) & 255u);
                        overlay = overlay || std::abs(sample - base) > 3;  // ignore gamma quantization dither
                    }
            readback->Unmap(0, nullptr);
            ++frames;
            const unsigned red = background & 255;
            if (red >= 16 && red <= 22) ++gammaFrames;
            if (overlay) ++framesWithOverlay;
            shades.insert(background);
            generations.insert(current);
            timestamps.insert(slot.timestamp);
            ce::CompleteInjectFrameRingSlot(ring, read);
        }
        return true;
    }

    uint64_t frames = 0;
    uint64_t framesWithOverlay = 0;
    uint64_t gammaFrames = 0;
    std::set<uint32_t> shades;
    std::set<uint32_t> generations;
    std::set<uint64_t> timestamps;

private:
    SharedMemoryLayout* memory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> done;
    HANDLE event = nullptr;
    uint64_t completion = 0;
    void (*refresh)() = nullptr;
};

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
    reader.shades.clear();

    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kFidelityFX)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, callback)) << game.Error();
    ASSERT_TRUE(record(120)) << game.Error();
    EXPECT_GT(reader.frames - dlssFrames, 170u) << "capture must include generated FSR outputs";
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
    ASSERT_TRUE(record(120)) << game.Error();
    EXPECT_GT(reader.frames - beforeReturn, 100u);
    EXPECT_GE(reader.generations.size(), 2u);

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
