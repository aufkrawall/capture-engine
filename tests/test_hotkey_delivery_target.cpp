// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <gtest/gtest.h>

#include "common/overlay/hotkey_delivery_target.h"

namespace {
static_assert(noexcept(HotkeyDeliveryTarget()));
static_assert(noexcept(HotkeyDeliveryTarget(1, WM_APP)));
constexpr UINT kFirstMessage = WM_APP + 0x210;
constexpr UINT kSecondMessage = WM_APP + 0x211;

class HotkeyDeliveryTargetTest : public ::testing::Test {
protected:
    void SetUp() override {
        MSG message{};
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);  // Create this thread's queue.
    }
    void TearDown() override {
        MSG message{};
        while (PeekMessageW(&message, reinterpret_cast<HWND>(-1), kFirstMessage, kSecondMessage, PM_REMOVE)) {}
    }
};

TEST_F(HotkeyDeliveryTargetTest, RoutesIdentityAndKeyToTheChosenThreadMessage) {
    const HotkeyDeliveryTarget target(GetCurrentThreadId(), kFirstMessage);
    ASSERT_TRUE(target.Post(3, '9'));
    MSG message{};
    ASSERT_TRUE(PeekMessageW(&message, reinterpret_cast<HWND>(-1), kFirstMessage, kFirstMessage, PM_REMOVE));
    EXPECT_EQ(message.message, kFirstMessage);
    EXPECT_EQ(message.wParam, 3u);
    EXPECT_EQ(message.lParam, '9');
}

TEST_F(HotkeyDeliveryTargetTest, MessageIdentityBelongsToTheHostRatherThanTheHookImplementation) {
    const HotkeyDeliveryTarget first(GetCurrentThreadId(), kFirstMessage);
    const HotkeyDeliveryTarget second(GetCurrentThreadId(), kSecondMessage);
    EXPECT_FALSE(first == second);
    ASSERT_TRUE(second.Post(5, '8'));
    MSG message{};
    EXPECT_FALSE(PeekMessageW(&message, reinterpret_cast<HWND>(-1), kFirstMessage, kFirstMessage, PM_REMOVE));
    ASSERT_TRUE(PeekMessageW(&message, reinterpret_cast<HWND>(-1), kSecondMessage, kSecondMessage, PM_REMOVE));
    EXPECT_EQ(message.wParam, 5u);
    EXPECT_EQ(message.lParam, '8');
}

TEST_F(HotkeyDeliveryTargetTest, InvalidTargetCannotPostQuitOrAnotherSystemMessage) {
    for (const HotkeyDeliveryTarget target :
         {HotkeyDeliveryTarget(0, kFirstMessage), HotkeyDeliveryTarget(GetCurrentThreadId(), WM_QUIT),
          HotkeyDeliveryTarget(GetCurrentThreadId(), 0x10000)}) {
        EXPECT_FALSE(target.IsValid());
        EXPECT_FALSE(target.Post(1, '9'));
        EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_INVALID_PARAMETER));
    }
    EXPECT_TRUE(HotkeyDeliveryTarget(GetCurrentThreadId(), WM_APP).IsValid());
    EXPECT_TRUE(HotkeyDeliveryTarget(GetCurrentThreadId(), 0xFFFF).IsValid());
}
}  // namespace
