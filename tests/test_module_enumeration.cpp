#include <gtest/gtest.h>

#include <algorithm>
#include "../common/module_enumeration.h"

namespace {

TEST(ModuleEnumerationTest, ReturnsOnlyCompleteReportedEntries) {
    EXPECT_EQ(ce::GetEnumeratedModuleCount(4, 0), 0u);
    EXPECT_EQ(ce::GetEnumeratedModuleCount(4, sizeof(HMODULE) - 1), 0u);
    EXPECT_EQ(ce::GetEnumeratedModuleCount(4, 2 * sizeof(HMODULE)), 2u);
}

TEST(ModuleEnumerationTest, ClampsRequiredSizeToProvidedStorage) {
    const DWORD requiredBytes = static_cast<DWORD>(9 * sizeof(HMODULE));

    EXPECT_EQ(ce::GetEnumeratedModuleCount(4, requiredBytes), 4u);
}

TEST(ModuleEnumerationTest, GrowsAndRetriesWhenPsapiReportsMoreModules) {
    std::vector<HMODULE> modules;
    int calls = 0;
    const auto enumerate = [&calls](HMODULE*, DWORD, DWORD* bytesNeeded) {
        ++calls;
        *bytesNeeded = static_cast<DWORD>(300 * sizeof(HMODULE));
        return true;
    };

    ASSERT_TRUE(ce::detail::EnumerateProcessModulesGrowing(modules, enumerate));
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(modules.size(), 300u);
}

TEST(ModuleEnumerationTest, EnumeratesCurrentProcessWithoutTruncatingMainModule) {
    std::vector<HMODULE> modules;

    ASSERT_TRUE(ce::EnumerateProcessModules(GetCurrentProcess(), modules));
    EXPECT_NE(std::find(modules.begin(), modules.end(), GetModuleHandleW(nullptr)), modules.end());
}

}  // namespace
