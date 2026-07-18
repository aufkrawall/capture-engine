#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "../common/process_identity.h"

namespace {

std::string ReadSource(const std::filesystem::path& relativePath) {
    std::ifstream stream(std::filesystem::current_path() / relativePath, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST(ProcessIdentityTest, ResolvesCurrentExecutableThroughLimitedQueryInterface) {
    const ce::process::ProcessIdentityResult identity = ce::process::QueryProcessIdentity(GetCurrentProcessId());
    ASSERT_TRUE(identity) << identity.error;
    EXPECT_NE(identity.imageName.find("unit_tests"), std::string::npos);
    EXPECT_EQ(identity.error, ERROR_SUCCESS);
}

TEST(ProcessIdentityTest, FailureCannotReturnAPartiallyOverwrittenSentinel) {
    const ce::process::ProcessIdentityResult identity = ce::process::QueryProcessIdentity(UINT32_MAX);
    EXPECT_FALSE(identity);
    EXPECT_TRUE(identity.imageName.empty());
    EXPECT_NE(identity.error, ERROR_SUCCESS);
}

TEST(ProcessIdentityTest, ProductionIdentityQueryDoesNotRequestTargetMemoryAccess) {
    const std::string source = ReadSource("common/process_identity.cpp");
    const std::string inject = ReadSource("captureengine/inject_main.cpp");
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(inject.empty());
    EXPECT_NE(source.find("PROCESS_QUERY_LIMITED_INFORMATION"), std::string::npos);
    EXPECT_EQ(source.find("PROCESS_VM_READ"), std::string::npos);
    EXPECT_EQ(inject.find("GetModuleBaseNameA"), std::string::npos);
    EXPECT_EQ(inject.find("PROCESS_VM_READ"), std::string::npos);
}
