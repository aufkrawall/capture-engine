#pragma once

#include <cstddef>
#include <cstdint>

#include "../common/streamline_api_generation.h"

// Policy for the Streamline generation bridge: running a 2.x runtime inside a game that
// shipped 1.x, so DLSS-G / multi-frame generation becomes reachable in titles that would
// otherwise be stuck on Streamline 1.x.
//
// This is NOT the `streamline_dll_path` substitution. That one rewrites the paths of loads
// the game and Streamline perform, and `StreamlineOverrideGenerationMatches` correctly
// refuses to let it cross generations - a 1.x game imports five exports a 2.x interposer
// does not have, so the loader would kill the process before its first frame. The bridge
// instead leaves the game's own 1.x runtime entirely alone and adds a SECOND, CE-owned 2.x
// runtime loaded by full path, then repoints the game's import slots at CE thunks that
// translate. Nothing about the game's loads is redirected, which is why the duplicate-
// instance guard in graphics_runtime_module_policy.h needs no exemption: the bridge never
// asks for a redirect. What it does need is for the ordinary sl.* redirect to stand down
// while it is active, so CE cannot simultaneously rewrite the game's 1.x plugin loads into
// the 2.x folder and build the version-mixed stack those guards exist to prevent.
//
// Feasibility was measured before any of this was written (2026-08-21): with The Witcher
// 3's sl.interposer 1.5.6 and its plugins resident, a 2.11.1 interposer loaded by full path
// initialises with `Preferences::pathsToPlugins`, maps its own sl.common alongside the 1.x
// one, and binds every plugin from its own folder even though `GetModuleHandleW` still
// answers the 1.x image for those base names. See llm-wiki/frame-generation/guardrails.md.
namespace ce::streamline_bridge {

using ce::streamline_api::Generation;

// ---------------------------------------------------------------------------
// Feature enum translation
// ---------------------------------------------------------------------------
//
// The generations do NOT agree on feature values, and two of the disagreements are silent
// rather than loud. Values below are 1.x from sl.interposer 1.5.6's own feature-name table
// and 2.x from the vendored 2.11.1 headers (build/fg_sdk_include/streamline/include).
inline constexpr uint32_t kV1FeatureDLSS = 0;
inline constexpr uint32_t kV1FeatureNRD = 1;
inline constexpr uint32_t kV1FeatureNIS = 2;
inline constexpr uint32_t kV1FeatureReflex = 3;
inline constexpr uint32_t kV1FeatureDebug = 4;
inline constexpr uint32_t kV1FeatureDLSS_G = 5;
inline constexpr uint32_t kV1FeatureCommon = UINT32_MAX;

inline constexpr uint32_t kV2FeatureDLSS = 0;
inline constexpr uint32_t kV2FeatureNIS = 2;
inline constexpr uint32_t kV2FeatureReflex = 3;
inline constexpr uint32_t kV2FeaturePCL = 4;
inline constexpr uint32_t kV2FeatureDeepDVC = 5;
inline constexpr uint32_t kV2FeatureDLSS_G = 1000;
inline constexpr uint32_t kV2FeatureCommon = UINT32_MAX;

// Translates a 1.x feature id to its 2.x equivalent. False means "no faithful mapping
// exists" and the call must be refused rather than guessed at.
//
// The two traps this exists for, both of which would corrupt silently if a caller passed
// the value through unchanged:
//   * 1.x eFeatureDLSS_G is 5, but 2.x reserves 5 for kFeatureDeepDVC and puts DLSS-G at
//     1000. An untranslated 5 drives an unrelated feature rather than frame generation.
//   * 1.x eFeatureDebug is 4, which in 2.x is kFeaturePCL. Same collision, opposite
//     direction, and 1.x's debug feature has no 2.x counterpart at all.
// NRD (1.x value 1) was removed from Streamline; 2.x spells the slot kFeatureNRD_INVALID.
inline bool TranslateV1FeatureToV2(uint32_t v1Feature, uint32_t* outV2Feature) {
    uint32_t mapped = 0;
    switch (v1Feature) {
        case kV1FeatureDLSS:
            mapped = kV2FeatureDLSS;
            break;
        case kV1FeatureNIS:
            mapped = kV2FeatureNIS;
            break;
        case kV1FeatureReflex:
            mapped = kV2FeatureReflex;
            break;
        case kV1FeatureDLSS_G:
            mapped = kV2FeatureDLSS_G;
            break;
        case kV1FeatureCommon:
            mapped = kV2FeatureCommon;
            break;
        case kV1FeatureNRD:   // removed in 2.x
        case kV1FeatureDebug: // collides with kFeaturePCL, no 2.x equivalent
        default:
            return false;
    }
    if (outV2Feature) {
        *outV2Feature = mapped;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Buffer type translation
// ---------------------------------------------------------------------------
//
// Unlike features, buffer types ARE identity. sl.common 1.5.6's own name table holds
// exactly 38 entries, indices 0..37 (Depth .. TransparencyAndCompositionMaskHint), and each
// one lands on the same value in the 2.11.1 headers - 2.x only appends beyond 37. So the
// bridge needs a range check, not a mapping table. `MVec`/`SpecularMVec` were renamed to
// `MotionVectors`/`SpecularMotionVectors` in 2.x but kept their slots (1 and 10).
inline constexpr uint32_t kV1BufferTypeMaxKnown = 37;

// A 1.x buffer type CE has actually verified against the 2.x table. Anything above the
// known range is refused rather than forwarded: 1.5.6 cannot produce it, so seeing one
// means the tag did not come from the interposer CE classified.
inline bool TranslateV1BufferTypeToV2(uint32_t v1BufferType, uint32_t* outV2BufferType) {
    if (v1BufferType > kV1BufferTypeMaxKnown) {
        return false;
    }
    if (outV2BufferType) {
        *outV2BufferType = v1BufferType;  // identity across the whole 1.x table
    }
    return true;
}

// ---------------------------------------------------------------------------
// The import surface a bridged process has to serve
// ---------------------------------------------------------------------------
//
// The Witcher 3 imports 15 symbols from sl.interposer.dll and no others, which is far less
// than the full 1.x API. Eight are Streamline calls that need real translation; seven are
// the DXGI/D3D12 entry points the interposer re-exports so it can interpose device and
// factory creation, and those have identical signatures in 2.x because they are Microsoft's
// and not NVIDIA's - they forward to the 2.x interposer unchanged.
//
// The bridge is keyed on the imports a title actually uses, never on its executable name.
// A 1.x game that imports something outside these two sets is not bridgeable by this code
// and must be refused rather than half-served.
inline constexpr const char* kTranslatedV1Exports[] = {
    "slInit",       "slShutdown",           "slIsFeatureSupported", "slSetTag",
    "slSetConstants", "slSetFeatureConstants", "slGetFeatureSettings", "slEvaluateFeature",
};
inline constexpr size_t kTranslatedV1ExportCount =
    sizeof(kTranslatedV1Exports) / sizeof(kTranslatedV1Exports[0]);

inline constexpr const char* kPassThroughExports[] = {
    "CreateDXGIFactory",  "CreateDXGIFactory1",     "CreateDXGIFactory2",
    "DXGIGetDebugInterface1", "D3D12CreateDevice",  "D3D12GetDebugInterface",
    "D3D12SerializeVersionedRootSignature",
};
inline constexpr size_t kPassThroughExportCount =
    sizeof(kPassThroughExports) / sizeof(kPassThroughExports[0]);

inline bool NamesEqual(const char* left, const char* right) {
    if (!left || !right) {
        return false;
    }
    while (*left != '\0' && *right != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

inline bool IsTranslatedV1Export(const char* name) {
    for (const char* candidate : kTranslatedV1Exports) {
        if (NamesEqual(name, candidate)) {
            return true;
        }
    }
    return false;
}

inline bool IsPassThroughExport(const char* name) {
    for (const char* candidate : kPassThroughExports) {
        if (NamesEqual(name, candidate)) {
            return true;
        }
    }
    return false;
}

// Every slot the bridge takes over. A symbol in neither set is one CE has no plan for.
inline bool IsBridgedExport(const char* name) {
    return IsTranslatedV1Export(name) || IsPassThroughExport(name);
}

// ---------------------------------------------------------------------------
// Preferences the bridge initialises its 2.x runtime with
// ---------------------------------------------------------------------------
//
// `sl::PreferenceFlags` bits, spelled locally so this header stays includable by the unit
// tests without the Streamline SDK on the include path.
inline constexpr uint64_t kPrefDisableCLStateTracking = 1ull << 0;
inline constexpr uint64_t kPrefAllowOTA = 1ull << 3;
inline constexpr uint64_t kPrefUseDXGIFactoryProxy = 1ull << 5;
inline constexpr uint64_t kPrefLoadDownloadedPlugins = 1ull << 6;

// The SDK's own default for `Preferences::flags`.
inline constexpr uint64_t kPrefSdkDefault =
    kPrefDisableCLStateTracking | kPrefAllowOTA | kPrefLoadDownloadedPlugins;

// Two deliberate departures from the SDK default, both measured rather than assumed:
//
//   * OTA off. With the default flags the 2.x interposer also discovers plugins under
//     C:\ProgramData\NVIDIA\NGX\models\sl_*_0\versions\... and arbitrates them against the
//     staged set by version ("Removing plugin with name: sl.common superseded by plugin
//     sl.common"). The staged 2.11.1 set won when this was measured, but a bridge that
//     claims to run a pinned, tested runtime cannot let a background driver update pick
//     which plugins it actually executes.
//   * DXGI factory proxy on. Streamline documents this flag as being for third-party
//     overlays that operate by injection rather than integration, which is exactly what CE
//     is: it makes Streamline hand out a factory proxy instead of mutating the base
//     interface's v-table underneath CE's own DXGI hooks.
inline constexpr uint64_t BridgePreferenceFlags() {
    return (kPrefSdkDefault & ~(kPrefAllowOTA | kPrefLoadDownloadedPlugins)) | kPrefUseDXGIFactoryProxy;
}

// ---------------------------------------------------------------------------
// Activation
// ---------------------------------------------------------------------------

enum class ActivationDecision {
    Activate,
    DeclinedNotEnabled,
    DeclinedNoRuntimePath,
    DeclinedNotAnUpgrade,
    DeclinedTooLate,
};

struct ActivationInputs {
    // `streamline_upgrade=on`. Default off: the bridge is more invasive than the path
    // overrides and carries the same anti-cheat warning, so it is never implicit.
    bool upgradeEnabled = false;
    // `streamline_dll_path` names the 2.x runtime folder. The bridge has nothing to load
    // without it.
    bool runtimePathConfigured = false;
    // The generation the game itself is running, and the one in the configured folder.
    Generation processGeneration = Generation::Unknown;
    Generation runtimeGeneration = Generation::Unknown;
    // Whether the game has already driven Streamline. Both are late-signals: once the game
    // has initialised its own 1.x runtime or created its device/factory through the 1.x
    // interposer, taking the imports over would leave half the process talking to each
    // generation.
    bool gameAlreadyInitializedStreamline = false;
    bool gameAlreadyCreatedDeviceOrFactory = false;
};

// The bridge is all-or-nothing and the decision is made once, before anything is taken
// over. A half-bridged process - some calls translated, some reaching the 1.x runtime, a
// device created through one generation and driven through the other - is worse than either
// end state, so every "no" here must leave the process exactly as it was.
inline ActivationDecision DecideActivation(const ActivationInputs& inputs) {
    if (!inputs.upgradeEnabled) {
        return ActivationDecision::DeclinedNotEnabled;
    }
    if (!inputs.runtimePathConfigured) {
        return ActivationDecision::DeclinedNoRuntimePath;
    }
    // Strictly 1.x -> 2.x. Same-generation substitution is what `streamline_dll_path`
    // already does on its own and must keep doing; a 2.x game needs no bridge, and an
    // unclassified module on either side is never guessed at.
    if (inputs.processGeneration != Generation::V1 || inputs.runtimeGeneration != Generation::V2) {
        return ActivationDecision::DeclinedNotAnUpgrade;
    }
    if (inputs.gameAlreadyInitializedStreamline || inputs.gameAlreadyCreatedDeviceOrFactory) {
        return ActivationDecision::DeclinedTooLate;
    }
    return ActivationDecision::Activate;
}

inline bool ShouldActivate(const ActivationInputs& inputs) {
    return DecideActivation(inputs) == ActivationDecision::Activate;
}

inline const char* Describe(ActivationDecision decision) {
    switch (decision) {
        case ActivationDecision::Activate:
            return "activating the Streamline generation bridge";
        case ActivationDecision::DeclinedNotEnabled:
            return "streamline_upgrade is off";
        case ActivationDecision::DeclinedNoRuntimePath:
            return "streamline_upgrade is on but streamline_dll_path names no 2.x runtime";
        case ActivationDecision::DeclinedNotAnUpgrade:
            return "this is not a 1.x process with a 2.x replacement runtime";
        case ActivationDecision::DeclinedTooLate:
            return "the game already drove its own Streamline runtime";
    }
    return "unrecognized bridge activation decision";
}

// While the bridge owns the process's Streamline, the ordinary path-substitution redirect
// must stand down for the whole sl.* family.
//
// The two mechanisms want the same configured folder for opposite purposes. The bridge
// loads that folder's 2.x runtime as a second, private runtime; the redirect would rewrite
// the game's OWN 1.x plugin loads into it, which is the cross-generation substitution
// `StreamlineOverrideGenerationMatches` refuses on its own and, worse, would leave the 1.x
// interposer resolving 2.x plugins. Letting both act on one config value is the version-
// mixing failure the redirect guards were written for.
inline bool StreamlineRedirectSuppressedByBridge(bool bridgeActive) { return bridgeActive; }

}  // namespace ce::streamline_bridge
