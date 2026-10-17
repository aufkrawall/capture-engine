#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "hook/overlay/overlay_compat.h"
#include "hook/present/dxgi_shared_internal.h"
#include "source_fragment_reader.h"

namespace DXGIShared {
extern std::atomic<int> g_TestHandleDX11ResizeBeginCalls;
}

// BioShock Infinite x86 + Steam (20261017_003122): CE returned its CWrapDXGISwapChain from a
// D3D11 create below Steam's factory hook, Steam tracked the proxy, and its Present hook - given
// the real object - never drew. The real object may go back only when CE sees Present, Present1
// and the chain's resizes below the foreign chain.
namespace {
using ce::overlay_compat::ShouldPreserveD3D11SwapchainIdentityBelowForeignPresentChain;

// A swapchain-shaped COM object: the slots the code under test calls, every other slot refuses.
// Unused slots share one function, which the x64 calling convention allows.
struct FakeChain;
HRESULT STDMETHODCALLTYPE Refuse(FakeChain*) {
    return E_NOTIMPL;
}

struct FakeDevice {
    void** vtable;
    ULONG refs = 1;
};
HRESULT STDMETHODCALLTYPE DeviceQueryInterface(FakeDevice*, REFIID, void** out) {
    *out = nullptr;
    return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE DeviceAddRef(FakeDevice* self) {
    return ++self->refs;
}
ULONG STDMETHODCALLTYPE DeviceRelease(FakeDevice* self) {
    return --self->refs;
}
void* g_deviceVTable[3] = {reinterpret_cast<void*>(&DeviceQueryInterface), reinterpret_cast<void*>(&DeviceAddRef),
                           reinterpret_cast<void*>(&DeviceRelease)};

struct FakeChain {
    void** vtable;
    void* slots[40];
    bool isSwapChain3 = false;
    bool d3d11Device = false;
    bool d3d12Device = false;
    FakeDevice device{g_deviceVTable};

    IDXGISwapChain* AsSwapChain() {
        return reinterpret_cast<IDXGISwapChain*>(this);
    }
};

HRESULT STDMETHODCALLTYPE ChainQueryInterface(FakeChain* self, REFIID riid, void** out) {
    *out = nullptr;
    if (self->isSwapChain3 && riid == __uuidof(IDXGISwapChain3)) {
        *out = self;
        return S_OK;
    }
    return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE ChainAddRef(FakeChain*) {
    return 2;
}
ULONG STDMETHODCALLTYPE ChainRelease(FakeChain*) {
    return 1;
}
HRESULT STDMETHODCALLTYPE ChainGetDevice(FakeChain* self, REFIID riid, void** out) {
    *out = nullptr;
    if ((self->d3d11Device && riid == __uuidof(ID3D11Device)) ||
        (self->d3d12Device && riid == __uuidof(ID3D12Device))) {
        ++self->device.refs;
        *out = &self->device;
        return S_OK;
    }
    return E_NOINTERFACE;
}

void InitChain(FakeChain& chain) {
    for (void*& slot : chain.slots) {
        slot = reinterpret_cast<void*>(&Refuse);
    }
    chain.slots[0] = reinterpret_cast<void*>(&ChainQueryInterface);
    chain.slots[1] = reinterpret_cast<void*>(&ChainAddRef);
    chain.slots[2] = reinterpret_cast<void*>(&ChainRelease);
    chain.slots[7] = reinterpret_cast<void*>(&ChainGetDevice);
    chain.vtable = chain.slots;
}

// Stand-ins for the dxgi functions the body hooks patched.
HRESULT STDMETHODCALLTYPE FakeResizeBuffers(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT) {
    return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeResizeBuffers1(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*,
                                             IUnknown* const*) {
    return S_OK;
}

int g_releasesSeenByForward = -1;
HRESULT STDMETHODCALLTYPE RecordingResizeTrampoline(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT) {
    g_releasesSeenByForward = DXGIShared::g_TestHandleDX11ResizeBeginCalls.load();
    return S_OK;
}

class ReconcileHookState {
public:
    ReconcileHookState() {
        DXGIShared::dxgi_shared_oResizeBuffersReconcile = &RecordingResizeTrampoline;
        DXGIShared::dxgi_shared_oResizeBuffers1Reconcile = &FakeResizeBuffers1;
        DXGIShared::dxgi_shared_s_resizeReconcileTarget = reinterpret_cast<void*>(&FakeResizeBuffers);
        DXGIShared::dxgi_shared_s_resize1ReconcileTarget = reinterpret_cast<void*>(&FakeResizeBuffers1);
    }
    ~ReconcileHookState() {
        DXGIShared::dxgi_shared_oResizeBuffersReconcile = nullptr;
        DXGIShared::dxgi_shared_oResizeBuffers1Reconcile = nullptr;
        DXGIShared::dxgi_shared_s_resizeReconcileTarget = nullptr;
        DXGIShared::dxgi_shared_s_resize1ReconcileTarget = nullptr;
    }
};

TEST(D3D11SwapchainIdentityTest, RealObjectNeedsEveryCoverageBelowTheChain) {
    EXPECT_TRUE(ShouldPreserveD3D11SwapchainIdentityBelowForeignPresentChain(true, true, 1, true, true));
    EXPECT_TRUE(ShouldPreserveD3D11SwapchainIdentityBelowForeignPresentChain(true, true, 3, true, true));
    // Without an overlay there is nobody above CE to mislead; the wrapper stays.
    EXPECT_FALSE(ShouldPreserveD3D11SwapchainIdentityBelowForeignPresentChain(true, true, 0, true, true));
    // A D3D12 chain has its own rule.
    EXPECT_FALSE(ShouldPreserveD3D11SwapchainIdentityBelowForeignPresentChain(false, true, 1, true, true));
    // Present1 missing (BioShock's first run of this fix): the game could present unseen.
    EXPECT_FALSE(ShouldPreserveD3D11SwapchainIdentityBelowForeignPresentChain(true, false, 1, true, true));
    // A present interposer's app-facing chain: the body hook sees another chain.
    EXPECT_FALSE(ShouldPreserveD3D11SwapchainIdentityBelowForeignPresentChain(true, true, 1, false, true));
    // No resize site: CE's retained back-buffer view would make every resize fail.
    EXPECT_FALSE(ShouldPreserveD3D11SwapchainIdentityBelowForeignPresentChain(true, true, 1, true, false));
}

TEST(D3D11SwapchainIdentityTest, ResizeCoverageFollowsTheFunctionsTheHooksPatched) {
    FakeChain chain;
    InitChain(chain);
    chain.slots[13] = reinterpret_cast<void*>(&FakeResizeBuffers);
    EXPECT_FALSE(DXGIShared::IsSwapchainResizeCoveredByReconciliationHooks(chain.AsSwapChain()))
        << "no hooks installed";

    ReconcileHookState hooks;
    EXPECT_TRUE(DXGIShared::IsSwapchainResizeCoveredByReconciliationHooks(chain.AsSwapChain()))
        << "a chain without ResizeBuffers1 only needs ResizeBuffers";
    chain.isSwapChain3 = true;
    chain.slots[39] = reinterpret_cast<void*>(&Refuse);
    EXPECT_FALSE(DXGIShared::IsSwapchainResizeCoveredByReconciliationHooks(chain.AsSwapChain()))
        << "ResizeBuffers1 leads to a function CE did not patch";
    chain.slots[39] = reinterpret_cast<void*>(&FakeResizeBuffers1);
    EXPECT_TRUE(DXGIShared::IsSwapchainResizeCoveredByReconciliationHooks(chain.AsSwapChain()));
    chain.slots[13] = reinterpret_cast<void*>(&Refuse);
    EXPECT_FALSE(DXGIShared::IsSwapchainResizeCoveredByReconciliationHooks(chain.AsSwapChain()));
}

TEST(D3D11SwapchainIdentityTest, ReconcileResizeReleasesD3D11ViewsBeforeForwarding) {
    ReconcileHookState hooks;
    FakeChain d3d11Chain;
    InitChain(d3d11Chain);
    d3d11Chain.d3d11Device = true;
    const int before = DXGIShared::g_TestHandleDX11ResizeBeginCalls.load();
    g_releasesSeenByForward = -1;
    EXPECT_EQ(DXGIShared::DetourResizeBuffersReconcileOnly(d3d11Chain.AsSwapChain(), 0, 640, 480,
                                                           DXGI_FORMAT_UNKNOWN, 0),
              S_OK);
    EXPECT_EQ(DXGIShared::g_TestHandleDX11ResizeBeginCalls.load(), before + 1);
    EXPECT_EQ(g_releasesSeenByForward, before + 1) << "the views must be gone before DXGI sees the resize";
    EXPECT_EQ(d3d11Chain.device.refs, 1u) << "the API probe must not leak a device reference";

    // A D3D12 chain holds no D3D11 views of CE's; its capture release is PrepareD3D12Resize's job.
    FakeChain d3d12Chain;
    InitChain(d3d12Chain);
    d3d12Chain.d3d12Device = true;
    const int afterD3D11 = DXGIShared::g_TestHandleDX11ResizeBeginCalls.load();
    EXPECT_EQ(DXGIShared::DetourResizeBuffersReconcileOnly(d3d12Chain.AsSwapChain(), 0, 640, 480,
                                                           DXGI_FORMAT_UNKNOWN, 0),
              S_OK);
    EXPECT_EQ(DXGIShared::g_TestHandleDX11ResizeBeginCalls.load(), afterD3D11);
}

std::string ReadSource(const char* relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

// Every create path that can hand out CE's proxy asks the D3D11 rule before it wraps.
TEST(D3D11SwapchainIdentityTest, EveryWrappingCreatePathAsksTheRuleFirst) {
    const std::string create = ReadSource("hook/d3d12/dx12_hook_swapchain_create.cpp");
    for (const char* path : {"\"DetourCreateSwapChainGlobal\")", "\"DetourCreateSwapChainForHwndGlobal\")"}) {
        const size_t rule = create.find("ShouldReturnRealD3D11SwapchainBelowForeignChain(");
        ASSERT_NE(rule, std::string::npos);
        const size_t call = create.find(path);
        ASSERT_NE(call, std::string::npos) << path;
        const size_t wrap = create.find("new CWrapDXGISwapChain(", call);
        ASSERT_NE(wrap, std::string::npos) << path;
        EXPECT_LT(call, wrap) << path;
    }
    const std::string factory = ReadSource("hook/wrappers/dxgi_factory_wrap.cpp");
    const size_t assign = factory.find("void AssignCreatedSwapchain(");
    ASSERT_NE(assign, std::string::npos);
    const size_t rule = factory.find("ShouldReturnRealD3D11SwapchainBelowForeignChain(", assign);
    const size_t wrap = factory.find("new CWrapDXGISwapChain(pReal, pDevice)", assign);
    ASSERT_NE(rule, std::string::npos);
    ASSERT_NE(wrap, std::string::npos);
    EXPECT_LT(rule, wrap);

    // The rule only installs resize hooks for a chain whose Present it already sees, so CE's own
    // proxy (Present in capture_hook) never logs a refused resize install.
    const std::string identity = ReadSource("hook/present/dxgi_swapchain_identity.cpp");
    const size_t gate = identity.find("if (presentMethodsBelowChain && presentCovered) {");
    const size_t install = identity.find("InstallResizeReconciliationHooks(pSwapChain");
    ASSERT_NE(gate, std::string::npos);
    ASSERT_NE(install, std::string::npos);
    EXPECT_LT(gate, install);
}

}  // namespace
