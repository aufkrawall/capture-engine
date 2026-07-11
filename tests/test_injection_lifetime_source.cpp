#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

TEST(InjectionLifetimeSourceTest, DelayedWorkersRemainOwnedUntilJoin) {
    const std::filesystem::path source = std::filesystem::current_path() / "captureengine" / "injection.cpp";
    std::ifstream file(source, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    const std::string text = contents.str();
    ASSERT_FALSE(text.empty());

    EXPECT_EQ(text.find("t.detach()"), std::string::npos);
    EXPECT_NE(text.find("because it still references InjectionManager state"), std::string::npos);
    EXPECT_EQ(text.find("shared_from_this()"), std::string::npos);
    EXPECT_NE(text.find("MarkDoneAndDrain()"), std::string::npos);
    EXPECT_EQ(text.find("Brief delay to let thread pool drain"), std::string::npos);
    const size_t shutdownWmi = text.find("ShutdownWMI();");
    const size_t waitWorkers = text.find("WaitForInjectionThreads(5000);", shutdownWmi);
    ASSERT_NE(shutdownWmi, std::string::npos);
    EXPECT_NE(waitWorkers, std::string::npos);
}
