// Frame presenter identity (hook/fg/fg_presenter.h): which component presents the displayed frames on a native
// swapchain, and when a Streamline FG signal is a front for another generator.
//
// Talos + OptiScaler with FGInput=DLSSG, FGOutput=XeFG (session 20261023_183119): the game called
// slDLSSGSetOptions(on) while XeSS frame generation presented every displayed frame from a swapchain it created
// itself; CE took the signal for DLSS-G (PostSL on half the presents, "DLSS FG 2x" published).

#include <gtest/gtest.h>

#include <cstdint>

#include "hook/fg/fg_presenter.h"

namespace {

using namespace ce::fg_presenter;
using ce::fg_runtime::RuntimeMode;

// Identities only: the registry never calls into a swapchain or a queue.
IDXGISwapChain* FakeSwapchain(uintptr_t id) {
    return reinterpret_cast<IDXGISwapChain*>(0x10000 + id * 0x100);
}
ID3D12CommandQueue* FakeQueue(uintptr_t id) {
    return reinterpret_cast<ID3D12CommandQueue*>(0x90000 + id * 0x100);
}
HWND FakeWindow(uintptr_t id) {
    return reinterpret_cast<HWND>(0x500 + id);
}

class FGPresenterRegistryTest : public ::testing::Test {
protected:
    void SetUp() override { ResetForTesting(); }
    void TearDown() override { ResetForTesting(); }
};

TEST(FGPresenterPolicyTest, ARuntimeCreateScopeIsAuthoritativeAndModuleIdentityTheFallback) {
    EXPECT_EQ(ResolveCreatedSwapchainPresenter(Presenter::kXeFGSwapchain, false), Presenter::kXeFGSwapchain);
    EXPECT_EQ(ResolveCreatedSwapchainPresenter(Presenter::kXeFGSwapchain, true), Presenter::kXeFGSwapchain);
    EXPECT_EQ(ResolveCreatedSwapchainPresenter(Presenter::kApplication, true), Presenter::kXeFGSwapchain);
    EXPECT_EQ(ResolveCreatedSwapchainPresenter(Presenter::kApplication, false), Presenter::kApplication);
    EXPECT_TRUE(IsRuntimeOwnedOutputPresenter(Presenter::kXeFGSwapchain));
    EXPECT_FALSE(IsRuntimeOwnedOutputPresenter(Presenter::kApplication));
    EXPECT_FALSE(IsRuntimeOwnedOutputPresenter(Presenter::kStreamlineDLSSG));
    EXPECT_STREQ(PresenterName(Presenter::kXeFGSwapchain), "xefg-swapchain");
}

TEST(FGPresenterPolicyTest, AStreamlineSignalIsAFrontWhenItsApiOrThePresenterIsSomeoneElses) {
    EXPECT_FALSE(IsStreamlineFGSignalFront(false, false));
    EXPECT_TRUE(IsStreamlineFGSignalFront(true, false));   // OptiScaler's slDLSSGSetOptions, FSR FG output
    EXPECT_TRUE(IsStreamlineFGSignalFront(false, true));   // XeFG presents
    EXPECT_TRUE(IsStreamlineFGSignalFront(true, true));    // the session: both
    EXPECT_EQ(ClassifyDLSSGApiProvider(true, false, true), DLSSGApiProvider::kStreamline);
    EXPECT_EQ(ClassifyDLSSGApiProvider(false, false, true), DLSSGApiProvider::kForeign);
    EXPECT_EQ(ClassifyDLSSGApiProvider(false, true, true), DLSSGApiProvider::kCaptureEngine);
    EXPECT_EQ(ClassifyDLSSGApiProvider(true, false, false), DLSSGApiProvider::kUnknown);
}

TEST(FGPresenterPolicyTest, XeFGGenerationTogglesKeepPresentationWhereItIs) {
    EXPECT_TRUE(IsPresenterOwnedRuntimeModeToggle(RuntimeMode::kOff, RuntimeMode::kXeFG, true));
    EXPECT_TRUE(IsPresenterOwnedRuntimeModeToggle(RuntimeMode::kXeFG, RuntimeMode::kOff, true));
    EXPECT_TRUE(IsPresenterOwnedRuntimeModeToggle(RuntimeMode::kStreamlineNoFG, RuntimeMode::kXeFG, true));
    // No presenter: an ordinary transition. A switch to another runtime moves presentation.
    EXPECT_FALSE(IsPresenterOwnedRuntimeModeToggle(RuntimeMode::kOff, RuntimeMode::kXeFG, false));
    EXPECT_FALSE(IsPresenterOwnedRuntimeModeToggle(RuntimeMode::kXeFG, RuntimeMode::kFSRFG, true));
    EXPECT_FALSE(IsPresenterOwnedRuntimeModeToggle(RuntimeMode::kDLSSFG, RuntimeMode::kXeFG, true));
    EXPECT_FALSE(IsPresenterOwnedRuntimeModeToggle(RuntimeMode::kXeFG, RuntimeMode::kXeFG, true));
    EXPECT_FALSE(IsPresenterOwnedRuntimeModeToggle(RuntimeMode::kOff, RuntimeMode::kFSRFG, true));
}

TEST_F(FGPresenterRegistryTest, AnApplicationCreateRegistersNothing) {
    EXPECT_EQ(NoteSwapchainCreated(FakeSwapchain(1), FakeWindow(1), FakeQueue(1), false, "test"),
              Presenter::kApplication);
    EXPECT_FALSE(HasRuntimeOwnedSwapchains());
    EXPECT_EQ(PresenterFor(FakeSwapchain(1)), Presenter::kApplication);
    EXPECT_EQ(OutputQueue(FakeSwapchain(1)), nullptr);
}

// Creator attribution beats provenance: whatever a Present's stack shows later, the chain XeFG created in its
// init is XeFG's, and its presents go to XeFG's queue.
TEST_F(FGPresenterRegistryTest, TheSwapchainCreatedInsideTheInitScopeIsTheRuntimes) {
    IDXGISwapChain* native = nullptr;
    {
        ScopedRuntimeSwapchainCreate scope(Presenter::kXeFGSwapchain, FakeWindow(1), 0xC0);
        EXPECT_EQ(OpenRuntimeCreateScopeFor(FakeWindow(1)), Presenter::kXeFGSwapchain);
        EXPECT_EQ(OpenRuntimeCreateScopeFor(nullptr), Presenter::kXeFGSwapchain);
        EXPECT_EQ(OpenRuntimeCreateScopeFor(FakeWindow(2)), Presenter::kApplication);
        EXPECT_EQ(NoteSwapchainCreated(FakeSwapchain(7), FakeWindow(1), FakeQueue(1), false, "test"),
                  Presenter::kXeFGSwapchain);
        native = scope.CreatedSwapchain();
    }
    EXPECT_EQ(native, FakeSwapchain(7));
    EXPECT_EQ(OpenRuntimeCreateScopeFor(FakeWindow(1)), Presenter::kApplication);
    EXPECT_TRUE(HasRuntimeOwnedSwapchains());
    EXPECT_TRUE(HasRuntimeOwnedOutputPresenter());
    EXPECT_EQ(PresenterFor(FakeSwapchain(7)), Presenter::kXeFGSwapchain);
    EXPECT_EQ(OutputQueue(FakeSwapchain(7)), FakeQueue(1));
    EXPECT_TRUE(IsStreamlineFGSignalFrontNow());
}

// A late injection without the scope: libxess_fg.dll on the create stack.
TEST_F(FGPresenterRegistryTest, ModuleIdentityAttributesACreateCESawWithoutTheScope) {
    EXPECT_EQ(NoteSwapchainCreated(FakeSwapchain(3), FakeWindow(1), FakeQueue(2), true, "test"),
              Presenter::kXeFGSwapchain);
    EXPECT_EQ(PresenterFor(FakeSwapchain(3)), Presenter::kXeFGSwapchain);
}

// XeFG substitutes its own queue on ResizeBuffers1; a resize is no transfer of ownership.
TEST_F(FGPresenterRegistryTest, AResizeRebindsTheQueueButKeepsThePresenter) {
    ScopedRuntimeSwapchainCreate scope(Presenter::kXeFGSwapchain, FakeWindow(1), 0xC0);
    NoteSwapchainCreated(FakeSwapchain(4), FakeWindow(1), FakeQueue(1), false, "test");
    NoteSwapchainPresentQueue(FakeSwapchain(4), FakeQueue(9), "test");
    EXPECT_EQ(PresenterFor(FakeSwapchain(4)), Presenter::kXeFGSwapchain);
    EXPECT_EQ(OutputQueue(FakeSwapchain(4)), FakeQueue(9));
    // A queue for a chain nobody registered changes nothing.
    NoteSwapchainPresentQueue(FakeSwapchain(5), FakeQueue(8), "test");
    EXPECT_EQ(PresenterFor(FakeSwapchain(5)), Presenter::kApplication);
}

// xefgSwapChainDestroy retires the chains of that context only.
TEST_F(FGPresenterRegistryTest, DestroyRetiresTheOwnersChains) {
    {
        ScopedRuntimeSwapchainCreate scope(Presenter::kXeFGSwapchain, FakeWindow(1), 0xC0);
        NoteSwapchainCreated(FakeSwapchain(1), FakeWindow(1), FakeQueue(1), false, "test");
    }
    {
        ScopedRuntimeSwapchainCreate scope(Presenter::kXeFGSwapchain, FakeWindow(2), 0xC1);
        NoteSwapchainCreated(FakeSwapchain(2), FakeWindow(2), FakeQueue(2), false, "test");
    }
    RetireOwner(0xC0, "test");
    EXPECT_EQ(PresenterFor(FakeSwapchain(1)), Presenter::kApplication);
    EXPECT_EQ(PresenterFor(FakeSwapchain(2)), Presenter::kXeFGSwapchain);
    RetireOwner(0xC1, "test");
    EXPECT_FALSE(HasRuntimeOwnedSwapchains());
    EXPECT_FALSE(IsStreamlineFGSignalFrontNow());
}

// The game recreates its swapchain on the window (OptiScaler destroys XeFG then): DXGI allows one flip swapchain
// per window, so the new create proves the old chain gone, even at a reused address.
TEST_F(FGPresenterRegistryTest, ANewCreateForTheWindowRetiresTheOldChainEvenAtTheSameAddress) {
    {
        ScopedRuntimeSwapchainCreate scope(Presenter::kXeFGSwapchain, FakeWindow(1), 0xC0);
        NoteSwapchainCreated(FakeSwapchain(1), FakeWindow(1), FakeQueue(1), false, "test");
    }
    EXPECT_EQ(NoteSwapchainCreated(FakeSwapchain(2), FakeWindow(1), FakeQueue(3), false, "test"),
              Presenter::kApplication);
    EXPECT_EQ(PresenterFor(FakeSwapchain(1)), Presenter::kApplication);
    EXPECT_FALSE(HasRuntimeOwnedSwapchains());

    {
        ScopedRuntimeSwapchainCreate scope(Presenter::kXeFGSwapchain, FakeWindow(1), 0xC2);
        NoteSwapchainCreated(FakeSwapchain(2), FakeWindow(1), FakeQueue(4), false, "test");
    }
    EXPECT_EQ(PresenterFor(FakeSwapchain(2)), Presenter::kXeFGSwapchain);
    const uint64_t first = PresenterLifetime(FakeSwapchain(2));
    EXPECT_NE(first, 0u);
    // XeFG's next chain at the same address: pointer identity cannot tell them apart, the lifetime can (ProcessFrame
    // re-initializes the overlay on it instead of drawing with the old chain's render targets).
    {
        ScopedRuntimeSwapchainCreate scope(Presenter::kXeFGSwapchain, FakeWindow(1), 0xC3);
        NoteSwapchainCreated(FakeSwapchain(2), FakeWindow(1), FakeQueue(5), false, "test");
    }
    EXPECT_GT(PresenterLifetime(FakeSwapchain(2)), first);
    EXPECT_EQ(NoteSwapchainCreated(FakeSwapchain(2), FakeWindow(1), FakeQueue(3), false, "test"),
              Presenter::kApplication);
    EXPECT_EQ(PresenterFor(FakeSwapchain(2)), Presenter::kApplication);
    EXPECT_EQ(PresenterLifetime(FakeSwapchain(2)), 0u);
}

TEST_F(FGPresenterRegistryTest, NestedScopesRestoreTheOuterOne) {
    ScopedRuntimeSwapchainCreate outer(Presenter::kXeFGSwapchain, FakeWindow(1), 0xC0);
    {
        ScopedRuntimeSwapchainCreate inner(Presenter::kUnknownExternal, FakeWindow(2), 0xD0);
        EXPECT_EQ(OpenRuntimeCreateScopeFor(FakeWindow(2)), Presenter::kUnknownExternal);
    }
    EXPECT_EQ(OpenRuntimeCreateScopeFor(FakeWindow(1)), Presenter::kXeFGSwapchain);
}

TEST_F(FGPresenterRegistryTest, AForeignDLSSGApiMakesTheSignalAFrontWithoutAPresenter) {
    EXPECT_FALSE(IsStreamlineFGSignalFrontNow());
    NoteDLSSGApiProvider(DLSSGApiProvider::kStreamline, reinterpret_cast<void*>(0x1000), "C:\\sl\\sl.dlss_g.dll");
    EXPECT_FALSE(IsStreamlineFGSignalFrontNow());
    // CE's own wrappers say nothing about the provider underneath.
    NoteDLSSGApiProvider(DLSSGApiProvider::kCaptureEngine, reinterpret_cast<void*>(0x2000), "capture_hook_x64.dll");
    EXPECT_FALSE(IsDLSSGApiServedOutsideStreamline());
    NoteDLSSGApiProvider(DLSSGApiProvider::kForeign, reinterpret_cast<void*>(0x3000), "H:\\game\\dxgi.dll");
    EXPECT_TRUE(IsDLSSGApiServedOutsideStreamline());
    EXPECT_TRUE(IsStreamlineFGSignalFrontNow());
}

}  // namespace
