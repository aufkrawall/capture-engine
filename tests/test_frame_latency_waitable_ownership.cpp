#include <gtest/gtest.h>

#include <windows.h>
#include <dxgi.h>

#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "hook/d3d12/dx12_overlay_policy.h"
#include "hook/present/present_pacing_policy.h"
#include "hook/present/swapchain_flag_apply.h"
#include "source_fragment_reader.h"

// GTA V Enhanced, session installed/captureengine/logs/20261002_060100: the
// first DLSS-G enable after a save load froze the game for about 5 s and wrote a
// freeze dump.
//
// sl.dlss_g created its swapchain with DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_
// WAITABLE_OBJECT itself (flags 0x842); CE had left that descriptor alone. CE's
// `backbuffer_count` pacing still waited on the object before the first
// present, after SetMaximumFrameLatency(1). The object's creator already waits
// on it once per frame, so CE's wait could only be satisfied by the present it
// was blocking: it ran into its 1000 ms ceiling. The pure-DLSS startup-stall
// detector then measured that second from the last ProcessFrame (which runs
// before the forward), read "dormant=1031ms" 26 ms after the present returned,
// and requested a dump that froze the game another 3.9 s.

namespace {

using ce::present_pacing_policy::ShouldWaitForFlipQueueRoom;
using ce::swapchain_flag_policy::DidCeAddFrameLatencyWaitable;
using ce::swapchain_flag_policy::NoteCeAddedFrameLatencyWaitable;

// Just enough of IDXGIObject for DXGI private data, which is all the tag uses.
class FakeDxgiObject final : public IDXGIObject {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject)) {
            *out = static_cast<IDXGIObject*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override {
        const auto* bytes = static_cast<const unsigned char*>(data);
        data_[Key(guid)] = std::vector<unsigned char>(bytes, bytes + size);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override {
        const auto it = data_.find(Key(guid));
        if (it == data_.end())
            return DXGI_ERROR_NOT_FOUND;
        if (*size < it->second.size())
            return DXGI_ERROR_MORE_DATA;
        *size = static_cast<UINT>(it->second.size());
        std::memcpy(data, it->second.data(), it->second.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void**) override { return E_NOTIMPL; }

    ULONG refs() const { return refs_; }

private:
    static std::string Key(REFGUID guid) { return std::string(reinterpret_cast<const char*>(&guid), sizeof(GUID)); }

    ULONG refs_ = 1;
    std::map<std::string, std::vector<unsigned char>> data_;
};

// A swapchain whose only behavior is its creation flags and DXGI private data.
class FakeFlagSwapChain final : public IDXGISwapChain {
public:
    explicit FakeFlagSwapChain(UINT flags) : flags_(flags) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGISwapChain)) {
            *out = static_cast<IDXGISwapChain*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override {
        return data_.SetPrivateData(guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override {
        return data_.GetPrivateData(guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Present(UINT, UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT, REFIID, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL, IDXGIOutput*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL*, IDXGIOutput**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* desc) override {
        if (!desc || descFails_)
            return E_FAIL;
        *desc = {};
        desc->Flags = flags_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT, UINT, UINT, DXGI_FORMAT, UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT*) override { return E_NOTIMPL; }

    void FailDesc() { descFails_ = true; }
    ULONG refs() const { return refs_; }

private:
    UINT flags_;
    bool descFails_ = false;
    ULONG refs_ = 1;
    FakeDxgiObject data_;
};

std::string ReadProjectSource(const char* relativePath) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / relativePath;
    if (!fs::exists(source))
        return {};
    return ce::test_source::ReadFile(source);
}

size_t CountOccurrences(const std::string& text, const char* needle) {
    size_t count = 0;
    for (size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + 1))
        ++count;
    return count;
}

}  // namespace

TEST(FrameLatencyWaitableOwnershipTest, CreatorOwnedWaitableIsNeverPacedByCe) {
    // Every other reason to pace holds; only the ownership differs.
    EXPECT_FALSE(ShouldWaitForFlipQueueRoom(/*backbufferCountOverrideActive=*/true,
                                            /*vulkanLayerOwnsPresentation=*/false,
                                            /*pacingLatchedOff=*/false, /*ceAddedWaitableObject=*/false));
    EXPECT_TRUE(ShouldWaitForFlipQueueRoom(/*backbufferCountOverrideActive=*/true,
                                           /*vulkanLayerOwnsPresentation=*/false,
                                           /*pacingLatchedOff=*/false, /*ceAddedWaitableObject=*/true));
}

TEST(FrameLatencyWaitableOwnershipTest, SecondWaiterTakesTheCountTheOwnerNeeds) {
    // A frame-latency object at SetMaximumFrameLatency(1) behaves as a
    // semaphore that one retired present refills by one. The owner's per-frame
    // wait takes that count, so a second waiter before the present has nothing
    // left: the GTA stall, with a zero timeout instead of CE's 1000 ms.
    HANDLE latencyObject = CreateSemaphoreW(nullptr, 1, 1, nullptr);
    ASSERT_NE(latencyObject, nullptr);
    EXPECT_EQ(WaitForSingleObject(latencyObject, 0), WAIT_OBJECT_0) << "the owner's own wait";
    EXPECT_EQ(WaitForSingleObject(latencyObject, 0), static_cast<DWORD>(WAIT_TIMEOUT)) << "a second waiter";
    CloseHandle(latencyObject);
}

TEST(FrameLatencyWaitableOwnershipTest, TagMarksOnlySwapchainsWhoseWaitableCeAdded) {
    FakeDxgiObject ceAdded;
    NoteCeAddedFrameLatencyWaitable(/*ceAddedWaitable=*/true, S_OK, &ceAdded, "unit-test");
    EXPECT_TRUE(DidCeAddFrameLatencyWaitable(&ceAdded));
    EXPECT_EQ(ceAdded.refs(), 1u) << "tagging must not leak a reference";

    // The sl.dlss_g create: the flag was the creator's own.
    FakeDxgiObject creatorOwned;
    NoteCeAddedFrameLatencyWaitable(/*ceAddedWaitable=*/false, S_OK, &creatorOwned, "unit-test");
    EXPECT_FALSE(DidCeAddFrameLatencyWaitable(&creatorOwned));

    FakeDxgiObject failedCreate;
    NoteCeAddedFrameLatencyWaitable(/*ceAddedWaitable=*/true, E_INVALIDARG, &failedCreate, "unit-test");
    EXPECT_FALSE(DidCeAddFrameLatencyWaitable(&failedCreate));

    NoteCeAddedFrameLatencyWaitable(/*ceAddedWaitable=*/true, S_OK, nullptr, "unit-test");
    EXPECT_FALSE(DidCeAddFrameLatencyWaitable(nullptr));
}

// Strange Brigade (DX12) session 20261019_014848: Flags=0x802, CE withheld the waitable, then called
// SetMaximumFrameLatency anyway (DXGI_ERROR_INVALID_CALL) and logged every skipped pacing wait as
// "requested by the application".
TEST(FrameLatencyWaitableOwnershipTest, ClassifiesTheWaitableFromTheRealCreationFlags) {
    using ce::swapchain_flag_policy::CanSetSwapChainMaximumFrameLatency;
    using ce::swapchain_flag_policy::ClassifyFrameLatencyWaitable;
    using Owner = ce::swapchain_flag_policy::FrameLatencyWaitableOwner;
    EXPECT_EQ(ClassifyFrameLatencyWaitable(0x802, false), Owner::None);
    EXPECT_EQ(ClassifyFrameLatencyWaitable(0x802, true), Owner::None) << "a stale tag cannot invent the flag";
    EXPECT_EQ(ClassifyFrameLatencyWaitable(0x842, false), Owner::Creator) << "sl.dlss_g's own waitable";
    EXPECT_EQ(ClassifyFrameLatencyWaitable(0x842, true), Owner::Ce);
    EXPECT_FALSE(CanSetSwapChainMaximumFrameLatency(Owner::None));
    EXPECT_TRUE(CanSetSwapChainMaximumFrameLatency(Owner::Creator));
    EXPECT_TRUE(CanSetSwapChainMaximumFrameLatency(Owner::Ce));
}

// BioShock Infinite DX11 (logs/20261019_022104): a blit chain (flags=0x2) carries backbuffer_count as
// its BufferCount and needs no waitable; the first diagnostics fix still logged "cannot pace".
TEST(FrameLatencyWaitableOwnershipTest, WaitablelessChainsCarryTheDepthInBufferCountWhereTheyCan) {
    using ce::swapchain_flag_policy::ClassifyBackbufferCountWithoutWaitable;
    using Outcome = ce::swapchain_flag_policy::WaitablelessBackbufferOutcome;
    EXPECT_EQ(ClassifyBackbufferCountWithoutWaitable(DXGI_SWAP_EFFECT_DISCARD, 2, 2), Outcome::CarriedByBufferCount);
    EXPECT_EQ(ClassifyBackbufferCountWithoutWaitable(DXGI_SWAP_EFFECT_SEQUENTIAL, 1, 3), Outcome::CarriedByBufferCount);
    EXPECT_EQ(ClassifyBackbufferCountWithoutWaitable(DXGI_SWAP_EFFECT_FLIP_DISCARD, 2, 2),
              Outcome::CarriedByBufferCount);
    // Strange Brigade DX12 before the resize-hook fix: 3 buffers, configured 2, waitable withheld.
    EXPECT_EQ(ClassifyBackbufferCountWithoutWaitable(DXGI_SWAP_EFFECT_FLIP_DISCARD, 3, 2), Outcome::NotEnforceable);
    EXPECT_EQ(ClassifyBackbufferCountWithoutWaitable(DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 3, 2),
              Outcome::NotEnforceable);
}

TEST(FrameLatencyWaitableOwnershipTest, ReadsTheOwnerFromTheSwapchainItself) {
    using ce::swapchain_flag_policy::TryClassifySwapChainFrameLatencyWaitable;
    using Owner = ce::swapchain_flag_policy::FrameLatencyWaitableOwner;
    auto owner = Owner::Creator;
    UINT flags = 0;

    FakeFlagSwapChain withheld(0x802);
    DXGI_SWAP_CHAIN_DESC desc{};
    ASSERT_TRUE(TryClassifySwapChainFrameLatencyWaitable(&withheld, &owner, &flags, &desc));
    EXPECT_EQ(owner, Owner::None);
    EXPECT_EQ(flags, 0x802u);
    EXPECT_EQ(desc.Flags, 0x802u) << "the descriptor the skip log classifies comes back whole";

    FakeFlagSwapChain creatorOwned(0x842);
    ASSERT_TRUE(TryClassifySwapChainFrameLatencyWaitable(&creatorOwned, &owner, nullptr));
    EXPECT_EQ(owner, Owner::Creator);

    FakeFlagSwapChain ceAdded(0x842);
    NoteCeAddedFrameLatencyWaitable(/*ceAddedWaitable=*/true, S_OK, &ceAdded, "unit-test");
    ASSERT_TRUE(TryClassifySwapChainFrameLatencyWaitable(&ceAdded, &owner, nullptr));
    EXPECT_EQ(owner, Owner::Ce);
    EXPECT_EQ(ceAdded.refs(), 1u) << "classification must not leak a reference";

    FakeFlagSwapChain noDesc(0x842);
    noDesc.FailDesc();
    owner = Owner::Ce;
    EXPECT_FALSE(TryClassifySwapChainFrameLatencyWaitable(&noDesc, &owner, nullptr));
    EXPECT_EQ(owner, Owner::Ce) << "an unreadable descriptor leaves the caller's behavior unchanged";
    EXPECT_FALSE(TryClassifySwapChainFrameLatencyWaitable(nullptr, &owner, nullptr));
}

TEST(FrameLatencyWaitableOwnershipSourceTest, LatencyOverrideNeverCallsDxgiOnAChainWithoutTheWaitable) {
    const std::string shared = ReadProjectSource("hook/present/dxgi_shared_present_pacing.cpp");
    ASSERT_FALSE(shared.empty());
    const size_t apply = shared.find("void ApplyPresentFrameLatencyOverrides(");
    ASSERT_NE(apply, std::string::npos);
    const size_t guard = shared.find("CanSetSwapChainMaximumFrameLatency(owner)", apply);
    const size_t call = shared.find("sc2->SetMaximumFrameLatency(requested)", apply);
    ASSERT_NE(guard, std::string::npos);
    ASSERT_NE(call, std::string::npos);
    EXPECT_LT(guard, call);
    const size_t wait = shared.find("void WaitBackbufferFrameLatency(");
    ASSERT_NE(wait, std::string::npos);
    EXPECT_NE(shared.find("TryClassifySwapChainFrameLatencyWaitable(pSwapChain", wait), std::string::npos)
        << "the skip reason must come from the chain's flags, not be assumed";
}

TEST(FrameLatencyWaitableOwnershipSourceTest, EveryDescriptorOverrideSiteTagsItsCreate) {
    struct Site {
        const char* path;
        const char* apply;
    };
    // Each create that can add the waitable flag must tag the swapchain it
    // created, or `backbuffer_count` silently stops pacing that chain.
    const Site sites[] = {
        {"hook/d3d12/dx12_hook_swapchain_create.cpp", "ApplyBackbufferCountOverrideToDesc("},
        {"hook/d3d12/dx12_hook_swapchain_tracking.cpp", "ApplyBackbufferCountOverrideToDesc("},
        {"hook/wrappers/dxgi_factory_wrap.cpp", "ApplyBackbufferCountOverrideToDesc("},
        {"hook/d3d11/dx11_hook_detours.cpp", "ApplyDX11BackbufferCountOverride("},
        {"hook/wrappers/wrapper_hooks_devices.cpp", "ApplyD3D11CreateDeviceSwapChainBackbufferOverride("},
    };
    for (const Site& site : sites) {
        const std::string source = ReadProjectSource(site.path);
        ASSERT_FALSE(source.empty()) << site.path;
        const size_t applies = CountOccurrences(source, site.apply);
        EXPECT_GT(applies, 0u) << site.path;
        EXPECT_EQ(CountOccurrences(source, "NoteCeAddedFrameLatencyWaitable("), applies) << site.path;
    }
}

TEST(FrameLatencyWaitableOwnershipSourceTest, BothPacingTransportsAskWhoAddedTheWaitable) {
    const std::string shared = ReadProjectSource("hook/present/dxgi_shared_present_pacing.cpp");
    ASSERT_FALSE(shared.empty());
    const size_t sharedWait = shared.find("void WaitBackbufferFrameLatency(");
    ASSERT_NE(sharedWait, std::string::npos);
    const size_t sharedOwner = shared.find("DidCeAddFrameLatencyWaitable(pSwapChain)", sharedWait);
    const size_t sharedBlock = shared.find("WaitFlipQueuePacingObject(hWaitable", sharedWait);
    ASSERT_NE(sharedOwner, std::string::npos);
    ASSERT_NE(sharedBlock, std::string::npos);
    EXPECT_LT(sharedOwner, sharedBlock);

    const std::string wrapper = ReadProjectSource("hook/wrappers/dxgi_swapchain_wrap_frame_latency.cpp");
    ASSERT_FALSE(wrapper.empty());
    const size_t wrapperOwner = wrapper.find("DidCeAddFrameLatencyWaitable(m_pReal)");
    const size_t wrapperBlock = wrapper.find("DXGIShared::WaitFlipQueuePacingObject(");
    ASSERT_NE(wrapperOwner, std::string::npos);
    ASSERT_NE(wrapperBlock, std::string::npos);
    EXPECT_LT(wrapperOwner, wrapperBlock);
}

TEST(PureDLSSStartupStallDumpTest, APresentThatJustReturnedIsNotAStall) {
    // The GTA reading: ProcessFrame 1031 ms ago, the present returned 26 ms ago.
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRequestImmediateDumpForPureDLSSStartupWrapperOnlyStall(
        /*hadFSRFGPhase=*/false, /*startupTopLevelPresentConsumed=*/true, /*wrapperProgressCount=*/4,
        /*startupActivationPending=*/true, /*postSLActive=*/false, /*postSLConfirmedRendering=*/false,
        /*processFrameDormantMs=*/1031, /*presentInFlight=*/false, /*msSincePresentReturned=*/26,
        /*dumpAlreadyRequested=*/false));
}

TEST(PureDLSSStartupStallDumpTest, APresentInsideCeIsTheWatchdogsNotThisDumps) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRequestImmediateDumpForPureDLSSStartupWrapperOnlyStall(
        false, true, 8, true, false, false, /*processFrameDormantMs=*/5000, /*presentInFlight=*/true,
        /*msSincePresentReturned=*/5000, false));
}

TEST(PureDLSSStartupStallDumpTest, SilenceSinceTheLastPresentStillDumps) {
    // The shape the dump exists for: submissions continue, no present arrives.
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldRequestImmediateDumpForPureDLSSStartupWrapperOnlyStall(
        false, true, 8, true, false, false, /*processFrameDormantMs=*/1500, /*presentInFlight=*/false,
        /*msSincePresentReturned=*/1200, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRequestImmediateDumpForPureDLSSStartupWrapperOnlyStall(
        false, true, 8, true, false, false, /*processFrameDormantMs=*/1500, /*presentInFlight=*/false,
        /*msSincePresentReturned=*/999, false));
}

TEST(PureDLSSStartupStallDumpSourceTest, PresentReturnIsRecordedOnEveryTransport) {
    for (const char* file : {"hook/present/dxgi_shared_present_routing.cpp", "hook/present/dxgi_shared_present1.cpp",
                             "hook/wrappers/dxgi_swapchain_wrap_present.cpp"}) {
        const std::string source = ReadProjectSource(file);
        ASSERT_FALSE(source.empty()) << file;
        const size_t enter = source.find("presentInFlightDepth.fetch_add(1");
        ASSERT_NE(enter, std::string::npos) << file;
        const size_t stamp = source.find("lastPresentReturnTickMs.store(ce::hook_clock::TickCount64()", enter);
        const size_t leave = source.find("presentInFlightDepth.fetch_sub(1", enter);
        ASSERT_NE(stamp, std::string::npos) << file;
        ASSERT_NE(leave, std::string::npos) << file;
        EXPECT_LT(stamp, leave) << file << ": stamp before leaving, so depth 0 never reads a stale return";
    }
    const std::string ecl = ReadProjectSource("hook/d3d12/dx12_hook_ecl.cpp");
    ASSERT_FALSE(ecl.empty());
    const size_t policy = ecl.find("ShouldRequestImmediateDumpForPureDLSSStartupWrapperOnlyStall(");
    ASSERT_NE(policy, std::string::npos);
    EXPECT_NE(ecl.find("presentInFlight, msSincePresentReturned, dumpAlreadyRequested", policy), std::string::npos);
}
