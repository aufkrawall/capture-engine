#include <gtest/gtest.h>
#include <thread>
#include "../hook/common/swapchain_create_recovery.h"
#include "source_fragment_reader.h"
#include <filesystem>

using ce::swapchain_create::RecoveryScope;

TEST(SwapchainCreateRecoveryTest, InlineDeepAndRetryShareOneOwner) {
    int window = 0;
    RecoveryScope outer(&window);
    EXPECT_TRUE(outer.OwnsRecovery());
    {
        RecoveryScope deep(&window);
        EXPECT_FALSE(deep.OwnsRecovery());
        RecoveryScope retryInline(&window);
        EXPECT_FALSE(retryInline.OwnsRecovery());
    }
    EXPECT_EQ(outer.NestedCalls(), 2u);
    RecoveryScope secondRetry(&window);
    EXPECT_FALSE(secondRetry.OwnsRecovery());
}

TEST(SwapchainCreateRecoveryTest, IndependentWindowsAndThreadsKeepTheirOwnRecovery) {
    int first = 0, second = 0;
    RecoveryScope outer(&first);
    RecoveryScope unrelated(&second);
    EXPECT_TRUE(unrelated.OwnsRecovery());
    RecoveryScope reentered(&first);
    EXPECT_FALSE(reentered.OwnsRecovery());
    bool threadOwns = false;
    std::thread worker([&]() { RecoveryScope scope(&first); threadOwns = scope.OwnsRecovery(); });
    worker.join();
    EXPECT_TRUE(threadOwns);
}

TEST(SwapchainCreateRecoveryTest, CompletedCallDoesNotSuppressNextCall) {
    int window = 0;
    { RecoveryScope scope(&window); }
    RecoveryScope next(&window);
    EXPECT_TRUE(next.OwnsRecovery());
    EXPECT_EQ(next.NestedCalls(), 0u);
}

TEST(SwapchainCreateRecoveryTest, BothHookLayersGateRecoveryAndHaveNoSleepRetries) {
    for (const char* file : {"hook/apis/dx12_hook_swapchain_create.cpp", "hook/apis/dx12_hook_swapchain_tracking.cpp"}) {
        const auto source = ce::test_source::ReadLogicalSource(std::filesystem::current_path() / file);
        EXPECT_NE(source.find("RecoveryScope recoveryScope(hWnd)"), std::string::npos);
        EXPECT_NE(source.find("hr == E_ACCESSDENIED && hWnd && recoveryScope.OwnsRecovery()"), std::string::npos);
        EXPECT_EQ(source.find("Sleep(20)"), std::string::npos);
        EXPECT_EQ(source.find("attempt <= 10"), std::string::npos);
    }
}
