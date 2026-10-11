#include <gtest/gtest.h>

#include <thread>

#include "hook/present/translated_swapchain_owner.h"

namespace {
using ce::translated_swapchain::CreateScope;
using ce::translated_swapchain::ObserveNativeCreate;
const auto kWindow = reinterpret_cast<HWND>(0x1000);
const auto kOtherWindow = reinterpret_cast<HWND>(0x2000);
auto* const kNative = reinterpret_cast<IDXGISwapChain*>(0x3000);
auto* const kOtherNative = reinterpret_cast<IDXGISwapChain*>(0x4000);

TEST(TranslatedSwapchainOwner, RequiresAnExactCoveredNativeCreateForTheSameWindow) {
    CreateScope scope(kWindow);
    ObserveNativeCreate(kNative, kOtherWindow, true);
    ObserveNativeCreate(kNative, kWindow, false);
    ObserveNativeCreate(nullptr, kWindow, true);
    EXPECT_FALSE(scope.HasSingleNativeOwner());
    ObserveNativeCreate(kNative, kWindow, true);
    ObserveNativeCreate(kNative, kWindow, true);  // inline/vtable views of the same create
    EXPECT_TRUE(scope.HasSingleNativeOwner());
    ObserveNativeCreate(kOtherNative, kWindow, true);
    EXPECT_FALSE(scope.HasSingleNativeOwner());
}

TEST(TranslatedSwapchainOwner, NativeQueueInputsCannotDelegateFgProxyOwnership) {
    CreateScope scope(kWindow, false);
    ObserveNativeCreate(kNative, kWindow, true);
    EXPECT_FALSE(scope.HasSingleNativeOwner());
}

TEST(TranslatedSwapchainOwner, NestedFrontendCreatesRetainTheirOwnEvidence) {
    CreateScope outer(kWindow);
    {
        CreateScope inner(kOtherWindow);
        ObserveNativeCreate(kOtherNative, kOtherWindow, true);
        EXPECT_TRUE(inner.HasSingleNativeOwner());
        EXPECT_FALSE(outer.HasSingleNativeOwner());
    }
    ObserveNativeCreate(kNative, kWindow, true);
    EXPECT_TRUE(outer.HasSingleNativeOwner());
}

TEST(TranslatedSwapchainOwner, ConcurrentCreationCannotClaimAnotherThreadsFrontend) {
    CreateScope scope(kWindow);
    std::thread peer([] { ObserveNativeCreate(kNative, kWindow, true); });
    peer.join();
    EXPECT_FALSE(scope.HasSingleNativeOwner());
    ObserveNativeCreate(kNative, kWindow, true);
    EXPECT_TRUE(scope.HasSingleNativeOwner());
}
}  // namespace
