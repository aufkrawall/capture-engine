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
// 1000, NOT 5 - and this one was measured after being inferred wrong.
//
// sl.interposer 1.5.6's feature-name table lists DLSS, NRD, NIS, Reflex, Debug, DLSS_G,
// Common in that order, and reading position as value put DLSS_G at 5. It does not: the
// table is in declaration order, and 1.x already assigns DLSS-G the same out-of-line 1000
// that 2.x uses. The Witcher 3 settles it - session `20260821_041255` records the game
// calling `slSetFeatureConstants` with feature 1000, immediately after the Reflex
// constants, which is exactly how a title brings DLSS-G up.
//
// Left as it was, this would have translated a value the game never sends while refusing
// the one it does, and frame generation would simply never have been configured.
inline constexpr uint32_t kV1FeatureDLSS_G = 1000;
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
// Measurement turned this table into an identity map for everything The Witcher 3 actually
// uses - DLSS 0, Reflex 3 and DLSS-G 1000 all keep their values - which is a much smaller
// translation than the generations' reputation suggests. What still has to be refused is
// the handful of values whose meaning genuinely differs:
//   * 1.x eFeatureDebug is 4, which in 2.x is kFeaturePCL. Forwarding it drives NVIDIA's
//     PCL marker feature from a debug request; 1.x's debug feature has no 2.x counterpart.
//   * 5 means nothing in 1.5.6 but is kFeatureDeepDVC in 2.x, so a stray 5 must never be
//     passed through as if it did.
//   * NRD (1) was removed from Streamline entirely; 2.x spells the slot kFeatureNRD_INVALID.
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
        case kV1FeatureNRD:    // removed in 2.x
        case kV1FeatureDebug:  // collides with kFeaturePCL, no 2.x equivalent
        default:               // includes 5, which is kFeatureDeepDVC in 2.x and unused in 1.5.6
            return false;
    }
    if (outV2Feature) {
        *outV2Feature = mapped;
    }
    return true;
}

// Names the 1.x feature a call carries, for diagnostics. Unknown values are reported as
// such rather than folded into a neighbour - the two collisions above are exactly why.
inline const char* DescribeV1Feature(uint32_t v1Feature) {
    switch (v1Feature) {
        case kV1FeatureDLSS:
            return "DLSS";
        case kV1FeatureNRD:
            return "NRD";
        case kV1FeatureNIS:
            return "NIS";
        case kV1FeatureReflex:
            return "Reflex";
        case kV1FeatureDebug:
            return "Debug";
        case kV1FeatureDLSS_G:
            return "DLSS-G";
        case kV1FeatureCommon:
            return "Common";
        default:
            break;
    }
    return "an unrecognized 1.x feature";
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
// Which translated calls may reach a 2.x runtime that has no device yet
// ---------------------------------------------------------------------------
//
// Streamline 2.x's exported entry points are NOT all callable before
// `slSetD3DDevice`. Most of them are thin forwarders into a plugin the manager binds only
// once the device is known, and before that the pointer they jump through is null - they
// do not return an error, they jump to address zero.
//
// This is measured, not defensive. Session `20260821_155250` crashed with
// `0xC0000005 at 0x0000000000000000`, RIP=0, stack:
//
//     capture_hook_x64!Bridged_slSetConstants
//     sl_interposer!slSetConstants+0x49
//     0x0
//
// The bridge had taken the imports over and initialised its 2.x runtime, the game called
// `slSetConstants`, and the translation forwarded it into a runtime with no device. The
// same run shows the other half of the same cause: the feature entry points never resolved,
// because `slGetFeatureFunction` needs the device too, so DLSS and DLSS-G were being
// refused for a reason that would never have stopped being true.
//
// So the bridge holds these calls back until the device is set, and refuses them meanwhile.
// A refused call costs the game one frame of configuration; a forwarded one kills it.
//
// The SDK header states the requirement per function - "requires DX/VK device to be created
// before calling it" on `slSetConstants`, `slSetTagForFrame` and `slEvaluateFeature`, and
// "Must be called AFTER device is set" on `slGetFeatureFunction`. `slIsFeatureSupported`
// takes an `AdapterInfo` and asks about hardware rather than a live device, which is why it
// is the one call that legitimately answers early - and it did, in that same session.
enum class V2Call {
    Init,
    Shutdown,
    IsFeatureSupported,
    SetTag,
    SetConstants,
    SetFeatureConstants,
    GetFeatureSettings,
    EvaluateFeature,
};

inline bool V2CallRequiresDevice(V2Call call) {
    switch (call) {
        case V2Call::Init:
        case V2Call::Shutdown:
        case V2Call::IsFeatureSupported:
            // Init and Shutdown never reach the 2.x runtime at all - CE owns its lifetime -
            // and IsFeatureSupported is adapter-scoped.
            return false;
        case V2Call::SetTag:
        case V2Call::SetConstants:
        case V2Call::EvaluateFeature:
            // These three say so themselves in sl_core_api.h: "requires DX/VK device to be
            // created before calling it".
        case V2Call::SetFeatureConstants:
        case V2Call::GetFeatureSettings:
            // These two are reached through slGetFeatureFunction, which the same header
            // says must be called after the device is set.
            return true;
    }
    return true;  // an unlisted call is held back, never let through
}

inline const char* DescribeV2Call(V2Call call) {
    switch (call) {
        case V2Call::Init:
            return "slInit";
        case V2Call::Shutdown:
            return "slShutdown";
        case V2Call::IsFeatureSupported:
            return "slIsFeatureSupported";
        case V2Call::SetTag:
            return "slSetTag";
        case V2Call::SetConstants:
            return "slSetConstants";
        case V2Call::SetFeatureConstants:
            return "slSetFeatureConstants";
        case V2Call::GetFeatureSettings:
            return "slGetFeatureSettings";
        case V2Call::EvaluateFeature:
            return "slEvaluateFeature";
    }
    return "an unrecognized call";
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

// `sl::kSDKVersion` for a given 2.x distribution.
//
// The host declares which SDK it was built against; Streamline uses that for compatibility
// decisions. CE is not built against any single one - it drives whichever runtime the
// profile staged - so the value has to be reconstructed from that runtime rather than
// frozen at one version. Hard-coding 2.11.1 would mis-declare CE to a 2.12.0 folder, which
// is exactly the "point it at the latest DLLs" case this feature exists for.
inline constexpr uint64_t kStreamlineSdkVersionMagic = 0xfedcull;

inline constexpr uint64_t StreamlineSdkVersion(uint32_t major, uint32_t minor, uint32_t patch) {
    return (static_cast<uint64_t>(major) << 48) | (static_cast<uint64_t>(minor) << 32) |
           (static_cast<uint64_t>(patch) << 16) | kStreamlineSdkVersionMagic;
}

// Nothing here pins a Streamline minor version, deliberately.
//
// The staged runtime is expected to change - that is the point of pointing CE at a folder -
// so every version-dependent value is read from the runtime that is actually present. The
// `Preferences` mirror stays valid across those updates without a version table because
// Streamline's structures are self-describing: each carries its own type GUID and a
// `structVersion`, and a host that declares version 1 is promising only that it filled the
// version-1 fields. Later SDKs append fields behind a higher version and keep reading a
// version-1 struct exactly as before, which is what that field is for. A layout change that
// this scheme could not absorb would be a new struct type, and Streamline rejects that
// itself - `slInit` returns a failing `sl::Result`, which CE logs together with the version
// it read. That is a real check against the runtime in hand, whereas a hard-coded range of
// "known good" versions would only ever go stale.
// ---------------------------------------------------------------------------
// Activation
// ---------------------------------------------------------------------------

enum class ActivationDecision {
    // Nothing of the game's own Streamline has run yet: take the imports over and its 1.x
    // runtime never comes up at all.
    Activate,
    // The game's 1.x core is already resident, so `slInit` has run. Take the imports over
    // anyway and shut that runtime back down - see DecideActivation for why that is a
    // complete end state rather than a half-switch.
    ActivateAndQuiesce,
    DeclinedNotEnabled,
    DeclinedNoRuntimePath,
    DeclinedNotAnUpgrade,
    DeclinedGameOwnsItsDevice,
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
    // `sl.common.dll` resident: 1.x loads its core from inside `slInit`, so this is proof
    // that the game already brought its own runtime up. Recoverable - see below.
    bool gameAlreadyInitializedStreamline = false;
    // The game already created its D3D12 device or DXGI factory through the 1.x interposer.
    // This one is not recoverable: a 2.x runtime can only drive a device it interposed.
    bool gameAlreadyCreatedDeviceOrFactory = false;
};

// The bridge is all-or-nothing and the decision is made once, before anything is taken
// over. A half-bridged process - some calls translated, some reaching the 1.x runtime, a
// device created through one generation and driven through the other - is worse than either
// end state, so every "no" here must leave the process exactly as it was.
//
// Where "too late" actually falls was measured, and it is later than this policy first
// claimed. The original version refused as soon as `sl.common.dll` was resident, which made
// the feature unreachable rather than careful: CE reaches a title through WMI process
// notification (`WITHIN 0.5`), a config reload and a remote-thread LoadLibrary, and in a
// 1.x DX12 title whose executable imports its D3D12/DXGI entry points FROM
// `sl.interposer.dll` - The Witcher 3 does - `d3d12.dll` is not even in the process until
// `sl.common.dll` drags it in through its own import table, from inside `slInit`. So the
// arrival CE can engineer and the deadline it was being held to are the same event. Both
// recorded sessions refused for this reason (`20260821_151738`, `20260821_151924`), and no
// amount of shaving milliseconds off CE's startup would reliably change that. A fix that
// consists of winning a race is not a fix.
//
// The genuinely irreversible step is not `slInit`, it is device creation. Until the game
// creates its device or factory through the 1.x interposer, everything 1.x has done is
// undoable: CE brings its own 2.x runtime up, repoints the imports, and then shuts the 1.x
// runtime back down through the very export slot it saved while repointing it. What is left
// is exactly one initialised runtime driving everything - the same end state an early
// takeover reaches, from a later start. The margin is not marginal: in session
// `20260821_151738` the 1.x core was resident by 15:18:12.3 while the game's real swapchain
// was not created until 15:18:29.1, and its 1.x feature plugins (`sl.dlss_g`, `sl.reflex`,
// `sl.dlss`) did not load until 15:18:13.4-13.9, all of it after the point CE refused at.
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
    if (inputs.gameAlreadyCreatedDeviceOrFactory) {
        return ActivationDecision::DeclinedGameOwnsItsDevice;
    }
    if (inputs.gameAlreadyInitializedStreamline) {
        return ActivationDecision::ActivateAndQuiesce;
    }
    return ActivationDecision::Activate;
}

inline bool ShouldActivate(const ActivationInputs& inputs) {
    const ActivationDecision decision = DecideActivation(inputs);
    return decision == ActivationDecision::Activate || decision == ActivationDecision::ActivateAndQuiesce;
}

// Whether the takeover has to shut the game's own 1.x runtime down afterwards. Only the
// late start does: on an early one there is nothing initialised to shut down, and calling
// `slShutdown` on a runtime that never ran is a state change made for no reason.
inline bool RequiresLegacyQuiesce(ActivationDecision decision) {
    return decision == ActivationDecision::ActivateAndQuiesce;
}

inline const char* Describe(ActivationDecision decision) {
    switch (decision) {
        case ActivationDecision::Activate:
            return "activating the Streamline generation bridge before the game's own runtime ran";
        case ActivationDecision::ActivateAndQuiesce:
            return "activating the Streamline generation bridge and shutting the game's own 1.x runtime back down";
        case ActivationDecision::DeclinedNotEnabled:
            return "streamline_upgrade is off";
        case ActivationDecision::DeclinedNoRuntimePath:
            return "streamline_upgrade is on but streamline_dll_path names no 2.x runtime";
        case ActivationDecision::DeclinedNotAnUpgrade:
            return "this is not a 1.x process with a 2.x replacement runtime";
        case ActivationDecision::DeclinedGameOwnsItsDevice:
            return "the game already created its device through its own 1.x interposer, which no takeover undoes";
    }
    return "unrecognized bridge activation decision";
}

// Which generation is authoritative for decisions that cannot be made per module.
//
// A process normally runs one Streamline distribution, so "the first module CE classified"
// answers for the whole process. The bridge breaks that on purpose: it leaves the game's
// 1.x interposer resident and adds a CE-owned 2.x runtime, and the 1.x one is loaded from
// process start so it is almost always classified first. Since every Streamline call the
// game makes now reaches CE's thunks and then the 2.x runtime, that runtime is the one a
// process-wide route has to match.
//
// This is only for genuinely process-wide decisions. Anything that installs an
// ABI-sensitive hook on a specific module must use THAT module's generation - with two
// generations resident, applying this answer to the 1.x module would install 2.x-shaped
// hooks on it, which is the argument truncation the whole generation gate exists to stop.
inline Generation AuthoritativeProcessGeneration(bool bridgeActive, Generation firstSeenGeneration) {
    return bridgeActive ? Generation::V2 : firstSeenGeneration;
}

// Whether CE should leave a Streamline module's own exports alone because the bridge has
// already routed every call away from it.
//
// With the bridge active the game's 1.x interposer is inert: all fifteen import slots that
// used to reach it now reach CE, and its plugins have been unloaded. Hooking it is not
// merely pointless, it is actively harmful, and session `20260821_155250` shows both ways:
//
//     Streamline Hook: sl.interposer.dll speaks Streamline 1.x - CE installs only the hooks
//     Streamline Hook: registered the Streamline 2.x slSetTag/slEvaluateFeature dynamic routes
//     Streamline Hook: Inline hook installed for slSetTag at 00007FFE25921D50   <- the 1.x module
//     ...
//     Streamline Hook: Refusing to retarget slSetTag from 00007FFE25921D50 to 00007FFE037273D0
//         - the installed target is still mapped
//
// A 2.x-shaped hook landed on the 1.x image (the argument truncation the generation gate
// exists to prevent), and then it held CE's single forward pointer, so the hook on the
// runtime that actually runs was refused. CE ends up watching a module nothing calls while
// the live one goes unobserved - which is `dlss_fg_factor`, `dlss_fg_preset` and the
// overlay's whole FG state machine, blind.
//
// The generation alone identifies it: the bridge activates only for a 1.x process paired
// with a 2.x runtime, so while it is active any V1 Streamline module in the process is by
// construction the superseded one.
inline bool StreamlineModuleSupersededByBridge(bool bridgeActive, Generation moduleGeneration) {
    return bridgeActive && moduleGeneration == Generation::V1;
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
