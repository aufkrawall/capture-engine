// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// The package is the folder of the binary the code is linked into, and the role host is the runtime's choice
// (common/platform/package_module.h). In this test image both are the test executable.
#include "common/platform/package_module.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace {

struct RoleHostReset {
    ~RoleHostReset() {
        try {
            ce::package::SetRoleHostExecutable({});
        } catch (...) {
            // Destructor must not throw: best-effort reset.
            (void)0;
        }
    }
};

}  // namespace

TEST(PackageModuleTest, ThePackageIsTheFolderOfTheModuleContainingTheCode) {
    const std::wstring self = ce::ansi_path::ModulePathW(nullptr);
    ASSERT_FALSE(self.empty());
    EXPECT_EQ(ce::package::PackageModule(), GetModuleHandleW(nullptr)) << "the test image links this code itself";
    EXPECT_EQ(ce::package::PackageModulePath(), self);
    EXPECT_EQ(ce::package::PackageDirectory(), std::filesystem::path(self).parent_path().wstring());
}

TEST(PackageModuleTest, TheRoleHostIsTheOwnExecutableUntilTheRuntimeChoosesOne) {
    RoleHostReset reset;
    ce::package::SetRoleHostExecutable({});
    EXPECT_EQ(ce::package::RoleHostExecutable(), ce::ansi_path::ModulePathW(nullptr));
    ce::package::SetRoleHostExecutable(L"C:\\Package\\captureengine.exe");
    EXPECT_EQ(ce::package::RoleHostExecutable(), L"C:\\Package\\captureengine.exe");
}
