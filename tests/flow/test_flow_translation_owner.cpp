#include "tests/flow/flow_test_support.h"
#include "tests/flow/fakes/fake_dxgi_forwarders.h"
#include <d3d11.h>

namespace {
using namespace ce::flow;

__attribute__((noinline)) HRESULT PresentFrontend(IDXGISwapChain1* frontend, bool present1) {
    DXGI_PRESENT_PARAMETERS parameters{};
    return present1 ? frontend->Present1(0, 0, &parameters) : frontend->Present(0, 0);
}

class ApiFrontend final : public fake::ForwardingSwapChain {
public:
    ApiFrontend(IDXGISwapChain4* native, IUnknown* apiDevice, bool forwardData = true)
        : ForwardingSwapChain(native), device_(apiDevice), forwardData_(forwardData) {}
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** out) override {
        ++deviceQueries;
        return device_ ? device_->QueryInterface(iid, out) : real_->GetDevice(iid, out);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* out) override {
        return forwardData_ ? real_->GetPrivateData(guid, size, out) : DXGI_ERROR_NOT_FOUND;
    }
    unsigned deviceQueries = 0;
private:
    ComPtr<IUnknown> device_;
    bool forwardData_;
};

ComPtr<ID3D11Device> CreateFrontendDevice() {
    const auto module = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return {};
    const auto create = reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module, "D3D11CreateDevice"));
    ComPtr<ID3D11Device> device;
    if (create) create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                       D3D11_SDK_VERSION, &device, nullptr, nullptr);
    FreeLibrary(module);
    return device;
}

TEST(FlowTranslationOwner, UnscopedFrontendDelegatesBeforeItsFirstPresentAndCachesTheDecision) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ComPtr<IDXGISwapChain4> native;
    ASSERT_EQ(game.RetainUnderlyingGameSwapchain().As(&native), S_OK);
    const auto device = CreateFrontendDevice();
    ASSERT_NE(device, nullptr);
    ComPtr<ApiFrontend> frontend;
    frontend.Attach(new ApiFrontend(native.Get(), device.Get()));
    game.ReleasePresentVTableHooks();  // move the physical vtable interception above the native inline hook
    ASSERT_TRUE(game.InstallPresentVTableHooks(frontend.Get()));
    const auto hook = GetModuleHandleW(L"capture_hook_x64.dll");
    const auto capture = reinterpret_cast<void (*)(bool)>(GetProcAddress(hook, "CEFlow_SetInjectCapture"));
    const auto frames = reinterpret_cast<uint32_t (*)()>(GetProcAddress(hook, "CEFlow_InjectCaptureFrames"));
    const auto advance = reinterpret_cast<CEFlow_AdvanceClock_t>(GetProcAddress(hook, "CEFlow_AdvanceClock"));
    const auto owns = reinterpret_cast<bool (*)(IDXGISwapChain*)>(GetProcAddress(hook, "CEFlow_HasTranslationOwner"));
    ASSERT_NE(capture, nullptr);
    ASSERT_NE(frames, nullptr);
    ASSERT_NE(advance, nullptr);
    ASSERT_NE(owns, nullptr);
    capture(true);
    const auto before = game.Coverage();
    advance(FlowGame::kFrameIntervalMicroseconds);
    ASSERT_EQ(PresentFrontend(frontend.Get(), false), S_OK);
    const unsigned queries = frontend->deviceQueries;
    EXPECT_EQ(queries, 2u);  // rejects DX12, proves DX11; no subsequent device queries
    advance(FlowGame::kFrameIntervalMicroseconds);
    ASSERT_EQ(PresentFrontend(frontend.Get(), true), S_OK);
    EXPECT_TRUE(owns(frontend.Get()));
    EXPECT_FALSE(owns(native.Get()));
    EXPECT_EQ(frontend->deviceQueries, queries);
    const auto after = game.Coverage();
    EXPECT_EQ(after.presents - before.presents, 2u);
    EXPECT_EQ(after.uncovered, before.uncovered);
    EXPECT_EQ(after.doubleDraws, before.doubleDraws);
    EXPECT_EQ(frames(), 2u);
    capture(false);
    ExpectNoDebugLayerErrors();
}

TEST(FlowTranslationOwner, NativeApiProxiesAndUnforwardedDataKeepTheirOwnership) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ComPtr<IDXGISwapChain4> native;
    ASSERT_EQ(game.RetainUnderlyingGameSwapchain().As(&native), S_OK);
    const auto device = CreateFrontendDevice();
    ASSERT_NE(device, nullptr);
    const auto owns = reinterpret_cast<bool (*)(IDXGISwapChain*)>(GetProcAddress(
        GetModuleHandleW(L"capture_hook_x64.dll"), "CEFlow_HasTranslationOwner"));
    ASSERT_NE(owns, nullptr);
    ComPtr<ApiFrontend> nativeProxy;
    nativeProxy.Attach(new ApiFrontend(native.Get(), nullptr));
    EXPECT_FALSE(owns(nativeProxy.Get()));
    const auto queries = nativeProxy->deviceQueries;
    EXPECT_EQ(queries, 1u);
    EXPECT_FALSE(owns(nativeProxy.Get()));
    EXPECT_EQ(nativeProxy->deviceQueries, queries);
    ComPtr<ApiFrontend> unforwarded;
    unforwarded.Attach(new ApiFrontend(native.Get(), device.Get(), false));
    EXPECT_FALSE(owns(unforwarded.Get()));
    EXPECT_EQ(unforwarded->deviceQueries, 0u);
    EXPECT_FALSE(owns(native.Get()));
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowTranslationOwner, ForwardedPrivateDataDelegatesOnlyTheFrontendPresentAndPresent1) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ComPtr<IDXGISwapChain4> native;
    ASSERT_EQ(game.RetainUnderlyingGameSwapchain().As(&native), S_OK);
    ComPtr<fake::ForwardingSwapChain> frontend;
    frontend.Attach(new fake::ForwardingSwapChain(native.Get()));
    ASSERT_TRUE(game.InstallPresentVTableHooks(frontend.Get()));
    using PublishOwner = bool (*)(IDXGISwapChain*, IDXGISwapChain*);
    auto publish = reinterpret_cast<PublishOwner>(GetProcAddress(GetModuleHandleW(L"capture_hook_x64.dll"),
                                                                "CEFlow_PublishTranslationOwner"));
    ASSERT_NE(publish, nullptr);
    ASSERT_TRUE(publish(frontend.Get(), native.Get()));
    const auto hook = GetModuleHandleW(L"capture_hook_x64.dll");
    const auto capture = reinterpret_cast<void (*)(bool)>(GetProcAddress(hook, "CEFlow_SetInjectCapture"));
    const auto frames = reinterpret_cast<uint32_t (*)()>(GetProcAddress(hook, "CEFlow_InjectCaptureFrames"));
    const auto advance = reinterpret_cast<CEFlow_AdvanceClock_t>(GetProcAddress(hook, "CEFlow_AdvanceClock"));
    ASSERT_NE(capture, nullptr);
    ASSERT_NE(frames, nullptr);
    ASSERT_NE(advance, nullptr);
    capture(true);
    const auto before = game.Coverage();
    advance(FlowGame::kFrameIntervalMicroseconds);
    ASSERT_EQ(frontend->Present(0, 0), S_OK);
    DXGI_PRESENT_PARAMETERS parameters{};
    advance(FlowGame::kFrameIntervalMicroseconds);
    ASSERT_EQ(frontend->Present1(0, 0, &parameters), S_OK);
    const auto after = game.Coverage();
    EXPECT_EQ(after.presents - before.presents, 2u);
    EXPECT_EQ(after.uncovered, before.uncovered);
    EXPECT_EQ(after.doubleDraws, before.doubleDraws);
    EXPECT_EQ(frames(), 2u);
    capture(false);
    ExpectNoDebugLayerErrors();
}
}  // namespace
