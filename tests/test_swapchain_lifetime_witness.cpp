#include <gtest/gtest.h>

#include <dxgi.h>

#include <map>
#include <new>

#include "hook/present/swapchain_lifetime_witness.h"

namespace witness = ce::swapchain_lifetime_witness;

namespace {

struct GuidLess {
    bool operator()(const GUID& a, const GUID& b) const { return memcmp(&a, &b, sizeof(GUID)) < 0; }
};

// A swapchain that stores private-data interfaces the way DXGI does: one reference per stored interface, released
// when replaced or when the object is destroyed. Only the IUnknown/IDXGIObject surface is ever called.
class FakeSwapChain : public IDXGISwapChain {
public:
    ~FakeSwapChain() { DestroyPrivateData(); }

    void DestroyPrivateData() {
        auto data = std::move(privateData_);
        privateData_.clear();
        for (auto& [guid, object] : data) {
            object->Release();
        }
    }
    size_t StoredInterfaces() const { return privateData_.size(); }

    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override {
        ++setPrivateDataInterfaceCalls;
        if (refusePrivateData) {
            return E_OUTOFMEMORY;
        }
        auto* object = const_cast<IUnknown*>(data);
        if (object) {
            object->AddRef();
        }
        const auto it = privateData_.find(guid);
        IUnknown* previous = it != privateData_.end() ? it->second : nullptr;
        if (object) {
            privateData_[guid] = object;
        } else if (it != privateData_.end()) {
            privateData_.erase(it);
        }
        if (previous) {
            previous->Release();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Present(UINT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT, REFIID, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL, IDXGIOutput*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL*, IDXGIOutput**) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT, UINT, UINT, DXGI_FORMAT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT*) override { return E_NOTIMPL; }

    ULONG refs = 1;
    int setPrivateDataInterfaceCalls = 0;
    bool refusePrivateData = false;

private:
    std::map<GUID, IUnknown*, GuidLess> privateData_;
};

HWND TestWindow(uintptr_t value) {
    return reinterpret_cast<HWND>(value);
}

class SwapchainLifetimeWitnessTest : public ::testing::Test {
protected:
    void SetUp() override { witness::ResetForTesting(); }
    void TearDown() override { witness::ResetForTesting(); }
};

}  // namespace

// The witness must never pin what it watches: a reference on the old chain is exactly what makes DXGI refuse the
// replacement create (logs/gtacrash, FidelityFX's create after DLSS-G).
TEST_F(SwapchainLifetimeWitnessTest, WatchingTakesNoReferenceOnTheChain) {
    FakeSwapChain chain;
    ASSERT_TRUE(witness::Watch(&chain, TestWindow(0x1000)));
    EXPECT_EQ(chain.refs, 1u);
    EXPECT_EQ(chain.StoredInterfaces(), 1u);
    const witness::ChainReport report = witness::Describe(&chain);
    EXPECT_EQ(report.state, witness::ChainState::kAlive);
    EXPECT_EQ(report.window, TestWindow(0x1000));
}

TEST_F(SwapchainLifetimeWitnessTest, TheChainsDestructionIsObservedOnTheReleasingThread) {
    FakeSwapChain chain;
    ASSERT_TRUE(witness::Watch(&chain, TestWindow(0x1000)));
    EXPECT_EQ(witness::LogWindowChains(TestWindow(0x1000), "unit-test"), 1u);

    chain.DestroyPrivateData();  // what DXGI does when the chain's last reference goes
    const witness::ChainReport report = witness::Describe(&chain);
    EXPECT_EQ(report.state, witness::ChainState::kDestroyed);
    EXPECT_EQ(report.destroyingThread, GetCurrentThreadId());
    EXPECT_GE(report.destroyedAtMs, report.watchedAtMs);
    EXPECT_NE(report.destroyedBy[0], '\0');
    EXPECT_EQ(witness::LogWindowChains(TestWindow(0x1000), "unit-test"), 0u);
}

// ProcessFrame asks for its presenting chain every frame; only the first call attaches a witness.
TEST_F(SwapchainLifetimeWitnessTest, WatchingALiveChainAgainAttachesNothingNew) {
    FakeSwapChain chain;
    ASSERT_TRUE(witness::Watch(&chain, TestWindow(0x1000)));
    for (int frame = 0; frame < 5; ++frame) {
        EXPECT_TRUE(witness::Watch(&chain, TestWindow(0x1000)));
    }
    EXPECT_EQ(chain.setPrivateDataInterfaceCalls, 1);
    EXPECT_EQ(witness::Describe(&chain).state, witness::ChainState::kAlive);
}

// Nothing removes a dead chain from CE's per-window list, so a new chain can arrive at its address. It must get
// its own witness, and the old record must not answer for it.
TEST_F(SwapchainLifetimeWitnessTest, ANewChainAtADeadChainsAddressIsWitnessedAgain) {
    alignas(FakeSwapChain) unsigned char storage[sizeof(FakeSwapChain)];
    auto* first = new (storage) FakeSwapChain();
    ASSERT_TRUE(witness::Watch(first, TestWindow(0x1000)));
    first->DestroyPrivateData();
    ASSERT_EQ(witness::Describe(first).state, witness::ChainState::kDestroyed);
    first->~FakeSwapChain();

    auto* second = new (storage) FakeSwapChain();
    ASSERT_EQ(static_cast<void*>(second), static_cast<void*>(first));
    EXPECT_TRUE(witness::Watch(second, TestWindow(0x2000)));
    EXPECT_EQ(second->setPrivateDataInterfaceCalls, 1);
    const witness::ChainReport report = witness::Describe(second);
    EXPECT_EQ(report.state, witness::ChainState::kAlive);
    EXPECT_EQ(report.window, TestWindow(0x2000));
    second->~FakeSwapChain();
}

// A witness replaced on the same object (a record evicted while its chain lived) reports its own generation's
// destruction; the live chain's record must stay alive.
TEST_F(SwapchainLifetimeWitnessTest, AReplacedWitnessDoesNotReportTheLiveChainDestroyed) {
    FakeSwapChain chain;
    ASSERT_TRUE(witness::Watch(&chain, TestWindow(0x1000)));
    witness::ResetForTesting();  // the record is gone, the attached witness is not
    ASSERT_TRUE(witness::Watch(&chain, TestWindow(0x1000)));
    EXPECT_EQ(chain.setPrivateDataInterfaceCalls, 2);
    EXPECT_EQ(chain.StoredInterfaces(), 1u);
    EXPECT_EQ(witness::Describe(&chain).state, witness::ChainState::kAlive);
}

TEST_F(SwapchainLifetimeWitnessTest, ARefusedWitnessLeavesNoRecord) {
    FakeSwapChain chain;
    chain.refusePrivateData = true;
    EXPECT_FALSE(witness::Watch(&chain, TestWindow(0x1000)));
    EXPECT_EQ(witness::Describe(&chain).state, witness::ChainState::kUnwatched);
    EXPECT_EQ(witness::LogWindowChains(TestWindow(0x1000), "unit-test"), 0u);
}

TEST_F(SwapchainLifetimeWitnessTest, ChainsAreReportedPerWindow) {
    FakeSwapChain gameAlive;
    FakeSwapChain gameDestroyed;
    FakeSwapChain otherWindow;
    ASSERT_TRUE(witness::Watch(&gameAlive, TestWindow(0x1000)));
    ASSERT_TRUE(witness::Watch(&gameDestroyed, TestWindow(0x1000)));
    ASSERT_TRUE(witness::Watch(&otherWindow, TestWindow(0x2000)));
    gameDestroyed.DestroyPrivateData();
    EXPECT_EQ(witness::LogWindowChains(TestWindow(0x1000), "unit-test"), 1u);
    EXPECT_EQ(witness::LogWindowChains(TestWindow(0x2000), "unit-test"), 1u);
    EXPECT_EQ(witness::LogWindowChains(TestWindow(0x3000), "unit-test"), 0u);
}

// A destruction stack, top first: the witness's own frames, DXGI's release machinery, then whoever released the
// chain's last reference.
TEST(SwapchainLifetimeWitnessReleaser, TheFirstFrameAfterTheReleaseMachineryNamesTheReleaser) {
    const char* streamline[] = {"capture_hook_x64.dll", "dxgi.dll", "dxgi.dll", "sl.interposer.dll", "GTA5.exe"};
    EXPECT_EQ(witness::SelectReleaserFrame(streamline, 5), 3);

    // CE itself can drop the last reference; it is named then, not skipped.
    const char* ceRelease[] = {"capture_hook_x64.dll", "dxgi.dll", "capture_hook_x64.dll", "GTA5.exe"};
    EXPECT_EQ(witness::SelectReleaserFrame(ceRelease, 4), 2);

    const char* systemOnly[] = {"capture_hook_x64.dll", "dxgi.dll", "KERNELBASE.dll", "ntdll.dll"};
    EXPECT_EQ(witness::SelectReleaserFrame(systemOnly, 4), -1);

    const char* neverEnteredDxgi[] = {"capture_hook_x64.dll", "unit_tests.exe"};
    EXPECT_EQ(witness::SelectReleaserFrame(neverEnteredDxgi, 2), -1);
    EXPECT_EQ(witness::SelectReleaserFrame(nullptr, 3), -1);
}

TEST(SwapchainLifetimeWitnessReleaser, ReleaseMachineryIsMatchedByImageNameIgnoringCase) {
    EXPECT_TRUE(witness::IsReleaseMachineryModule("dxgi.dll"));
    EXPECT_TRUE(witness::IsReleaseMachineryModule("D3D12Core.dll"));
    EXPECT_TRUE(witness::IsReleaseMachineryModule("KERNELBASE.dll"));
    EXPECT_TRUE(witness::IsReleaseMachineryModule("VCRUNTIME140.dll"));
    EXPECT_FALSE(witness::IsReleaseMachineryModule("sl.interposer.dll"));
    EXPECT_FALSE(witness::IsReleaseMachineryModule("EOSOVH-Win64-Shipping.dll"));
    EXPECT_FALSE(witness::IsReleaseMachineryModule("dxgi.dll.bak"));
    EXPECT_FALSE(witness::IsReleaseMachineryModule(""));
    EXPECT_FALSE(witness::IsReleaseMachineryModule(nullptr));
}
