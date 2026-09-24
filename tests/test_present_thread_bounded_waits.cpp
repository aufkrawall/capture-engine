#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "source_fragment_reader.h"

namespace {

std::string ReadSource(const char* relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

}  // namespace

// The focus-loss overlay fence wait runs on the application's present thread.
// After its 2 s bound it used to fall into WaitForSingleObject(INFINITE), so a
// fence that could only complete after this Present returned hung the game for
// good. The bound is final; a still-pending fence only holds later overlay work.
TEST(PresentThreadBoundedWaitTest, FocusLossOverlayFenceWaitNeverWaitsUnbounded) {
    const std::string source = ReadSource("hook/apis/dx12_hook_focus_loss.cpp");
    ASSERT_FALSE(source.empty());
    const size_t start = source.find("bool WaitForFocusLossImmediateOverlayFenceBeforePresent(");
    ASSERT_NE(start, std::string::npos);
    const size_t end = source.find("\nvoid EnsureDx12FaAdapter()", start);
    ASSERT_NE(end, std::string::npos);
    const std::string body = source.substr(start, end - start);
    EXPECT_EQ(body.find("INFINITE"), std::string::npos);
    EXPECT_NE(body.find("WaitForSingleObject(fenceEvent, kFocusLossImmediateFenceDumpTimeoutMs)"), std::string::npos);
    // A timed-out wait leaves the fence pending so later unfocused overlay work is held.
    EXPECT_NE(body.find("dx12_hook_g_FocusLossPendingOverlayFenceValue.store(fenceValue"), std::string::npos);
}

// DXGI calls the destruction callback at reference count zero, on the thread
// releasing the swapchain (frame-generation swapchain recreation). It polled
// Sleep(1) up to 100 times on a counter Present never touches.
TEST(PresentThreadBoundedWaitTest, SwapchainDestructionCallbackNeverSleepPolls) {
    const std::string source = ReadSource("hook/wrappers/dxgi_swapchain_wrap_lifetime.cpp");
    ASSERT_FALSE(source.empty());
    const size_t start = source.find("void WINAPI CWrapDXGISwapChain::DestructionCallback(");
    ASSERT_NE(start, std::string::npos);
    const size_t end = source.find("\nHRESULT CWrapDXGISwapChain::RegisterDestructionCallback()", start);
    ASSERT_NE(end, std::string::npos);
    const std::string body = source.substr(start, end - start);
    EXPECT_EQ(body.find("Sleep(1);"), std::string::npos);
    EXPECT_EQ(body.find("while ("), std::string::npos);
    EXPECT_NE(body.find("std::lock_guard<std::mutex> lock(pSwapChain->m_ResourceLock);"), std::string::npos);
}
