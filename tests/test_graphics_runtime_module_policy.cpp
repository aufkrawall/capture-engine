#include <gtest/gtest.h>

#include <filesystem>

#include "../hook/common/graphics_runtime_module_policy.h"
#include "source_fragment_reader.h"

namespace {

using ce::graphics_runtime::EqualsModulePathIgnoreCase;
using ce::graphics_runtime::IsRuntimeModuleBaseName;
using ce::graphics_runtime::IsBridgeNgxFeatureModuleName;
using ce::graphics_runtime::IsNgxModelRepositoryPath;
using ce::graphics_runtime::IsStreamlineCoreProvidedDllName;
using ce::graphics_runtime::ModelSegmentToDllName;
using ce::graphics_runtime::NgxModelSegment;
using ce::graphics_runtime::ResolveStreamlineProvidedDllName;
using ce::graphics_runtime::ShouldApplyStreamlineOverrideRedirect;
using ce::graphics_runtime::ShouldRetireLegacyBridgeNgxFeatureModule;
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

TEST(GraphicsRuntimeModulePolicy, IdentifiesOnlyTheNgxFeaturesOwnedByTheGenerationBridge) {
    EXPECT_TRUE(IsBridgeNgxFeatureModuleName("nvngx_dlss.dll"));
    EXPECT_TRUE(IsBridgeNgxFeatureModuleName("C:\\npi\\sl\\NVNGX_DLSSG.DLL"));
    const char* rejected[] = {"_nvngx.dll", "nvngx.dll", "nvngx_dlssd.dll",
                              "nvngx_deepdvc.dll", "sl.dlss.dll", nullptr};
    for (const char* name : rejected) {
        EXPECT_FALSE(IsBridgeNgxFeatureModuleName(name));
    }
}

TEST(GraphicsRuntimeModulePolicy, RetiresOnlyForeignNgxFeaturesAfterLegacyShutdown) {
    EXPECT_TRUE(ShouldRetireLegacyBridgeNgxFeatureModule(
        true, "C:\\npi\\sl\\nvngx_dlss.dll", "H:\\game\\nvngx_dlss.dll"));
    EXPECT_TRUE(ShouldRetireLegacyBridgeNgxFeatureModule(
        true, "C:\\npi\\sl\\nvngx_dlssg.dll", "C:\\DriverStore\\nvngx_dlssg.dll"));

    EXPECT_FALSE(ShouldRetireLegacyBridgeNgxFeatureModule(
        false, "C:\\npi\\sl\\nvngx_dlss.dll", "H:\\game\\nvngx_dlss.dll"));
    EXPECT_FALSE(ShouldRetireLegacyBridgeNgxFeatureModule(
        true, "C:\\npi\\sl\\nvngx_dlss.dll", "c:/NPI/sl/NVNGX_DLSS.DLL"));
    EXPECT_FALSE(ShouldRetireLegacyBridgeNgxFeatureModule(
        true, "C:\\npi\\sl\\nvngx_dlss.dll", "H:\\game\\nvngx_dlssg.dll"));
    EXPECT_FALSE(ShouldRetireLegacyBridgeNgxFeatureModule(
        true, "C:\\npi\\sl\\nvngx_dlssd.dll", "H:\\game\\nvngx_dlssd.dll"));
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

TEST(GraphicsRuntimeModulePolicy, ExtractsNgxModelSegments) {
    char segment[64] = {};
    EXPECT_TRUE(NgxModelSegment(
        "C:\\ProgramData\\NVIDIA\\NGX\\models\\sl_common_0\\versions\\133888\\files\\1B0_E658703.dll", segment,
        sizeof(segment)));
    EXPECT_STREQ(segment, "sl_common_0");
    EXPECT_TRUE(NgxModelSegment("C:/ProgramData/NVIDIA/NGX/models/sl_dlss_g_0/versions/1/files/x.dll", segment,
                                sizeof(segment)));
    EXPECT_STREQ(segment, "sl_dlss_g_0");
    EXPECT_FALSE(NgxModelSegment("C:\\games\\app\\sl.dlss_g.dll", segment, sizeof(segment)));
    EXPECT_FALSE(NgxModelSegment(nullptr, segment, sizeof(segment)));
    EXPECT_FALSE(NgxModelSegment("C:\\ProgramData\\NVIDIA\\NGX\\models\\sl_common_0\\x.dll", segment, 4));
}

// Which Streamline plugin an image provides can only be answered from the resolved full path:
// the driver's NGX cache stores every plugin under the same hashed base name.
TEST(GraphicsRuntimeModulePolicy, ResolvesTheStreamlinePluginAnImageProvides) {
    char provided[64] = {};
    EXPECT_TRUE(ResolveStreamlineProvidedDllName(
        "C:\\ProgramData\\NVIDIA\\NGX\\models\\sl_common_0\\versions\\133888\\files\\1B0_E658703.dll", provided,
        sizeof(provided)));
    EXPECT_STREQ(provided, "sl.common.dll");
    EXPECT_TRUE(ResolveStreamlineProvidedDllName(
        "C:\\ProgramData\\NVIDIA\\NGX\\models\\sl_reflex_0\\versions\\133888\\files\\1B0_E658703.dll", provided,
        sizeof(provided)));
    EXPECT_STREQ(provided, "sl.reflex.dll");
    EXPECT_TRUE(ResolveStreamlineProvidedDllName("H:\\game\\bin\\x64\\SL.Common.DLL", provided, sizeof(provided)));
    EXPECT_STREQ(provided, "sl.common.dll");

    EXPECT_FALSE(ResolveStreamlineProvidedDllName("H:\\game\\bin\\x64\\nvngx_dlss.dll", provided, sizeof(provided)));
    EXPECT_FALSE(ResolveStreamlineProvidedDllName(
        "C:\\ProgramData\\NVIDIA\\NGX\\models\\dlss\\versions\\131844\\files\\160_B9DB490.bin", provided,
        sizeof(provided)));
    EXPECT_FALSE(ResolveStreamlineProvidedDllName(nullptr, provided, sizeof(provided)));
    EXPECT_FALSE(ResolveStreamlineProvidedDllName("H:\\game\\bin\\x64\\sl.common.dll", provided, 4));

    EXPECT_TRUE(IsStreamlineCoreProvidedDllName("sl.common.dll"));
    EXPECT_TRUE(IsStreamlineCoreProvidedDllName("C:\\npi\\sl\\SL.COMMON.DLL"));
    EXPECT_FALSE(IsStreamlineCoreProvidedDllName("sl.reflex.dll"));
    EXPECT_FALSE(IsStreamlineCoreProvidedDllName("sl.interposer.dll"));
}

// Cyberpunk 20260816_153027: NVIDIA's NGX cache loaded sl.common 2.11 before CE's loader
// redirect was armed, so Streamline had already resolved its core. Every LATER plugin load did
// reach the redirect and became the 2.12 override copy — sl.interposer 2.7.1 + sl.common 2.11 +
// sl.reflex 2.12 in one runtime, and sl.reflex called through a null interface pointer it asked
// that sl.common for. A partially applied Streamline override is worse than none.
TEST(GraphicsRuntimeModulePolicy, StreamlineOverrideIsAllOrNothingAnchoredOnTheCore) {
    EXPECT_TRUE(ShouldApplyStreamlineOverrideRedirect(true, false));
    EXPECT_FALSE(ShouldApplyStreamlineOverrideRedirect(true, true));
    EXPECT_FALSE(ShouldApplyStreamlineOverrideRedirect(false, false));
    EXPECT_FALSE(ShouldApplyStreamlineOverrideRedirect(false, true));
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
    const size_t modelGuard =
        redirect.rfind("RedirectWouldDuplicateLoadedModule(modelFinal, &loadedPath)", modelReturn);
    EXPECT_NE(modelGuard, std::string::npos);

    const size_t genericReturn = redirect.find("return finalPath;");
    ASSERT_NE(genericReturn, std::string::npos);
    const size_t genericGuard =
        redirect.rfind("RedirectWouldDuplicateLoadedModule(finalPath, &loadedPath)", genericReturn);
    EXPECT_NE(genericGuard, std::string::npos);

    // Empty means "use the caller's original path", which is precisely wrong when the
    // original request is already the configured absolute path. A duplicate refusal must
    // return the resident physical path so every loader front-end reuses that image.
    EXPECT_NE(redirect.find("*loadedPathForReuse = loadedPath[0] ? loadedPath : baseName"),
              std::string::npos);
    EXPECT_NE(redirect.find("return loadedPath;", modelGuard), std::string::npos);
    EXPECT_NE(redirect.find("return loadedPath;", genericGuard), std::string::npos);
}

// The all-or-nothing rule has to be enforced at every place CE can place an override plugin:
// both redirect decisions AND the up-front preload, which would otherwise register override
// copies under names a foreign-core runtime then resolves by name.
TEST(GraphicsRuntimeModulePolicy, StreamlineOverridePlacementIsGatedOnOwningTheCore) {
    namespace fs = std::filesystem;
    const std::string redirect = ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "main_redirect.cpp");
    const std::string detect =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "main_overlay_detect.cpp");
    ASSERT_FALSE(redirect.empty());
    ASSERT_FALSE(detect.empty());

    // The core observation is fed from the loader notification, which resolves every load's
    // full path — the only place the driver's hashed NGX copy is identifiable.
    EXPECT_NE(detect.find("NoteRuntimeModuleLoadedForOverridePolicy(narrowPath)"), std::string::npos);
    EXPECT_NE(redirect.find("ResolveStreamlineProvidedDllName("), std::string::npos);
    EXPECT_NE(redirect.find("IsStreamlineCoreProvidedDllName("), std::string::npos);
    EXPECT_NE(redirect.find("ShouldApplyStreamlineOverrideRedirect("), std::string::npos);

    // It is a latch: a core Streamline probes and unloads has still been resolved.
    EXPECT_NE(redirect.find("g_ForeignStreamlineCoreObserved.exchange(true"), std::string::npos);

    // Both redirect decisions are gated.
    const size_t modelReturn = redirect.find("return modelFinal;");
    ASSERT_NE(modelReturn, std::string::npos);
    EXPECT_NE(redirect.rfind("StreamlineOverrideRedirectAllowed(modelDllName)", modelReturn), std::string::npos);
    const size_t genericReturn = redirect.find("return finalPath;");
    ASSERT_NE(genericReturn, std::string::npos);
    EXPECT_NE(redirect.rfind("StreamlineOverrideRedirectAllowed(filename.c_str())", genericReturn),
              std::string::npos);

    // The preload answers the question for modules that predate CE, then gates the sl.* set.
    const size_t preload = redirect.find("void PreloadConfiguredGraphicsRuntimeDlls()");
    ASSERT_NE(preload, std::string::npos);
    const size_t scan = redirect.find("ScanLoadedModulesForForeignStreamlineCore();", preload);
    ASSERT_NE(scan, std::string::npos);
    const size_t gate = redirect.find("StreamlineOverrideRedirectAllowed(\"sl.* plugin set\")", preload);
    ASSERT_NE(gate, std::string::npos);
    EXPECT_LT(scan, gate);
    const size_t interposerPreload = redirect.find("PreloadOverrideDll(gfx.streamlineDllPath, \"sl.interposer.dll\")",
                                                   preload);
    ASSERT_NE(interposerPreload, std::string::npos);
    EXPECT_LT(gate, interposerPreload);
    // The independent NGX snippet overrides must stay outside the gate.
    const size_t snippetPreload = redirect.find("PreloadOverrideDll(gfx.dlssSrDllPath, \"nvngx_dlss.dll\")", preload);
    ASSERT_NE(snippetPreload, std::string::npos);
    EXPECT_LT(interposerPreload, snippetPreload);
}

}  // namespace
