#include "tests/flow/flow_test_support.h"
#include "tests/flow/queue_dispatch_probe.h"

namespace {
using namespace ce::flow;

TEST(FlowQueueDispatch, UntrackedImplementationUsesItsOwnEntry) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    {
        Microsoft::WRL::ComPtr<FirstQueueDispatchProbe> first;
        Microsoft::WRL::ComPtr<SecondQueueDispatchProbe> second;
        first.Attach(new FirstQueueDispatchProbe(game.RetainGameQueue().Get()));
        second.Attach(new SecondQueueDispatchProbe(game.RetainGameQueue().Get()));
        ASSERT_NE(first->CurrentECL(), second->CurrentECL());
        // Reject the wrong target before invoking it: a native method cannot accept this proxy layout.
        ASSERT_EQ(game.QueueOriginal(first.Get()), first->CurrentECL());
        ASSERT_EQ(game.QueueOriginal(second.Get()), second->CurrentECL());
        game.ForwardQueue(first.Get());
        game.ForwardQueue(second.Get());
        EXPECT_EQ(first->Calls(), 1u);
        EXPECT_EQ(second->Calls(), 1u);
    }
    EXPECT_EQ(g_queueProbeObjects.load(), 0u);
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowQueueDispatch, InstalledImplementationsRetainExactOriginals) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    {
        Microsoft::WRL::ComPtr<FirstQueueDispatchProbe> first;
        Microsoft::WRL::ComPtr<SecondQueueDispatchProbe> second;
        first.Attach(new FirstQueueDispatchProbe(game.RetainGameQueue().Get()));
        second.Attach(new SecondQueueDispatchProbe(game.RetainGameQueue().Get()));
        void* firstOriginal = first->CurrentECL();
        void* secondOriginal = second->CurrentECL();
        ASSERT_NE(firstOriginal, secondOriginal);
        game.TrackQueue(first.Get());
        game.TrackQueue(second.Get());
        // Duplicate discovery preserves the predecessor rather than inserting the detour into its own chain.
        game.TrackQueue(first.Get());
        for (int i = 0; i < 16; ++i) {
            ASSERT_EQ(game.QueueOriginal(first.Get()), firstOriginal);
            ASSERT_EQ(game.QueueOriginal(second.Get()), secondOriginal);
            game.ForwardQueue(first.Get());
            game.ForwardQueue(second.Get());
        }
        game.ResetQueueBindings();
        ASSERT_EQ(game.QueueOriginal(first.Get()), firstOriginal);
        ASSERT_EQ(game.QueueOriginal(second.Get()), secondOriginal);
        game.ForwardQueue(first.Get());
        game.ForwardQueue(second.Get());
        EXPECT_EQ(first->Calls(), 17u);
        EXPECT_EQ(second->Calls(), 17u);
    }
    EXPECT_EQ(g_queueProbeObjects.load(), 0u);
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowQueueDispatch, TranslatorDiscoverySubmitsWithoutClaimingTheGameQueue) {
    FlowGame game(CurrentTestName());
    auto hook = GetModuleHandleW(L"capture_hook_x64.dll");
    using SubmitDiscovery = void (*)(ID3D12CommandQueue*, ID3D12CommandList*);
    auto submit = reinterpret_cast<SubmitDiscovery>(GetProcAddress(hook, "CEFlow_SubmitDiscoveryQueue"));
    auto snapshot = reinterpret_cast<CEFlow_GetQueueBinding_t>(GetProcAddress(hook, "CEFlow_GetQueueBinding"));
    ASSERT_NE(submit, nullptr);
    ASSERT_NE(snapshot, nullptr);
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    ASSERT_EQ(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), S_OK);
    ASSERT_EQ(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), S_OK);
    ASSERT_EQ(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), S_OK);
    D3D12_COMMAND_QUEUE_DESC desc{};
    ComPtr<ID3D12CommandQueue> native;
    ASSERT_EQ(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&native)), S_OK);
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ASSERT_EQ(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), S_OK);
    ASSERT_EQ(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                       IID_PPV_ARGS(&list)), S_OK);
    ASSERT_EQ(list->Close(), S_OK);
    ComPtr<FirstQueueDispatchProbe> probe;
    probe.Attach(new FirstQueueDispatchProbe(native.Get()));
    game.TrackQueue(probe.Get());
    CEFlowQueueBinding before{}, after{};
    snapshot(&before);
    submit(probe.Get(), list.Get());
    snapshot(&after);
    EXPECT_EQ(probe->Calls(), 2u);
    EXPECT_EQ(after.queue, before.queue);
    EXPECT_EQ(after.device, before.device);
    EXPECT_EQ(after.capturedSwapchain, before.capturedSwapchain);
    EXPECT_EQ(after.successfulPostSLSwapchain, before.successfulPostSLSwapchain);
    // Complete the actual probe submissions before releasing their allocator/device. The game enables
    // the debug layer next; a probe device retained as application state would be invalidated there.
    ComPtr<ID3D12Fence> fence;
    ASSERT_EQ(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), S_OK);
    const HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ASSERT_NE(done, nullptr);
    ASSERT_EQ(native->Signal(fence.Get(), 1), S_OK);
    ASSERT_EQ(fence->SetEventOnCompletion(1, done), S_OK);
    const DWORD wait = WaitForSingleObject(done, 5000);
    CloseHandle(done);
    ASSERT_EQ(wait, WAIT_OBJECT_0);
    fence.Reset();
    list.Reset();
    allocator.Reset();
    probe.Reset();
    native.Reset();
    device.Reset();
    EXPECT_EQ(g_queueProbeObjects.load(), 0u);
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrames(3)) << game.Error();
    snapshot(&after);
    EXPECT_NE(after.queue, nullptr);
    EXPECT_NE(after.device, nullptr);
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

// ReLimiter 4.0's Streamline GPU handoff and D3D12 queue-based pacer both reject a command queue
// whose vtable[10] (ExecuteCommandLists) or vtable[14] (Signal) has been overwritten outside
// D3D12Core.dll / d3d12.dll. Verify on a real headless WARP D3D12 queue that CE intercepts via
// an inline hook and leaves the class vtable slots pointing inside D3D12Core.dll / d3d12.dll.
TEST(FlowQueueDispatch, NativeD3D12QueueKeepsVTableSlotsInD3D12Core) {
    FlowGame game(CurrentTestName());
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    ASSERT_EQ(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), S_OK);
    ASSERT_EQ(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), S_OK);
    ASSERT_EQ(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), S_OK);

    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> native;
    ASSERT_EQ(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&native)), S_OK);

    auto isNativeD3D12Address = [](const void* addr) {
        HMODULE mod = nullptr;
        if (!addr ||
            !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCSTR>(addr), &mod) ||
            !mod) {
            return false;
        }
        char path[MAX_PATH] = {};
        if (!GetModuleFileNameA(mod, path, MAX_PATH))
            return false;
        const char* base = std::strrchr(path, '\\');
        base = base ? base + 1 : path;
        return _stricmp(base, "d3d12core.dll") == 0 || _stricmp(base, "d3d12.dll") == 0;
    };

    void** vtbl = *reinterpret_cast<void***>(native.Get());
    ASSERT_NE(vtbl, nullptr);
    void* const eclBefore = vtbl[10];
    void* const signalBefore = vtbl[14];
    ASSERT_TRUE(isNativeD3D12Address(static_cast<const void*>(vtbl)))
        << "expected native D3D12Core.dll command queue vtable";
    ASSERT_TRUE(isNativeD3D12Address(eclBefore)) << "expected native ExecuteCommandLists slot";
    ASSERT_TRUE(isNativeD3D12Address(signalBefore)) << "expected native Signal slot";

    game.TrackQueue(native.Get());

    // VTable slots [10] and [14] must remain unchanged inside D3D12Core.dll / d3d12.dll.
    EXPECT_EQ(vtbl[10], eclBefore);
    EXPECT_EQ(vtbl[14], signalBefore);
    EXPECT_TRUE(isNativeD3D12Address(vtbl[10]));
    EXPECT_TRUE(isNativeD3D12Address(vtbl[14]));

    // Resolved original must be CE's callable inline trampoline (distinct from the inline-hooked
    // native entry), so forwarding never recurses back into DetourExecuteCommandLists.
    void* const resolved = game.QueueOriginal(native.Get());
    ASSERT_NE(resolved, nullptr);
    EXPECT_NE(resolved, eclBefore);

    // Real submission through both the virtual call (hitting CE's inline detour) and the resolved
    // trampoline must complete cleanly on the GPU.
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ASSERT_EQ(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), S_OK);
    ASSERT_EQ(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&list)),
              S_OK);
    ASSERT_EQ(list->Close(), S_OK);
    ID3D12CommandList* lists[] = {list.Get()};
    native->ExecuteCommandLists(1, lists);
    game.ForwardQueue(native.Get());

    // Resetting queue bindings must still recover the exact trampoline rather than looping into
    // the inline-hooked native entry.
    game.ResetQueueBindings();
    EXPECT_EQ(game.QueueOriginal(native.Get()), resolved);
    game.ForwardQueue(native.Get());
    EXPECT_EQ(vtbl[10], eclBefore);

    ComPtr<ID3D12Fence> fence;
    ASSERT_EQ(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), S_OK);
    const HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ASSERT_NE(done, nullptr);
    ASSERT_EQ(native->Signal(fence.Get(), 1), S_OK);
    ASSERT_EQ(fence->SetEventOnCompletion(1, done), S_OK);
    const DWORD wait = WaitForSingleObject(done, 5000);
    CloseHandle(done);
    ASSERT_EQ(wait, WAIT_OBJECT_0);
    EXPECT_EQ(device->GetDeviceRemovedReason(), S_OK);
}

}  // namespace
