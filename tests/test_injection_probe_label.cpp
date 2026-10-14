// Regression: with early injection the module probe fails with
// ERROR_PARTIAL_COPY, and session 20261014_204616 still labelled Witcher 3
// (D3D12) "non-D3D12 (DX11/DX9/Vulkan) detected".
#include <windows.h>

#include "captureengine/injection/injection_policy.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using ce::injection_policy::GraphicsProbeLabel;

TEST(InjectionProbeLabel, UnprobedModuleListMakesNoApiClaim) {
    const std::string early = GraphicsProbeLabel(false, false, ERROR_PARTIAL_COPY);
    EXPECT_NE(early.find("not probed"), std::string::npos) << early;
    EXPECT_NE(early.find("loader init"), std::string::npos) << early;
    EXPECT_EQ(early.find("non-D3D12"), std::string::npos) << early;

    const std::string denied = GraphicsProbeLabel(false, false, ERROR_ACCESS_DENIED);
    EXPECT_NE(denied.find("not probed"), std::string::npos) << denied;
    EXPECT_EQ(denied.find("loader init"), std::string::npos) << denied;
}

TEST(InjectionProbeLabel, ProbedModuleListReportsWhatItSaw) {
    EXPECT_STREQ(GraphicsProbeLabel(true, true, 0), "D3D12 detected");
    const std::string withoutD3D12 = GraphicsProbeLabel(false, true, 0);
    EXPECT_NE(withoutD3D12.find("no D3D12 module loaded yet"), std::string::npos) << withoutD3D12;
}

}  // namespace
