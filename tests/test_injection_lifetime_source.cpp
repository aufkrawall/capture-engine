#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "source_fragment_reader.h"

TEST(InjectionLifetimeSourceTest, DelayedWorkersRemainOwnedUntilJoin) {
    const std::filesystem::path source = std::filesystem::current_path() / "captureengine" / "injection.cpp";
    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());

    EXPECT_EQ(text.find("t.detach()"), std::string::npos);
    EXPECT_EQ(text.find("detaching to avoid indefinite block"), std::string::npos);
    EXPECT_NE(text.find("t.join()"), std::string::npos);
    EXPECT_NE(text.find("to preserve manager lifetime"), std::string::npos);
    EXPECT_EQ(text.find("shared_from_this()"), std::string::npos);
    EXPECT_NE(text.find("MarkDoneAndDrain()"), std::string::npos);
    EXPECT_EQ(text.find("Brief delay to let thread pool drain"), std::string::npos);
    const size_t shutdownWmi = text.find("ShutdownWMI();");
    const size_t waitWorkers = text.find("WaitForInjectionThreads(5000);", shutdownWmi);
    ASSERT_NE(shutdownWmi, std::string::npos);
    EXPECT_NE(waitWorkers, std::string::npos);
}
