#include <gtest/gtest.h>

#include <filesystem>

#include "hook/runtime/dll_utils.h"
#include "hook/runtime/graphics_runtime_module_policy.h"
#include "source_fragment_reader.h"

namespace {

using ce::graphics_runtime::CanTakeOverForeignStreamlineInterposer;
using ce::graphics_runtime::EqualsModulePathIgnoreCase;
using ce::graphics_runtime::IsRuntimeModuleBaseName;
using ce::graphics_runtime::IsBridgeNgxFeatureModuleName;
using ce::graphics_runtime::IsNgxModelRepositoryPath;
using ce::graphics_runtime::IsStreamlineCoreProvidedDllName;
using ce::graphics_runtime::IsStreamlineInterposerModuleQuery;
using ce::graphics_runtime::ModelSegmentToDllName;
using ce::graphics_runtime::NgxModelSegment;
using ce::graphics_runtime::ResolveStreamlineProvidedDllName;
using ce::graphics_runtime::IsForeignStreamlineInterposer;
using ce::graphics_runtime::ShouldApplyStreamlineOverrideRedirect;
using ce::graphics_runtime::ShouldPlaceStreamlinePluginSet;
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

// Regression: a profile that only configures the override PATHS used to make CE
// map sl.interposer.dll into every injected process. NVIDIA's Windows Vulkan ICD
// answers a mapped Streamline interposer by abandoning its native present path
// for a layered DXGI presenter, so DOOM Eternal - which never loads Streamline -
// lost native WSI present purely because streamline_dll_path was set
// (2026-09-13, reproduced with build/vk-wsi-probe down to sl.interposer.dll
// alone). The copies may only be placed once the process shows Streamline use.
TEST(GraphicsRuntimeModulePolicy, StreamlinePluginSetIsPlacedOnlyForAProcessThatUsesStreamline) {
    ce::graphics_runtime::StreamlineUseEvidence none = {};
    EXPECT_FALSE(ShouldPlaceStreamlinePluginSet(true, false, none));

    ce::graphics_runtime::StreamlineUseEvidence mapped = {};
    mapped.coreAlreadyMapped = true;
    EXPECT_TRUE(ShouldPlaceStreamlinePluginSet(true, false, mapped));

    ce::graphics_runtime::StreamlineUseEvidence shipped = {};
    shipped.coreShippedWithApplication = true;
    EXPECT_TRUE(ShouldPlaceStreamlinePluginSet(true, false, shipped));

    ce::graphics_runtime::StreamlineUseEvidence observed = {};
    observed.loadObserved = true;
    EXPECT_TRUE(ShouldPlaceStreamlinePluginSet(true, false, observed));

    // The existing gates still win: no configured override, and a foreign core
    // that would turn the placement into a version-mixed stack.
    EXPECT_FALSE(ShouldPlaceStreamlinePluginSet(false, false, observed));
    EXPECT_FALSE(ShouldPlaceStreamlinePluginSet(true, true, observed));
}

// 20261009_110607: CE preloaded override sl.common/sl.dlss/... beside the game's resident interposer; the
// interposer then mapped its own core from the game folder and the preloaded copies were orphans (they
// held CE's single slGetPluginFunction forward pointer). A resident interposer that is not the override
// copy owns the stack, so nothing is placed by name - whatever else says Streamline is in use.
TEST(GraphicsRuntimeModulePolicy, ResidentGameInterposerOwnsTheStackAndSuppressesPlacement) {
    const char* const overrideInterposer = "C:\\npi\\sl\\sl.interposer.dll";
    const char* const gameInterposer = "H:\\game\\bin\\sl.interposer.dll";

    EXPECT_FALSE(IsForeignStreamlineInterposer(nullptr, overrideInterposer));  // none mapped
    EXPECT_TRUE(IsForeignStreamlineInterposer(gameInterposer, overrideInterposer));
    // CE's own copy, however the loader spells the path, is not foreign.
    EXPECT_FALSE(IsForeignStreamlineInterposer(overrideInterposer, overrideInterposer));
    EXPECT_FALSE(IsForeignStreamlineInterposer("c:/NPI/sl/SL.INTERPOSER.DLL", overrideInterposer));
    // Mapped but unreadable, or no override path to compare with: fail closed, never place beside it.
    EXPECT_TRUE(IsForeignStreamlineInterposer("", overrideInterposer));
    EXPECT_TRUE(IsForeignStreamlineInterposer(gameInterposer, nullptr));

    for (const bool mapped : {false, true}) {
        for (const bool shipped : {false, true}) {
            for (const bool observed : {false, true}) {
                ce::graphics_runtime::StreamlineUseEvidence evidence = {};
                evidence.coreAlreadyMapped = mapped;
                evidence.coreShippedWithApplication = shipped;
                evidence.loadObserved = observed;
                evidence.foreignInterposerResident = true;
                EXPECT_FALSE(ShouldPlaceStreamlinePluginSet(true, false, evidence))
                    << mapped << shipped << observed;
            }
        }
    }

    // CE's own interposer (placed earlier, or served by the redirect) leaves the existing rules untouched.
    ce::graphics_runtime::StreamlineUseEvidence ours = {};
    ours.coreAlreadyMapped = true;
    ours.foreignInterposerResident = false;
    EXPECT_TRUE(ShouldPlaceStreamlinePluginSet(true, false, ours));
}

// The probe has to run before the first placement and end the evaluation for good; the policy is only as
// good as the call site that feeds it.
TEST(GraphicsRuntimeModulePolicy, PlacementConsultsTheResidentInterposerBeforeMappingAnything) {
    namespace fs = std::filesystem;
    const std::string redirect = ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "runtime" / "main_redirect.cpp");
    ASSERT_FALSE(redirect.empty());

    const size_t placement = redirect.find("bool PlaceStreamlinePluginSet()");
    ASSERT_NE(placement, std::string::npos);
    const size_t probe = redirect.find("GetModuleHandleA(\"sl.interposer.dll\")", placement);
    const size_t verdict = redirect.find("IsForeignStreamlineInterposer(", placement);
    const size_t finalReturn = redirect.find("evidence.foreignInterposerResident) {", placement);
    const size_t interposerPreload =
        redirect.find("PreloadOverrideDll(gfx.streamlineDllPath, \"sl.interposer.dll\")", placement);
    ASSERT_NE(probe, std::string::npos);
    ASSERT_NE(verdict, std::string::npos);
    ASSERT_NE(finalReturn, std::string::npos);
    ASSERT_NE(interposerPreload, std::string::npos);
    EXPECT_LT(probe, verdict);
    EXPECT_LT(verdict, finalReturn);
    EXPECT_LT(finalReturn, interposerPreload);
    // Final, not "held back": the caller must stop re-evaluating, and the reason is logged once.
    const size_t returnTrue = redirect.find("return true;", finalReturn);
    ASSERT_NE(returnTrue, std::string::npos);
    EXPECT_LT(returnTrue, interposerPreload);
    EXPECT_NE(redirect.find("Runtime preload: sl.* plugin set not placed - sl.interposer.dll is already resident", placement),
              std::string::npos);
}

// Both redirect decisions in GetRedirectedPath must consult the duplicate check: the NGX
// model-repository branch returns early with its own path, so guarding only the generic branch
// would leave the driver-managed plugin loads (which is how sl.common/sl.reflex arrive) unguarded.
TEST(GraphicsRuntimeModulePolicy, LoaderRedirectGuardsBothDecisionsAgainstDuplicateInstances) {
    namespace fs = std::filesystem;
    const std::string redirect = ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "runtime" / "main_redirect.cpp");
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
    const std::string redirect = ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "runtime" / "main_redirect.cpp");
    const std::string detect =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "runtime" / "main_overlay_detect.cpp");
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

    // The sl.* placement is gated twice, in this order: on owning the core, and
    // on the process actually using Streamline. Both gates precede the first
    // PreloadOverrideDll of an sl.* name.
    const size_t placement = redirect.find("bool PlaceStreamlinePluginSet()");
    ASSERT_NE(placement, std::string::npos);
    const size_t coreGate = redirect.find("StreamlineOverrideRedirectAllowed(\"sl.* plugin set\")", placement);
    ASSERT_NE(coreGate, std::string::npos);
    const size_t useGate = redirect.find("ShouldPlaceStreamlinePluginSet(", coreGate);
    ASSERT_NE(useGate, std::string::npos);
    const size_t interposerPreload =
        redirect.find("PreloadOverrideDll(gfx.streamlineDllPath, \"sl.interposer.dll\")", useGate);
    ASSERT_NE(interposerPreload, std::string::npos);

    // The preload answers "did CE lose the core" for modules that predate CE
    // before it places anything.
    const size_t preload = redirect.find("void PreloadConfiguredGraphicsRuntimeDlls()");
    ASSERT_NE(preload, std::string::npos);
    const size_t scan = redirect.find("ScanLoadedModulesForForeignStreamlineCore();", preload);
    ASSERT_NE(scan, std::string::npos);
    const size_t placementCall = redirect.find("PlaceStreamlinePluginSet()", scan);
    ASSERT_NE(placementCall, std::string::npos);
    // The independent NGX snippet overrides must stay outside both gates.
    const size_t snippetPreload = redirect.find("PreloadOverrideDll(gfx.dlssSrDllPath, \"nvngx_dlss.dll\")", preload);
    ASSERT_NE(snippetPreload, std::string::npos);
    EXPECT_LT(placementCall, snippetPreload);
}

TEST(GraphicsRuntimeModulePolicy, RecognizesStreamlineInterposerModuleQueries) {
    EXPECT_TRUE(IsStreamlineInterposerModuleQuery("sl.interposer.dll"));
    EXPECT_TRUE(IsStreamlineInterposerModuleQuery("SL.INTERPOSER.DLL"));
    EXPECT_TRUE(IsStreamlineInterposerModuleQuery("sl.interposer"));
    EXPECT_TRUE(IsStreamlineInterposerModuleQuery("H:\\Games\\Witcher 3\\bin\\x64_dx12\\sl.interposer.dll"));
    EXPECT_TRUE(IsStreamlineInterposerModuleQuery("C:/Users/User/npi/sl/SL.Interposer"));

    EXPECT_TRUE(IsStreamlineInterposerModuleQuery(L"sl.interposer.dll"));
    EXPECT_TRUE(IsStreamlineInterposerModuleQuery(L"SL.INTERPOSER"));
    EXPECT_TRUE(IsStreamlineInterposerModuleQuery(L"H:\\Games\\Witcher 3\\bin\\x64_dx12\\sl.interposer.dll"));

    EXPECT_FALSE(IsStreamlineInterposerModuleQuery(static_cast<const char*>(nullptr)));
    EXPECT_FALSE(IsStreamlineInterposerModuleQuery(""));
    EXPECT_FALSE(IsStreamlineInterposerModuleQuery("sl.common.dll"));
    EXPECT_FALSE(IsStreamlineInterposerModuleQuery("sl.dlss_g.dll"));
    EXPECT_FALSE(IsStreamlineInterposerModuleQuery("sl.interposer.dll.bak"));
    EXPECT_FALSE(IsStreamlineInterposerModuleQuery(static_cast<const wchar_t*>(nullptr)));
    EXPECT_FALSE(IsStreamlineInterposerModuleQuery(L""));
    EXPECT_FALSE(IsStreamlineInterposerModuleQuery(L"sl.common.dll"));
}

// 20261010_225115 (The Witcher 3 DX12): witcher3.exe statically imports sl.interposer.dll, so the
// OS loader maps the game-directory copy during kernel-mode process creation before any user-mode
// instruction runs, and the main thread enters slInit 36 ms after DllMain returns. In-process
// takeover may replace that untouched static-import interposer only when the configured override
// exists, no Streamline core/plugin has loaded yet, and the major API generation matches.
TEST(GraphicsRuntimeModulePolicy, AllowsForeignInterposerTakeoverOnlyBeforeCoreLoadsAndWithinGeneration) {
    EXPECT_TRUE(CanTakeOverForeignStreamlineInterposer(true, true, false, true, 2, 2));
    EXPECT_TRUE(CanTakeOverForeignStreamlineInterposer(true, true, false, true, 1, 1));

    // Any missing precondition, unknown version, or cross-generation substitution is refused.
    EXPECT_FALSE(CanTakeOverForeignStreamlineInterposer(true, true, false, true, 0, 2));
    EXPECT_FALSE(CanTakeOverForeignStreamlineInterposer(true, true, false, true, 2, 0));
    EXPECT_FALSE(CanTakeOverForeignStreamlineInterposer(false, true, false, true, 2, 2));
    EXPECT_FALSE(CanTakeOverForeignStreamlineInterposer(true, false, false, true, 2, 2));
    EXPECT_FALSE(CanTakeOverForeignStreamlineInterposer(true, true, true, true, 2, 2));
    EXPECT_FALSE(CanTakeOverForeignStreamlineInterposer(true, true, false, false, 2, 2));
    EXPECT_FALSE(CanTakeOverForeignStreamlineInterposer(true, true, false, true, 1, 2));
    EXPECT_FALSE(CanTakeOverForeignStreamlineInterposer(true, true, false, true, 2, 1));
}

TEST(GraphicsRuntimeModulePolicy, TwoPhaseInterposerTakeoverWiresDllMainAndHookThreadBoundaries) {
    namespace fs = std::filesystem;
    const std::string dllmain =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "runtime" / "main_dllmain.cpp");
    const std::string hookthread =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "runtime" / "main_hookthread.cpp");
    const std::string redirect =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "runtime" / "main_redirect.cpp");
    const std::string slInstall =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "streamline" / "streamline_hook_install.cpp");
    const std::string drsOverride =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "ngx" / "ngx_drs_override.cpp");
    ASSERT_FALSE(dllmain.empty());
    ASSERT_FALSE(hookthread.empty());
    ASSERT_FALSE(redirect.empty());
    ASSERT_FALSE(slInstall.empty());
    ASSERT_FALSE(drsOverride.empty());

    // Phase 1 (DllMain): must arm after ResolveEarlyRuntimeOverridePaths and BEFORE
    // InstallSlInitRouteIfConfigured so Hooked_slInit chains on top of the takeover thunk
    // and the game's main thread cannot enter the game-directory slInit after DllMain returns.
    const size_t resolvePaths = dllmain.find("ce::published_config::ResolveEarlyRuntimeOverridePaths()");
    const size_t armTakeover = dllmain.find("ce::streamline_interposer_takeover::ArmEarlyFromDllMain()");
    const size_t slInitRoute = dllmain.find("ce::streamline_ota::InstallSlInitRouteIfConfigured()");
    ASSERT_NE(resolvePaths, std::string::npos);
    ASSERT_NE(armTakeover, std::string::npos);
    ASSERT_NE(slInitRoute, std::string::npos);
    EXPECT_LT(resolvePaths, armTakeover);
    EXPECT_LT(armTakeover, slInitRoute);

    // Phase 2 (HookThread): must eagerly load and bind the custom interposer right after
    // InstallLowLevelLoaderHook and before InstallGlobalVTableHooks.
    const size_t ldrHook = hookthread.find("InstallLowLevelLoaderHook(\"early hook thread\")");
    const size_t ensureReady = hookthread.find("ce::streamline_interposer_takeover::EnsureCustomInterposerReady()");
    const size_t vtableHooks = hookthread.find("InstallGlobalVTableHooks()");
    ASSERT_NE(ldrHook, std::string::npos);
    ASSERT_NE(ensureReady, std::string::npos);
    ASSERT_NE(vtableHooks, std::string::npos);
    EXPECT_LT(ldrHook, ensureReady);
    EXPECT_LT(ensureReady, vtableHooks);

    // Redirect and late-load hooks must exempt the takeover load, resolve the active
    // interposer, and patch GetModuleHandle* on late-loaded Streamline modules.
    EXPECT_NE(redirect.find("ShouldAllowTakeoverInterposerLoad("), std::string::npos);
    EXPECT_NE(redirect.find("ResolveModuleByNameForRedirect("), std::string::npos);
    EXPECT_NE(redirect.find("ActiveInterposerOr(GetModuleHandleA(\"sl.interposer.dll\"))"), std::string::npos);
    EXPECT_NE(redirect.find("PatchModuleHandleIatForModule(module)"), std::string::npos);
    EXPECT_NE(redirect.find("PatchInterposerIatForLateLoadedModule(module, moduleNameOrPath)"), std::string::npos);

    // Both Streamline export hooking and NGX DRS sweeps must ignore the superseded foreign
    // sl.interposer.dll so CE's single forward pointers bind to the active custom runtime.
    EXPECT_NE(slInstall.find("IsSupersededInterposerModule(module)"), std::string::npos);
    EXPECT_NE(drsOverride.find("IsSupersededInterposerModule(retained)"), std::string::npos);

    // InstallKernel32LoaderHooks("DllMain") must arm takeover before the 10-pass
    // PatchIATAllModules kernel32 scan so static sl.interposer.dll!slInit IAT slots are
    // gated in single-digit milliseconds without waiting for all kernel32 IAT passes.
    const std::string injection =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "runtime" / "main_injection.cpp");
    ASSERT_FALSE(injection.empty());
    const size_t injResolve = injection.find("ce::published_config::ResolveEarlyRuntimeOverridePaths()");
    const size_t injArm = injection.find("ce::streamline_interposer_takeover::ArmEarlyFromDllMain()");
    const size_t injInitK32 = injection.find("IATHook::InitializeKernel32Hooks(");
    ASSERT_NE(injResolve, std::string::npos);
    ASSERT_NE(injArm, std::string::npos);
    ASSERT_NE(injInitK32, std::string::npos);
    EXPECT_LT(injResolve, injArm);
    EXPECT_LT(injArm, injInitK32);
}

TEST(GraphicsRuntimeModulePolicy, DirectPeVersionResourceReaderMatchesLoadedModuleVersionWithoutLoaderCalls) {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    ASSERT_NE(kernel32, nullptr);
    wchar_t kernel32PathW[MAX_PATH] = {};
    char kernel32PathA[MAX_PATH] = {};
    ASSERT_GT(GetModuleFileNameW(kernel32, kernel32PathW, MAX_PATH), 0u);
    ASSERT_GT(GetModuleFileNameA(kernel32, kernel32PathA, MAX_PATH), 0u);

    uint32_t loadedMajor = 0, loadedMinor = 0, loadedBuild = 0;
    ASSERT_TRUE(ModuleFileVersionParts(kernel32, &loadedMajor, &loadedMinor, &loadedBuild));

    const std::string peResource = DllReadPeVersionResourceW(kernel32PathW);
    ASSERT_FALSE(peResource.empty());
    uint32_t peMajor = 0, peMinor = 0, peBuild = 0;
    ASSERT_TRUE(VersionResourceFileVersionParts(peResource, &peMajor, &peMinor, &peBuild));

    uint32_t wrapperMajor = 0, wrapperMinor = 0, wrapperBuild = 0;
    ASSERT_TRUE(DllFileVersionPartsW(kernel32PathW, &wrapperMajor, &wrapperMinor, &wrapperBuild));

    EXPECT_GT(loadedMajor, 0u);
    EXPECT_EQ(peMajor, loadedMajor);
    EXPECT_EQ(peMinor, loadedMinor);
    EXPECT_EQ(peBuild, loadedBuild);
    EXPECT_EQ(wrapperMajor, loadedMajor);
    EXPECT_EQ(wrapperMinor, loadedMinor);
    EXPECT_EQ(wrapperBuild, loadedBuild);
    EXPECT_EQ(DllFileMajorVersion(kernel32PathA), loadedMajor);

    const std::string missing = DllReadPeVersionResourceW(L"Z:\\__nonexistent_ce_test_path__\\missing.dll");
    EXPECT_TRUE(missing.empty());
}

}  // namespace
