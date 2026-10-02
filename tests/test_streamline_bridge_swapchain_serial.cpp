#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <thread>
#include <vector>

#include "hook/streamline/streamline_bridge_swapchain_serial.h"

// Session 20261001_153717 (Witcher 3, streamline_upgrade=true, DLSS-G on): alt-tab crashed the game.
// The window thread's SetFullscreenState(FALSE) ran 2.x sl.dlss_g's SetFullscreenStatePre, which
// force-destroyed the back-buffer wrappers while the render thread was inside sl.dlss_g's Present
// hook; that present then read the freed NativeBackBuffer[1]. 2.x's interposer leaves serializing
// swapchain calls to the title. The bridge now runs those hooks one at a time.
namespace {

namespace bridge = ce::streamline_bridge;

TEST(StreamlineBridgeSwapchainSerial, ReentersOnTheOwningThreadWithoutWaiting) {
    // sl.dlss_g's slHookPresent calls its own (also serialized) slHookPresent1.
    bridge::SwapchainCallSerializer serializer;
    EXPECT_FALSE(serializer.Enter());
    EXPECT_FALSE(serializer.Enter());
    EXPECT_EQ(serializer.DepthForTest(), 2u);
    serializer.Leave();
    EXPECT_EQ(serializer.DepthForTest(), 1u);
    serializer.Leave();
    EXPECT_EQ(serializer.DepthForTest(), 0u);
}

TEST(StreamlineBridgeSwapchainSerial, LeaveFromANonOwnerIsIgnored) {
    bridge::SwapchainCallSerializer serializer;
    serializer.Enter();
    std::thread other([&] { serializer.Leave(); });
    other.join();
    EXPECT_EQ(serializer.DepthForTest(), 1u);
    serializer.Leave();
    EXPECT_EQ(serializer.DepthForTest(), 0u);
}

TEST(StreamlineBridgeSwapchainSerial, NeverAdmitsTwoThreadsAtOnce) {
    bridge::SwapchainCallSerializer serializer;
    std::atomic<int> inside{0};
    std::atomic<int> maxInside{0};
    uint64_t counter = 0;  // deliberately unsynchronized: the serializer is the only guard
    constexpr int kThreads = 4;
    constexpr int kIterations = 5000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kIterations; ++i) {
                bridge::SwapchainCallScope scope(serializer);
                const int now = inside.fetch_add(1) + 1;
                int seen = maxInside.load();
                while (now > seen && !maxInside.compare_exchange_weak(seen, now)) {
                }
                ++counter;
                inside.fetch_sub(1);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(maxInside.load(), 1);
    EXPECT_EQ(counter, static_cast<uint64_t>(kThreads) * kIterations);
    EXPECT_EQ(serializer.DepthForTest(), 0u);
}

std::atomic<bool> g_sentMessageHandled{false};

LRESULT CALLBACK SerialTestWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_APP + 7) {
        g_sentMessageHandled.store(true);
        return 42;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// The deadlock the serializer must not add: the holder (the render thread inside Present) makes DXGI
// SendMessage to the window thread, which is itself waiting to enter (SetFullscreenState). The wait
// must dispatch the sent message; a plain mutex wait would hang both threads.
TEST(StreamlineBridgeSwapchainSerial, WaitingWindowThreadServicesSentMessagesFromTheHolder) {
    g_sentMessageHandled.store(false);
    bridge::SwapchainCallSerializer serializer;

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = &SerialTestWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"CeStreamlineBridgeSerialTest";
    RegisterClassW(&windowClass);

    HANDLE windowReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE holderEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(windowReady, nullptr);
    ASSERT_NE(holderEntered, nullptr);

    HWND window = nullptr;
    std::atomic<bool> holderLeft{false};
    bool windowThreadWaited = false;
    bool holderHadLeftWhenAdmitted = false;

    std::thread windowThread([&] {
        window = CreateWindowExW(0, windowClass.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                 windowClass.hInstance, nullptr);
        SetEvent(windowReady);
        // A plain wait: no sent message is delivered until the serializer's own wait.
        WaitForSingleObject(holderEntered, INFINITE);
        windowThreadWaited = serializer.Enter();
        holderHadLeftWhenAdmitted = holderLeft.load();
        serializer.Leave();
        DestroyWindow(window);
    });

    WaitForSingleObject(windowReady, INFINITE);
    ASSERT_NE(window, nullptr);
    serializer.Enter();
    SetEvent(holderEntered);
    // Only a failure safety net: a non-pumping wait would otherwise hang the suite.
    DWORD_PTR reply = 0;
    const LRESULT sent = SendMessageTimeoutW(window, WM_APP + 7, 0, 0, SMTO_NORMAL, 30000, &reply);
    holderLeft.store(true);
    serializer.Leave();
    windowThread.join();

    EXPECT_NE(sent, 0);
    EXPECT_EQ(reply, 42u);
    EXPECT_TRUE(g_sentMessageHandled.load());
    EXPECT_TRUE(windowThreadWaited);
    EXPECT_TRUE(holderHadLeftWhenAdmitted);
    EXPECT_EQ(serializer.DepthForTest(), 0u);

    CloseHandle(windowReady);
    CloseHandle(holderEntered);
    UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
}

}  // namespace
