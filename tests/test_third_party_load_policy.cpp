#include <gtest/gtest.h>

#include <string>

#include "../hook/common/third_party_load_policy.h"

namespace {

using ce::third_party_load::DefaultDllBaseName;
using ce::third_party_load::HasAnyThirdPartyLoadConfigured;
using ce::third_party_load::IsKnownModuleBaseNameForTool;
using ce::third_party_load::ResolveThirdPartyDllPath;
using ce::third_party_load::Tool;

TEST(ThirdPartyLoadPolicyTest, DefaultDllBaseNamesFollowArchitecture) {
    EXPECT_STREQ(DefaultDllBaseName(Tool::kReShade, true), "ReShade64.dll");
    EXPECT_STREQ(DefaultDllBaseName(Tool::kReShade, false), "ReShade32.dll");
    EXPECT_STREQ(DefaultDllBaseName(Tool::kSpecialK, true), "SpecialK64.dll");
    EXPECT_STREQ(DefaultDllBaseName(Tool::kSpecialK, false), "SpecialK32.dll");
    EXPECT_STREQ(DefaultDllBaseName(Tool::kOptiScaler, true), "OptiScaler.dll");
    EXPECT_STREQ(DefaultDllBaseName(Tool::kOptiScaler, false), "OptiScaler.dll");
}

TEST(ThirdPartyLoadPolicyTest, DirectoryResolutionAppendsPerBitnessName) {
    EXPECT_EQ(ResolveThirdPartyDllPath(Tool::kReShade, R"(C:\tools\reshade)", true),
              R"(C:\tools\reshade\ReShade64.dll)");
    EXPECT_EQ(ResolveThirdPartyDllPath(Tool::kReShade, R"(C:\tools\reshade\)", false),
              R"(C:\tools\reshade\ReShade32.dll)");
    EXPECT_EQ(ResolveThirdPartyDllPath(Tool::kOptiScaler, R"(C:\tools\opti)", false),
              R"(C:\tools\opti\OptiScaler.dll)");
}

TEST(ThirdPartyLoadPolicyTest, FilePathsLoadVerbatim) {
    EXPECT_EQ(ResolveThirdPartyDllPath(Tool::kSpecialK, R"(C:\tools\SpecialK64.dll)", true),
              R"(C:\tools\SpecialK64.dll)");
    EXPECT_EQ(ResolveThirdPartyDllPath(Tool::kReShade, R"(C:\tools\reshade\dxgi.dll)", true),
              R"(C:\tools\reshade\dxgi.dll)");
}

TEST(ThirdPartyLoadPolicyTest, EmptyPathsResolveEmptyAndAreNotConfigured) {
    EXPECT_TRUE(ResolveThirdPartyDllPath(Tool::kReShade, "", true).empty());
    EXPECT_FALSE(HasAnyThirdPartyLoadConfigured("", "", ""));
    EXPECT_FALSE(HasAnyThirdPartyLoadConfigured(nullptr, nullptr, nullptr));
    EXPECT_TRUE(HasAnyThirdPartyLoadConfigured("", R"(C:\reshade)", ""));
}

TEST(ThirdPartyLoadPolicyTest, LoadOrderConstantIsSpecialKThenReShadeThenOptiScaler) {
    // Special K loads FIRST. Later tools' DllMains pass through Special K's
    // thread-creation hook, so the executor suspends all peer threads around
    // every load after the first — Special K's enumerator then cannot hold its
    // critical section across a loader call (sessions 20260813_020236 /
    // 20260813_021731 / 20260813_025615 / 20260813_031321).
    EXPECT_EQ(static_cast<int>(Tool::kSpecialK), 0);
    EXPECT_EQ(static_cast<int>(Tool::kReShade), 1);
    EXPECT_EQ(static_cast<int>(Tool::kOptiScaler), 2);
    EXPECT_EQ(static_cast<int>(Tool::kCount), 3);
    EXPECT_STREQ(ce::third_party_load::ToolName(Tool::kSpecialK), "SpecialK");
    EXPECT_STREQ(ce::third_party_load::ToolName(Tool::kReShade), "ReShade");
    EXPECT_STREQ(ce::third_party_load::ToolName(Tool::kOptiScaler), "OptiScaler");
}

TEST(ThirdPartyLoadPolicyTest, LoaderQuiescenceWaitPrecedesEveryToolAfterTheFirst) {
    EXPECT_FALSE(ce::third_party_load::ShouldWaitForLoaderQuiescenceBeforeToolLoad(0));
    EXPECT_TRUE(ce::third_party_load::ShouldWaitForLoaderQuiescenceBeforeToolLoad(1));
    EXPECT_TRUE(ce::third_party_load::ShouldWaitForLoaderQuiescenceBeforeToolLoad(2));
}

TEST(ThirdPartyLoadPolicyTest, PeerThreadSuspensionGuardsEveryToolAfterTheFirst) {
    EXPECT_FALSE(ce::third_party_load::ShouldSuspendPeerThreadsForToolLoad(0));
    EXPECT_TRUE(ce::third_party_load::ShouldSuspendPeerThreadsForToolLoad(1));
    EXPECT_TRUE(ce::third_party_load::ShouldSuspendPeerThreadsForToolLoad(2));
}

TEST(ThirdPartyLoadPolicyTest, KnownBaseNamesMatchTheirToolOnly) {
    EXPECT_TRUE(IsKnownModuleBaseNameForTool(Tool::kReShade, "ReShade64.dll"));
    EXPECT_TRUE(IsKnownModuleBaseNameForTool(Tool::kReShade, "ReShade32.dll"));
    EXPECT_TRUE(IsKnownModuleBaseNameForTool(Tool::kReShade, "ReShade.dll"));
    EXPECT_FALSE(IsKnownModuleBaseNameForTool(Tool::kReShade, "SpecialK64.dll"));
    EXPECT_FALSE(IsKnownModuleBaseNameForTool(Tool::kReShade, "OptiScaler.dll"));
    EXPECT_TRUE(IsKnownModuleBaseNameForTool(Tool::kOptiScaler, "OptiScaler.dll"));
    EXPECT_TRUE(IsKnownModuleBaseNameForTool(Tool::kOptiScaler, "OptiScaler.asi"));
    EXPECT_FALSE(IsKnownModuleBaseNameForTool(Tool::kOptiScaler, "ReShade64.dll"));
    EXPECT_TRUE(IsKnownModuleBaseNameForTool(Tool::kSpecialK, "SpecialK32.dll"));
    EXPECT_TRUE(IsKnownModuleBaseNameForTool(Tool::kSpecialK, "SpecialK.dll"));
    EXPECT_FALSE(IsKnownModuleBaseNameForTool(Tool::kSpecialK, "ReShade64.dll"));
}

TEST(ThirdPartyLoadPolicyTest, RenamedProxyNamesAreSharedWithOverlayDetection) {
    EXPECT_TRUE(ce::overlay_compat::IsThirdPartyGraphicsProxyCandidateName("dxgi.dll"));
    EXPECT_TRUE(ce::overlay_compat::IsThirdPartyGraphicsProxyCandidateName("d3d12.dll"));
    EXPECT_TRUE(ce::overlay_compat::IsThirdPartyGraphicsProxyCandidateName("OptiScaler.dll"));
    EXPECT_TRUE(ce::overlay_compat::IsThirdPartyGraphicsProxyCandidateName("DXGI.DLL"));
    EXPECT_FALSE(ce::overlay_compat::IsThirdPartyGraphicsProxyCandidateName("kernel32.dll"));
    EXPECT_FALSE(ce::overlay_compat::IsThirdPartyGraphicsProxyCandidateName("ReShade64.dll"));
}

}  // namespace
