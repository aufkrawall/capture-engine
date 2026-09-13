#include <gtest/gtest.h>

#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <array>
#include <cstdint>
#include <string>

#include "../hook/common/dx12_overlay_policy.h"

#include "source_fragment_reader.h"

namespace {

using ce::dx12_overlay_policy::ShouldWaitForOverlayUploadSlot;
using ce::dx12_overlay_policy::UploadSlotGuardFenceBinding;

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

// Minimal reference-counted ID3D12Fence fake. Only the COM lifetime and the
// completed value are meaningful; the tests never submit real GPU work.
class FakeD3D12Fence final : public ID3D12Fence {
public:
    explicit FakeD3D12Fence(uint64_t completedValue = 0) : completed_(completedValue) {}

    ULONG GetRefCount() const { return refs_.load(std::memory_order_relaxed); }
    void SetCompletedValue(uint64_t value) { completed_ = value; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0)
            delete this;
        return remaining;
    }

    // ID3D12Object
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(const WCHAR*) override { return E_NOTIMPL; }

    // ID3D12DeviceChild
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void**) override { return E_NOTIMPL; }

    // ID3D12Fence
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override { return completed_; }
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64, HANDLE) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Signal(UINT64) override { return S_OK; }

private:
    std::atomic<ULONG> refs_{1};
    UINT64 completed_ = 0;
};

TEST(DX12UploadSlotGuardTest, FirstRebindPinsFenceAndRepublishKeepsGuards) {
    auto* fence1 = new FakeD3D12Fence();
    ASSERT_EQ(fence1->GetRefCount(), 1u);

    UploadSlotGuardFenceBinding binding;
    EXPECT_TRUE(binding.RebindIfNeeded(fence1));
    EXPECT_EQ(binding.GetFence(), fence1);
    EXPECT_EQ(fence1->GetRefCount(), 2u);  // pinned with an owning reference

    // Republishing the same fence is not a lifetime change: guards stay valid.
    EXPECT_FALSE(binding.RebindIfNeeded(fence1));
    EXPECT_EQ(fence1->GetRefCount(), 2u);

    binding.Reset();
    EXPECT_EQ(binding.GetFence(), nullptr);
    EXPECT_EQ(fence1->GetRefCount(), 1u);
    fence1->Release();
}

TEST(DX12UploadSlotGuardTest, ReplacementFenceReportsRebindAndReleasesOldPin) {
    auto* fence1 = new FakeD3D12Fence();
    auto* fence2 = new FakeD3D12Fence();

    UploadSlotGuardFenceBinding binding;
    binding.RebindIfNeeded(fence1);
    EXPECT_TRUE(binding.RebindIfNeeded(fence2));
    EXPECT_EQ(binding.GetFence(), fence2);
    EXPECT_EQ(fence1->GetRefCount(), 1u);  // only the test's own reference remains
    EXPECT_EQ(fence2->GetRefCount(), 2u);

    binding.Reset();
    EXPECT_EQ(fence2->GetRefCount(), 1u);
    fence1->Release();
    fence2->Release();
}

TEST(DX12UploadSlotGuardTest, PinnedFenceSurvivesPublisherRelease) {
    // The ABA property the fix relies on: while guards keyed to a fence may
    // still be outstanding, the binding must keep that fence alive. If the
    // publisher's release destroyed the fence, a replacement fence could be
    // allocated at the same address and raw-pointer comparison would miss the
    // lifetime change (session 20260813_173453).
    auto* fence1 = new FakeD3D12Fence();
    UploadSlotGuardFenceBinding binding;
    binding.RebindIfNeeded(fence1);
    ASSERT_EQ(fence1->GetRefCount(), 2u);

    fence1->Release();  // publisher (overlay reinit) drops its reference
    EXPECT_EQ(fence1->GetRefCount(), 1u);  // still pinned by the binding
    EXPECT_EQ(binding.GetFence(), fence1);

    binding.Reset();
}

TEST(DX12UploadSlotGuardTest, StaleGuardsCannotWaitAcrossFenceLifetimeChange) {
    // Models the repro sequence: guards are recorded against fence1's value
    // space (~219-222), overlay reinit replaces fence1 with fence2 whose
    // values restart at 0, and the next render rebinds before any wait. With
    // the pin the rebind always detects the replacement, the guards are
    // cleared, and no slot ever waits against the unreachable stale values.
    constexpr int kPoolSize = 4;
    uint64_t slotGuard[kPoolSize] = {0, 0, 0, 0};

    auto* fence1 = new FakeD3D12Fence(0);
    UploadSlotGuardFenceBinding binding;
    ASSERT_TRUE(binding.RebindIfNeeded(fence1));

    uint64_t currentFenceValue = 218;
    for (int frame = 0; frame < kPoolSize; ++frame) {
        const int slot = frame % kPoolSize;
        ++currentFenceValue;
        slotGuard[slot] = currentFenceValue;  // guard recorded at submit time
    }

    fence1->Release();  // overlay reinit drops the publisher reference
    auto* fence2 = new FakeD3D12Fence(0);  // new fence lifetime, values restart

    if (binding.RebindIfNeeded(fence2)) {
        for (int i = 0; i < kPoolSize; ++i)
            slotGuard[i] = 0;
    }
    EXPECT_EQ(binding.GetFence(), fence2);

    for (int slot = 0; slot < kPoolSize; ++slot) {
        EXPECT_FALSE(ShouldWaitForOverlayUploadSlot(slotGuard[slot], fence2->GetCompletedValue()));
    }

    binding.Reset();
    fence2->Release();
}

TEST(DX12UploadSlotGuardTest, NullPublishedFenceDisablesWaiting) {
    auto* fence1 = new FakeD3D12Fence();
    UploadSlotGuardFenceBinding binding;
    binding.RebindIfNeeded(fence1);

    EXPECT_TRUE(binding.RebindIfNeeded(nullptr));
    EXPECT_EQ(binding.GetFence(), nullptr);
    EXPECT_EQ(fence1->GetRefCount(), 1u);
    fence1->Release();
}

TEST(DX12UploadSlotGuardTest, DetachHandsOwnershipToTheCaller) {
    auto* fence1 = new FakeD3D12Fence();
    UploadSlotGuardFenceBinding binding;
    binding.RebindIfNeeded(fence1);
    ASSERT_EQ(fence1->GetRefCount(), 2u);

    ID3D12Fence* detached = binding.Detach();
    EXPECT_EQ(detached, fence1);
    EXPECT_EQ(binding.GetFence(), nullptr);
    EXPECT_EQ(fence1->GetRefCount(), 2u);  // ownership handed back, not dropped

    detached->Release();
    EXPECT_EQ(fence1->GetRefCount(), 1u);
    fence1->Release();
}

TEST(DX12UploadSlotGuardTest, DescFreeBackendClearsGuardsOnRebind) {
    const std::string source = ReadSource("hook/apis/dx12_hook_types_impl.cpp");
    ASSERT_FALSE(source.empty());

    const size_t rebind = source.find("slotGuardBinding_.RebindIfNeeded(dx12_hook_s_descFreeSlotFence)");
    ASSERT_NE(rebind, std::string::npos);
    const size_t clear = source.find("slotFenceValue_[i] = 0;", rebind);
    ASSERT_NE(clear, std::string::npos);
    EXPECT_LT(rebind, clear);
}

TEST(DX12UploadSlotGuardTest, TexturedBackendClearsGuardsOnFenceRebind) {
    const std::string source = ReadSource("hook/common/custom_overlay_dx12.cpp");
    ASSERT_FALSE(source.empty());

    const size_t rebind = source.find("slotGuardBinding.RebindIfNeeded(fence)");
    ASSERT_NE(rebind, std::string::npos);
    const size_t clear = source.find("slotFenceValue[i] = 0;", rebind);
    ASSERT_NE(clear, std::string::npos);
    EXPECT_LT(rebind, clear);
}

TEST(DX12UploadSlotGuardTest, AllocatorCoupledSlotsCannotWrapInsideTheInFlightAllocatorPool) {
    namespace policy = ce::dx12_overlay_policy;
    constexpr int kOldIndependentUploadSlotCount = 4;

    ASSERT_EQ(policy::kAllocatorCoupledUploadSlotCount, 16);
    // The old upload ring aliased allocator 0 while allocator 4 could legally
    // be selected and allocator 0's GPU work was still in flight.
    EXPECT_EQ(0 % kOldIndependentUploadSlotCount, 4 % kOldIndependentUploadSlotCount);
    EXPECT_NE(policy::ResolveAllocatorCoupledUploadSlot(0), policy::ResolveAllocatorCoupledUploadSlot(4));

    std::array<int, policy::kAllocatorCoupledUploadSlotCount> owners;
    owners.fill(-1);
    for (int allocatorSlot = 0; allocatorSlot < policy::kAllocatorCoupledUploadSlotCount; ++allocatorSlot) {
        const int uploadSlot = policy::ResolveAllocatorCoupledUploadSlot(allocatorSlot);
        ASSERT_GE(uploadSlot, 0);
        ASSERT_LT(uploadSlot, static_cast<int>(owners.size()));
        EXPECT_EQ(owners[uploadSlot], -1);
        owners[uploadSlot] = allocatorSlot;
    }
    EXPECT_EQ(policy::ResolveAllocatorCoupledUploadSlot(-1), -1);
    EXPECT_EQ(policy::ResolveAllocatorCoupledUploadSlot(policy::kAllocatorCoupledUploadSlotCount), -1);

    EXPECT_TRUE(policy::CanUseFenceGuardedUploadSlotFallback(true, 1));
    EXPECT_FALSE(policy::CanUseFenceGuardedUploadSlotFallback(true, 0));
    EXPECT_FALSE(policy::CanUseFenceGuardedUploadSlotFallback(false, 1));
}

TEST(DX12UploadSlotGuardTest, PostSLBindsUploadStorageToAllocatorAndItsExactSignal) {
    const std::string types = ReadSource("hook/apis/dx12_hook_types.h");
    const std::string backend = ReadSource("hook/apis/dx12_hook_types_impl.cpp");
    const std::string adapter = ReadSource("hook/common/overlay_adapter.cpp");
    const std::string route = ReadSource("hook/apis/dx12_hook_postsl_render_route.cpp");
    const std::string submit = ReadSource("hook/apis/dx12_hook_postsl_render_submit.cpp");
    const std::string normalRoute = ReadSource("hook/apis/dx12_hook_process_session_draw_submit.cpp");
    ASSERT_FALSE(types.empty());
    ASSERT_FALSE(backend.empty());
    ASSERT_FALSE(adapter.empty());
    ASSERT_FALSE(route.empty());
    ASSERT_FALSE(submit.empty());
    ASSERT_FALSE(normalRoute.empty());

    EXPECT_NE(types.find("kPoolSize = ce::dx12_overlay_policy::kAllocatorCoupledUploadSlotCount"),
              std::string::npos);
    EXPECT_NE(types.find("ALLOC_POOL_SIZE = ce::dx12_overlay_policy::kAllocatorCoupledUploadSlotCount"),
              std::string::npos);
    EXPECT_NE(backend.find("nextUploadSlot_.exchange(-1"), std::string::npos);
    EXPECT_NE(backend.find("CanUseFenceGuardedUploadSlotFallback"), std::string::npos);
    EXPECT_NE(backend.find("draw refused"), std::string::npos);
    EXPECT_NE(adapter.find("backend->SetNextUploadSlot(slot)"), std::string::npos);

    EXPECT_NE(route.find("uploadGuardValue = dx12_hook_g_State.fence ?"), std::string::npos);
    EXPECT_NE(route.find("dx12_hook_s_descFreeSlotGuardValue = uploadGuardValue"), std::string::npos);
    EXPECT_NE(route.find("dx12_hook_g_D3D11On12Adapter.SetDX12NextUploadSlot(idx)"), std::string::npos);
    EXPECT_EQ(route.find("dx12_hook_s_descFreeSlotGuardValue = 0"), std::string::npos);
    EXPECT_NE(submit.find("uploadGuardValue != 0 ? uploadGuardValue"), std::string::npos);

    const size_t firstNormalCoupling = normalRoute.find("SetDX12NextUploadSlot(idx)");
    ASSERT_NE(firstNormalCoupling, std::string::npos);
    EXPECT_NE(normalRoute.find("SetDX12NextUploadSlot(idx)", firstNormalCoupling + 1), std::string::npos);
}

}  // namespace
