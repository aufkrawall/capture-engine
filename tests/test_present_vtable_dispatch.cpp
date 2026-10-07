#include <gtest/gtest.h>

#include "hook/present/dxgi_shared_internal.h"
#include "hook/present/present_vtable_dispatch.h"
#include "hook/present/dxgi_shared_detail/present_vtable_call.h"

#include <array>

namespace {
struct Probe {
    IDXGISwapChain* receiver = nullptr;
    const DXGI_PRESENT_PARAMETERS* parameters = nullptr;
    unsigned presentCalls = 0;
    unsigned present1Calls = 0;
} probe;

constexpr UINT kInterval = 3;
constexpr UINT kFlags = DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_WAIT;

void CheckReentry(IDXGISwapChain* receiver) {
    HRESULT result = E_UNEXPECTED;
    const void* inlineCaller = reinterpret_cast<void*>(&CheckReentry);
    EXPECT_EQ(DXGIShared::ResolvePresentDetourCaller(receiver, inlineCaller), inlineCaller);
    EXPECT_EQ(DXGIShared::ResolvePresent1DetourCaller(receiver, inlineCaller), inlineCaller);
    // A predecessor can reach the inline view on this receiver. Neither method
    // may reuse the vtable link currently being forwarded, or borrow another view.
    EXPECT_FALSE(DXGIShared::TryForwardPresentVTableCall(receiver, kInterval, kFlags, result));
    EXPECT_FALSE(DXGIShared::TryForwardPresent1VTableCall(receiver, kInterval, kFlags, probe.parameters, result));
    void** otherVTable = nullptr;
    auto* otherReceiver = reinterpret_cast<IDXGISwapChain*>(&otherVTable);
    EXPECT_FALSE(DXGIShared::TryForwardPresentVTableCall(otherReceiver, kInterval, kFlags, result));
    EXPECT_FALSE(DXGIShared::TryForwardPresent1VTableCall(otherReceiver, kInterval, kFlags, probe.parameters, result));
    EXPECT_EQ(result, E_UNEXPECTED);
}

HRESULT STDMETHODCALLTYPE ForeignPresent(IDXGISwapChain* receiver, UINT interval, UINT flags) {
    EXPECT_EQ(receiver, probe.receiver);
    EXPECT_EQ(interval, kInterval);
    EXPECT_EQ(flags, kFlags);
    if (++probe.presentCalls == 1)
        CheckReentry(receiver);
    return S_FALSE;
}

HRESULT STDMETHODCALLTYPE ForeignPresent1(IDXGISwapChain* receiver, UINT interval, UINT flags,
                                          const DXGI_PRESENT_PARAMETERS* parameters) {
    EXPECT_EQ(receiver, probe.receiver);
    EXPECT_EQ(interval, kInterval);
    EXPECT_EQ(flags, kFlags);
    EXPECT_EQ(parameters, probe.parameters);
    if (++probe.present1Calls == 1)
        CheckReentry(receiver);
    return DXGI_STATUS_OCCLUDED;
}

class PresentVTableFixture : public ::testing::Test {
protected:
    void SetUp() override {
        previousPresent_ = DXGIShared::dxgi_shared_oPresent;
        previousPresent1_ = DXGIShared::dxgi_shared_oPresent1;
        vtable_[8] = reinterpret_cast<void*>(&ForeignPresent);
        vtable_[22] = reinterpret_cast<void*>(&ForeignPresent1);
        receiverVTable_ = vtable_.data();
        probe = {};
        probe.receiver = reinterpret_cast<IDXGISwapChain*>(&receiverVTable_);
        probe.parameters = &parameters_;
        ASSERT_TRUE(DXGIShared::InstallSwapchainPresentVTableHooks(probe.receiver));
    }
    void TearDown() override {
        DXGIShared::ReleaseSwapchainPresentVTableHooksForRuntimeHandoff("native dispatch fixture");
        EXPECT_EQ(vtable_[8], reinterpret_cast<void*>(&ForeignPresent));
        EXPECT_EQ(vtable_[22], reinterpret_cast<void*>(&ForeignPresent1));
        DXGIShared::dxgi_shared_oPresent = previousPresent_;
        DXGIShared::dxgi_shared_oPresent1 = previousPresent1_;
        probe = {};
    }
    void ExpectScopeClosed() {
        HRESULT result = E_UNEXPECTED;
        EXPECT_FALSE(DXGIShared::TryForwardPresentVTableCall(probe.receiver, kInterval, kFlags, result));
        EXPECT_FALSE(DXGIShared::TryForwardPresent1VTableCall(probe.receiver, kInterval, kFlags, &parameters_, result));
        EXPECT_EQ(result, E_UNEXPECTED);
    }
    std::array<void*, 40> vtable_{};
    void** receiverVTable_ = nullptr;
    DXGI_PRESENT_PARAMETERS parameters_{};
    PFN_Present previousPresent_ = nullptr;
    PFN_Present1 previousPresent1_ = nullptr;
};

TEST_F(PresentVTableFixture, AnotherReceiverCannotBorrowAnIdleScope) {
    DXGIShared::detail::ScopedVTableCall<PFN_Present1> scope(probe.receiver, &ForeignPresent1);
    void** otherVTable = nullptr;
    auto* otherReceiver = reinterpret_cast<IDXGISwapChain*>(&otherVTable);
    HRESULT result = E_UNEXPECTED;
    EXPECT_FALSE(DXGIShared::TryForwardPresent1VTableCall(otherReceiver, kInterval, kFlags, &parameters_, result));
    EXPECT_EQ(probe.present1Calls, 0u);
    EXPECT_EQ(result, E_UNEXPECTED);
    EXPECT_TRUE(DXGIShared::TryForwardPresent1VTableCall(probe.receiver, kInterval, kFlags, &parameters_, result));
    EXPECT_EQ(result, DXGI_STATUS_OCCLUDED);
    EXPECT_EQ(probe.present1Calls, 1u);
}

TEST_F(PresentVTableFixture, BothInterceptionViewsAreRecognizedWithoutForeignOrCrossMethodTargets) {
    EXPECT_TRUE(DXGIShared::IsPresentDetourAddress(vtable_[8]));
    EXPECT_TRUE(DXGIShared::IsPresent1DetourAddress(vtable_[22]));
    EXPECT_TRUE(DXGIShared::IsPresentDetourAddress(reinterpret_cast<void*>(&DXGIShared::DetourPresent)));
    EXPECT_TRUE(DXGIShared::IsPresent1DetourAddress(reinterpret_cast<void*>(&DXGIShared::DetourPresent1)));
    EXPECT_FALSE(DXGIShared::IsPresentDetourAddress(vtable_[22]));
    EXPECT_FALSE(DXGIShared::IsPresent1DetourAddress(vtable_[8]));
    EXPECT_FALSE(DXGIShared::IsPresentDetourAddress(reinterpret_cast<void*>(&ForeignPresent)));
    EXPECT_FALSE(DXGIShared::IsPresent1DetourAddress(reinterpret_cast<void*>(&ForeignPresent1)));
}

TEST_F(PresentVTableFixture, OriginalCallerIsScopedToTheReceiverMethodAndInterceptionView) {
    const void* originalCaller = reinterpret_cast<void*>(&ForeignPresent1);
    const void* inlineCaller = reinterpret_cast<void*>(&ForeignPresent);
    void** otherVTable = nullptr;
    auto* otherReceiver = reinterpret_cast<IDXGISwapChain*>(&otherVTable);
    {
        DXGIShared::detail::ScopedVTableCall<PFN_Present> scope(probe.receiver, &ForeignPresent, originalCaller);
        EXPECT_EQ(DXGIShared::ResolvePresentDetourCaller(probe.receiver, inlineCaller), originalCaller);
        EXPECT_EQ(DXGIShared::ResolvePresentDetourCaller(otherReceiver, inlineCaller), inlineCaller);
        EXPECT_EQ(DXGIShared::ResolvePresent1DetourCaller(probe.receiver, inlineCaller), inlineCaller);
        HRESULT result = E_UNEXPECTED;
        EXPECT_TRUE(DXGIShared::TryForwardPresentVTableCall(probe.receiver, kInterval, kFlags, result));
        EXPECT_EQ(result, S_FALSE);
    }
    EXPECT_EQ(DXGIShared::ResolvePresentDetourCaller(probe.receiver, inlineCaller), inlineCaller);
}

TEST_F(PresentVTableFixture, NestedReceiverScopeRestoresTheOuterReceiver) {
    auto* outerReceiver = probe.receiver;
    DXGIShared::detail::ScopedVTableCall<PFN_Present> outer(probe.receiver, &ForeignPresent);
    void** otherVTable = vtable_.data();
    auto* otherReceiver = reinterpret_cast<IDXGISwapChain*>(&otherVTable);
    HRESULT result = E_UNEXPECTED;
    {
        DXGIShared::detail::ScopedVTableCall<PFN_Present> inner(otherReceiver, &ForeignPresent);
        EXPECT_FALSE(DXGIShared::TryForwardPresentVTableCall(probe.receiver, kInterval, kFlags, result));
        probe.receiver = otherReceiver;
        EXPECT_TRUE(DXGIShared::TryForwardPresentVTableCall(probe.receiver, kInterval, kFlags, result));
    }
    probe.receiver = outerReceiver;
    EXPECT_TRUE(DXGIShared::TryForwardPresentVTableCall(probe.receiver, kInterval, kFlags, result));
    EXPECT_EQ(probe.presentCalls, 2u);
    EXPECT_EQ(result, S_FALSE);
}

TEST_F(PresentVTableFixture, PresentForwardsExactArgumentsAndResultWithoutReusingAnActiveLink) {
    const auto present = reinterpret_cast<PFN_Present>(vtable_[8]);
    EXPECT_EQ(present(probe.receiver, kInterval, kFlags), S_FALSE);
    EXPECT_EQ(probe.presentCalls, 1u);
    EXPECT_EQ(probe.present1Calls, 0u);
    ExpectScopeClosed();
}

TEST_F(PresentVTableFixture, Present1ForwardsItsParametersAndResultWithoutBorrowingPresent) {
    const auto present1 = reinterpret_cast<PFN_Present1>(vtable_[22]);
    EXPECT_EQ(present1(probe.receiver, kInterval, kFlags, &parameters_), DXGI_STATUS_OCCLUDED);
    EXPECT_EQ(probe.presentCalls, 0u);
    EXPECT_EQ(probe.present1Calls, 1u);
    ExpectScopeClosed();
}
}  // namespace
