#include <gtest/gtest.h>

#include <filesystem>

#include "../hook/common/graphics_runtime_module_policy.h"
#include "source_fragment_reader.h"

namespace {

using ce::graphics_runtime::EqualsModulePathIgnoreCase;
using ce::graphics_runtime::IsRuntimeModuleBaseName;
using ce::graphics_runtime::IsNgxModelRepositoryPath;
using ce::graphics_runtime::ModelSegmentToDllName;
using ce::graphics_runtime::WouldRedirectDuplicateLoadedModule;

// The full family CE can redirect through the per-profile override paths
// (dlss_sr_dll_path, dlss_fg_dll_path, dlss_rr_dll_path, streamline_dll_path).
TEST(GraphicsRuntimeModulePolicy, RecognizesStreamlineFamily) {
    for (const char* name : {"sl.interposer.dll", "sl.common.dll", "sl.dlss.dll", "sl.dlss_g.dll",
                             "sl.dlss_d.dll", "sl.directsr.dll", "sl.nis.dll", "sl.nvperf.dll",
                             "sl.reflex.dll", "sl.pcl.dll", "sl.deepdvc.dll"}) {
        EXPECT_TRUE(IsRuntimeModuleBaseName(name)) << name;
        EXPECT_TRUE(IsRuntimeModuleBaseName((std::string("C:\\npi\\sl\\") + name).c_str())) << name;
    }
}

TEST(GraphicsRuntimeModulePolicy, RecognizesNgxCoreAndSnippets) {
    for (const char* name : {"nvngx.dll", "_nvngx.dll", "nvngx_dlss.dll", "nvngx_dlssg.dll",
                             "nvngx_dlssd.dll", "nvngx_deepdvc.dll", "nvlowlatencyvk.dll",
                             "nvapi64.dll", "nvapi.dll"}) {
        EXPECT_TRUE(IsRuntimeModuleBaseName(name)) << name;
        EXPECT_TRUE(IsRuntimeModuleBaseName((std::string("D:\\games\\app\\") + name).c_str())) << name;
    }
}

TEST(GraphicsRuntimeModulePolicy, IsCaseInsensitive) {
    EXPECT_TRUE(IsRuntimeModuleBaseName("SL.INTERPOSER.DLL"));
    EXPECT_TRUE(IsRuntimeModuleBaseName("Sl.Dlss_G.dll"));
    EXPECT_TRUE(IsRuntimeModuleBaseName("NVNGX_DLSS.DLL"));
    EXPECT_TRUE(IsRuntimeModuleBaseName("NvLowLatencyVk.dll"));
}

// The classifier mirrors the loader redirect's match semantics: the sl.*
// prefix rule is intentionally broad (a new sl.* plugin is redirected and
// logged without code changes), while non-prefix near-misses must never
// qualify.
TEST(GraphicsRuntimeModulePolicy, RejectsUnrelatedNames) {
    for (const char* name : {"", "sl", "xsl.dlss.dll", "sldlss.dll", "dlss.dll", "nvngx",
                             "nvngx.dll.bak", "xnvngx_dlss.dll", "nvngx_dlss.dllx",
                             "nvngx_dlssg.dllx", "nvapi", "nvapi64.dllx", "d3d12.dll",
                             "gameoverlayrenderer64.dll"}) {
        EXPECT_FALSE(IsRuntimeModuleBaseName(name)) << name;
    }
    EXPECT_FALSE(IsRuntimeModuleBaseName(nullptr));
}

// Prefix rule parity with the loader redirect: "sl.dll" and a hypothetical
// "sl.<new>.dll" are treated as Streamline-family names, exactly like
// GetRedirectedPath classifies them.
TEST(GraphicsRuntimeModulePolicy, PrefixRuleMatchesLoaderRedirectSemantics) {
    EXPECT_TRUE(IsRuntimeModuleBaseName("sl.dll"));
    EXPECT_TRUE(IsRuntimeModuleBaseName("sl.something_new.dll"));
}

// The NGX model repository stores the Streamline plugins under hashed file
// names; the redirect must map the model folder back to the real DLL name so
// the configured override can replace the driver-managed plugin.
TEST(GraphicsRuntimeModulePolicy, MapsNgxModelSegmentsToStreamlineDllNames) {
    const struct {
        const char* segment;
        const char* expected;
    } cases[] = {
        {"sl_dlss_g_0", "sl.dlss_g.dll"},
        {"sl_dlss_0", "sl.dlss.dll"},
        {"sl_dlss_d_0", "sl.dlss_d.dll"},
        {"sl_common_0", "sl.common.dll"},
        {"sl_deepdvc_0", "sl.deepdvc.dll"},
        {"sl_nis_0", "sl.nis.dll"},
        {"sl_nvperf_0", "sl.nvperf.dll"},
        {"sl_pcl_0", "sl.pcl.dll"},
        {"sl_reflex_0", "sl.reflex.dll"},
    };
    for (const auto& entry : cases) {
        char out[64] = {};
        EXPECT_TRUE(ModelSegmentToDllName(entry.segment, out, sizeof(out))) << entry.segment;
        EXPECT_STREQ(out, entry.expected) << entry.segment;
    }
}

TEST(GraphicsRuntimeModulePolicy, RejectsMalformedNgxModelSegments) {
    char out[64] = {};
    for (const char* segment : {"", "sl", "sl_", "sl__0", "sl_0", "dlss_g_0", "sl_dlss_g",
                                "sl_dlss_g_x", "sl_dlss_g_0_extra", "x_sl_dlss_g_0"}) {
        EXPECT_FALSE(ModelSegmentToDllName(segment, out, sizeof(out))) << segment;
    }
    EXPECT_FALSE(ModelSegmentToDllName(nullptr, out, sizeof(out)));
    EXPECT_FALSE(ModelSegmentToDllName("sl_dlss_g_0", nullptr, 64));
    EXPECT_FALSE(ModelSegmentToDllName("sl_dlss_g_0", out, 8));
}

TEST(GraphicsRuntimeModulePolicy, RecognizesNgxModelRepositoryPaths) {
    EXPECT_TRUE(IsNgxModelRepositoryPath(
        "C:\\ProgramData\\NVIDIA\\NGX\\models\\sl_dlss_g_0\\versions\\133888\\files\\1B0_E658703.dll"));
    EXPECT_TRUE(IsNgxModelRepositoryPath(
        "C:/ProgramData/NVIDIA/NGX/models/sl_common_0/versions/1/files/x.dll"));
    EXPECT_FALSE(IsNgxModelRepositoryPath("C:\\games\\app\\sl.dlss_g.dll"));
    EXPECT_FALSE(IsNgxModelRepositoryPath("C:\\ProgramData\\NVIDIA\\NGX\\notmodels\\x.dll"));
    EXPECT_FALSE(IsNgxModelRepositoryPath(nullptr));
    EXPECT_FALSE(IsNgxModelRepositoryPath(""));
}

TEST(GraphicsRuntimeModulePolicy, ComparesModulePathsTheWayTheLoaderKeysIdentity) {
    EXPECT_TRUE(EqualsModulePathIgnoreCase("C:\\npi\\sl\\sl.common.dll", "c:\\NPI\\SL\\SL.COMMON.DLL"));
    EXPECT_TRUE(EqualsModulePathIgnoreCase("C:/npi/sl/sl.common.dll", "C:\\npi\\sl\\sl.common.dll"));
    EXPECT_FALSE(EqualsModulePathIgnoreCase("C:\\npi\\sl\\sl.common.dll", "H:\\game\\bin\\sl.common.dll"));
    EXPECT_FALSE(EqualsModulePathIgnoreCase("C:\\npi\\sl\\sl.common.dll", "C:\\npi\\sl\\sl.common.dll.bak"));
    EXPECT_FALSE(EqualsModulePathIgnoreCase(nullptr, "x"));
    EXPECT_FALSE(EqualsModulePathIgnoreCase("x", nullptr));
}

// Cyberpunk 20260816_045933: CE pinned the live sl.interposer by re-loading its own full path,
// this redirect rewrote the pin to the override directory, and the loader mapped the override
// copy as a SECOND sl.interposer instance. CE hooked the duplicate — overwriting the single
// process-global forward pointers for slSetTag/slSetD3DDevice/slEvaluateFeature — then freed it
// again, after which the game's Streamline calls returned kSlResultErrorInvalidState without
// ever reaching Streamline and sl.dlss_g dereferenced null.
TEST(GraphicsRuntimeModulePolicy, RefusesRedirectThatWouldDuplicateAnAlreadyLoadedRuntime) {
    EXPECT_TRUE(WouldRedirectDuplicateLoadedModule("C:\\npi\\sl\\sl.interposer.dll", true,
                                                   "H:\\game\\bin\\x64\\sl.interposer.dll"));
    // The name is loaded but its path is unresolvable: fail closed, exactly like the preload,
    // which skips any base name that is already loaded.
    EXPECT_TRUE(WouldRedirectDuplicateLoadedModule("C:\\npi\\sl\\sl.interposer.dll", true, ""));
    EXPECT_TRUE(WouldRedirectDuplicateLoadedModule("C:\\npi\\sl\\sl.interposer.dll", true, nullptr));
}

// The override must still win every load it can actually win, or the whole feature dies with the
// fix: a name that is NOT loaded yet, and a repeat load of the override copy itself (Streamline
// and NGX both re-request the same plugin several times per startup), keep redirecting.
TEST(GraphicsRuntimeModulePolicy, KeepsRedirectingWhenNoDuplicateInstanceCanResult) {
    EXPECT_FALSE(WouldRedirectDuplicateLoadedModule("C:\\npi\\sl\\sl.dlss_g.dll", false, nullptr));
    EXPECT_FALSE(WouldRedirectDuplicateLoadedModule("C:\\npi\\sl\\sl.dlss_g.dll", false,
                                                    "H:\\game\\bin\\x64\\sl.dlss_g.dll"));
    EXPECT_FALSE(
        WouldRedirectDuplicateLoadedModule("C:\\npi\\sl\\sl.common.dll", true, "C:\\npi\\sl\\sl.common.dll"));
    EXPECT_FALSE(
        WouldRedirectDuplicateLoadedModule("C:\\npi\\sl\\sl.common.dll", true, "c:/NPI/sl/SL.COMMON.DLL"));
    EXPECT_FALSE(WouldRedirectDuplicateLoadedModule(nullptr, true, "H:\\game\\bin\\x64\\sl.common.dll"));
    EXPECT_FALSE(WouldRedirectDuplicateLoadedModule("", true, "H:\\game\\bin\\x64\\sl.common.dll"));
}

// Both redirect decisions in GetRedirectedPath must consult the duplicate check: the NGX
// model-repository branch returns early with its own path, so guarding only the generic branch
// would leave the driver-managed plugin loads (which is how sl.common/sl.reflex arrive) unguarded.
TEST(GraphicsRuntimeModulePolicy, LoaderRedirectGuardsBothDecisionsAgainstDuplicateInstances) {
    namespace fs = std::filesystem;
    const std::string redirect = ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "main_redirect.cpp");
    ASSERT_FALSE(redirect.empty());

    EXPECT_NE(redirect.find("WouldRedirectDuplicateLoadedModule("), std::string::npos);

    const size_t modelReturn = redirect.find("return modelFinal;");
    ASSERT_NE(modelReturn, std::string::npos);
    const size_t modelGuard = redirect.rfind("RedirectWouldDuplicateLoadedModule(modelFinal)", modelReturn);
    EXPECT_NE(modelGuard, std::string::npos);

    const size_t genericReturn = redirect.find("return finalPath;");
    ASSERT_NE(genericReturn, std::string::npos);
    const size_t genericGuard = redirect.rfind("RedirectWouldDuplicateLoadedModule(finalPath)", genericReturn);
    EXPECT_NE(genericGuard, std::string::npos);
}

}  // namespace
