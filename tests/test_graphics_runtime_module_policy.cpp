#include <gtest/gtest.h>

#include "../hook/common/graphics_runtime_module_policy.h"

namespace {

using ce::graphics_runtime::IsRuntimeModuleBaseName;
using ce::graphics_runtime::IsNgxModelRepositoryPath;
using ce::graphics_runtime::ModelSegmentToDllName;

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

}  // namespace
