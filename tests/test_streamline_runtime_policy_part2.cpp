#include <gtest/gtest.h>

#include <filesystem>

#include "../common/config.h"
#include "../hook/common/dx12_overlay_policy.h"
#include "../hook/common/streamline_runtime_policy.h"
#include "source_fragment_reader.h"

namespace {

TEST(StreamlineRuntimePolicyTest, StreamlineModuleUnloadDispatchesHookInvalidation) {
    // Crash 20260612_003407: the game unloads/reloads the whole SL stack when
    // toggling DLSS FG; every sl.* unload must invalidate stale hook slots.
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload("sl.interposer.dll"));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload("sl.common.dll"));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload("sl.dlss_g.dll"));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload("SL.COMMON.DLL"));

    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload("slang.dll"));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload("common.dll"));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload("dxgi.dll"));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload(nullptr));
}

// Regression (session 20260811_230524): late injection into an already-running
// DLSS-FG game never hooks slDLSSGSetOptions because the game resolved the
// feature function before injection and never re-resolves it. The loaded-module
// scan must proactively resolve the DLSS-G/Reflex feature functions through
// the interposer after hooking the loaded modules, so the game's cached
// slDLSSGSetOptions pointer flows through CE (and the FG multiplier / Reflex
// state signals become observable). Without this, Talos's 4x MFG
// (slDLSSGSetOptions numFramesToGenerate=3) stays invisible and the overlay
// reports DLSS 2x.
TEST(StreamlineRuntimePolicyTest, LoadedModuleScanResolvesFeatureHooksAfterHookingModules) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "streamline_hook_install.cpp";
    const fs::path headerSource = fs::current_path() / "hook" / "apis" / "streamline_hook_internal.h";
    ASSERT_TRUE(fs::exists(source));
    ASSERT_TRUE(fs::exists(headerSource));

    const std::string text = ce::test_source::ReadLogicalSource(source);
    const std::string headerText = ce::test_source::ReadLogicalSource(headerSource);
    ASSERT_FALSE(text.empty());
    ASSERT_FALSE(headerText.empty());

    const size_t scanStart = text.find("bool ScanLoadedStreamlineModules(bool pinFeatureResolution)");
    ASSERT_NE(scanStart, std::string::npos);
    const size_t snapshotClose = text.find("CloseHandle(snapshot);", scanStart);
    ASSERT_NE(snapshotClose, std::string::npos);
    const size_t dlssgResolve = text.find("TryResolveDLSSGFeatureHooks(pinFeatureResolution)", snapshotClose);
    const size_t reflexResolve = text.find("TryResolveReflexFeatureHooks(pinFeatureResolution)", snapshotClose);
    ASSERT_NE(dlssgResolve, std::string::npos);
    ASSERT_NE(reflexResolve, std::string::npos);
    // Resolution must run after the module snapshot is released (runtime
    // stable, no loader lock) and inside the scan so late inject and the
    // runtime retry path both benefit.
    EXPECT_LT(snapshotClose, dlssgResolve);
    EXPECT_LT(snapshotClose, reflexResolve);
    EXPECT_NE(text.find("Resolved feature hooks after loaded-module scan", scanStart), std::string::npos);
    // Reflex SetConstants can be genuinely absent from a sl.reflex build; the
    // retry loop must bound the failed queries instead of re-scanning forever
    // (session 20260811_231851 logged endless 2.5s rescans).
    EXPECT_NE(headerText.find("kReflexSetConstantsUnavailableQueryLimit"), std::string::npos);
    EXPECT_NE(headerText.find("streamline_hook_g_ReflexSetConstantsUnavailableQueries"), std::string::npos);
}

// Session 20260814_014012: Streamline unload notifications cleared the feature-level
// hook slots while CE's identical low-level detours were still live. Every loaded-module
// scan then retried all exports and emitted thousands of paired "already hooked" / "failed"
// lines. Rediscovery must recover the retained trampoline and restore the feature-level
// state; genuine install failures remain visible with an initial burst and sparse heartbeat.
TEST(StreamlineRuntimePolicyTest, RediscoveredLiveInlineHooksReconcileWithoutFailureSpam) {
    namespace fs = std::filesystem;
    const std::string header = ce::test_source::ReadLogicalSource(
        fs::current_path() / "hook" / "apis" / "streamline_hook_internal.h");
    const std::string inlineHook = ce::test_source::ReadLogicalSource(
        fs::current_path() / "hook" / "wrappers" / "inline_hook.cpp");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(inlineHook.empty());

    const size_t installer = header.find("bool InstallInlineHookOnce(");
    ASSERT_NE(installer, std::string::npos);
    const size_t reconcile = header.find("InlineHook::TryGetInstalledTrampoline", installer);
    const size_t install = header.find("InlineHook::InstallPublished", installer);
    ASSERT_NE(reconcile, std::string::npos);
    ASSERT_NE(install, std::string::npos);
    EXPECT_LT(reconcile, install);
    EXPECT_NE(header.find("original = reinterpret_cast<T>(retainedTrampoline)", reconcile), std::string::npos);
    EXPECT_NE(header.find("installedFlag.store(true", reconcile), std::string::npos);
    EXPECT_NE(header.find("ShouldLogCadence(failureCount, 10, 300)", install), std::string::npos);

    EXPECT_NE(inlineHook.find("ReadProcessMemory(GetCurrentProcess()"), std::string::npos);
    EXPECT_NE(inlineHook.find("h.detour == detour && h.installed && InstalledEntryBytesMatch(h)"),
              std::string::npos);
    EXPECT_NE(inlineHook.find("ShouldLogCadence(duplicateCount, 4, 300)"), std::string::npos);
    EXPECT_EQ(inlineHook.find("FAILED: Target %p already hooked by us"), std::string::npos);
}

// Crash 20260812_042259 (dx12_fg_switch_test via Steam + RTSS): switching DLSS FG -> FSR FG
// unloads sl.dlss_g / sl.reflex BEFORE sl.interposer, and the HookThread's proactive feature
// resolution called sl.interposer!slGetFeatureFunction which dispatched into the already-unmapped
// sl.dlss_g -> DEP execute violation. The proactive scan must pin the queried modules and fail
// closed when a tracked sl.* unload is in flight; runtime-internal callers (which may run under
// the loader lock during SL DllMain) must not load libraries and only skip when the feature
// module is already gone.
TEST(StreamlineRuntimePolicyTest, FeatureResolutionSkipsStreamlineTeardownRace) {
    namespace fs = std::filesystem;
    const fs::path installSource = fs::current_path() / "hook" / "apis" / "streamline_hook_install.cpp";
    const fs::path hookSource = fs::current_path() / "hook" / "apis" / "streamline_hook.cpp";
    const fs::path resolveSource = fs::current_path() / "hook" / "apis" / "streamline_hook_resolve.cpp";
    const fs::path headerSource = fs::current_path() / "hook" / "apis" / "streamline_hook_internal.h";
    ASSERT_TRUE(fs::exists(installSource));
    ASSERT_TRUE(fs::exists(hookSource));
    ASSERT_TRUE(fs::exists(resolveSource));
    ASSERT_TRUE(fs::exists(headerSource));

    const std::string install = ce::test_source::ReadLogicalSource(installSource);
    const std::string hook = ce::test_source::ReadLogicalSource(hookSource);
    const std::string resolve = ce::test_source::ReadLogicalSource(resolveSource);
    const std::string header = ce::test_source::ReadLogicalSource(headerSource);
    ASSERT_FALSE(install.empty());
    ASSERT_FALSE(hook.empty());
    ASSERT_FALSE(resolve.empty());
    ASSERT_FALSE(header.empty());

    // Every tracked sl.* unload bumps a teardown generation that the resolve path snapshots.
    EXPECT_NE(header.find("streamline_hook_g_StreamlineModuleUnloadGeneration"), std::string::npos);
    EXPECT_NE(hook.find("streamline_hook_g_StreamlineModuleUnloadGeneration.fetch_add(1"), std::string::npos);

    // The HookThread's Init scan pins the queried modules; the runtime-activity retry path must
    // not (it can run under the loader lock where LoadLibrary is forbidden).
    EXPECT_NE(hook.find("ScanLoadedStreamlineModules(/*pinFeatureResolution=*/true)"), std::string::npos);
    EXPECT_NE(install.find("const bool foundModule = ScanLoadedStreamlineModules();"), std::string::npos);

    // The query guard pins EVERY loaded sl.* module (not only the feature plugin and the
    // interposer), fails closed while a teardown is in flight, and rejects the query when the
    // generation changed between the liveness check and the pins.
    EXPECT_NE(resolve.find("class ScopedStreamlineFeatureQueryGuard"), std::string::npos);
    EXPECT_NE(resolve.find("PinLoadedStreamlineModule"), std::string::npos);
    // The pin resolves the module by address; see
    // StreamlineFeatureQueryPinsByAddressNotByPath for why a path-based LoadLibrary is not a pin.
    EXPECT_NE(resolve.find("GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS"), std::string::npos);
    EXPECT_NE(resolve.find("OpenLoadedModuleSnapshotWithRetry"), std::string::npos);
    EXPECT_NE(resolve.find("IsStreamlineModuleNameForFeatureHooking(moduleNameOrPath)"), std::string::npos);
    EXPECT_NE(resolve.find("kMaxPinnedStreamlineFeatureQueryModules"), std::string::npos);
    EXPECT_NE(resolve.find("GetModuleHandleA(\"sl.interposer.dll\")"), std::string::npos);
    EXPECT_NE(resolve.find("GetModuleHandleA(\"sl.dlss_g.dll\")"), std::string::npos);
    EXPECT_NE(resolve.find("GetModuleHandleA(\"sl.reflex.dll\")"), std::string::npos);
    EXPECT_NE(resolve.find("streamline_hook_g_StreamlineModuleUnloadGeneration.load("), std::string::npos);

    // The pins must remain alive across every slGetFeatureFunction call. Crash 20260814_012246
    // exposed a lexical-lifetime bug where a correctly populated guard lived only inside the
    // proactiveScan conditional and released its pins before the first query.
    const size_t dlssgResolver = resolve.find("bool TryResolveDLSSGFeatureHooks(bool proactiveScan)");
    const size_t reflexResolver = resolve.find("bool TryResolveReflexFeatureHooks(bool proactiveScan)");
    ASSERT_NE(dlssgResolver, std::string::npos);
    ASSERT_NE(reflexResolver, std::string::npos);
    EXPECT_NE(resolve.find(
                  "ScopedStreamlineFeatureQueryGuard teardownGuard(proactiveScan ? \"sl.dlss_g.dll\" : nullptr);",
                  dlssgResolver),
              std::string::npos);
    EXPECT_NE(resolve.find(
                  "ScopedStreamlineFeatureQueryGuard teardownGuard(proactiveScan ? \"sl.reflex.dll\" : nullptr);",
                  reflexResolver),
              std::string::npos);
    const size_t dlssgQuery = resolve.find("originalGetFeatureFunction(streamline_hook_kSLFeatureDLSSG", dlssgResolver);
    const size_t reflexQuery = resolve.find("originalGetFeatureFunction(streamline_hook_kSLFeatureReflex", reflexResolver);
    ASSERT_NE(dlssgQuery, std::string::npos);
    ASSERT_NE(reflexQuery, std::string::npos);
    EXPECT_LT(dlssgQuery, reflexResolver);
    EXPECT_LT(reflexQuery, resolve.find("uint32_t QueryCapabilityMax", reflexResolver));

    // Non-proactive callers (runtime-internal, DllMain-safe) only check module presence.
    EXPECT_NE(resolve.find("bool TryResolveDLSSGFeatureHooks(bool proactiveScan)"), std::string::npos);
    EXPECT_NE(resolve.find("bool TryResolveReflexFeatureHooks(bool proactiveScan)"), std::string::npos);
    EXPECT_NE(resolve.find("} else if (!GetModuleHandleA(\"sl.dlss_g.dll\"))"), std::string::npos);
    EXPECT_NE(resolve.find("} else if (!GetModuleHandleA(\"sl.reflex.dll\"))"), std::string::npos);
}

// Crash 20260813_160845 (dx12_fg_switch_test, DLSS FG used between FSR sessions): the HookThread's
// proactive Reflex resolution held pins on sl.reflex + sl.interposer, but the interposer's
// slGetFeatureFunction dispatched into sl.dlss_d.dll, whose earlier unload had been silent (no CE
// hook slots in its range, so nothing logged) and could not be detected by the generation snapshot
// (the bump had already happened). Pin that every sl.* unload latches teardown-in-flight, every
// sl.* load clears it, both resolution paths fail closed while it is set, and returned function
// pointers are only hooked when no teardown was observed during the query and the pointer still
// belongs to a loaded module.
TEST(StreamlineRuntimePolicyTest, FeatureResolutionLatchesTeardownInFlightUntilNextStreamlineLoad) {
    namespace fs = std::filesystem;
    const fs::path hookSource = fs::current_path() / "hook" / "apis" / "streamline_hook.cpp";
    const fs::path resolveSource = fs::current_path() / "hook" / "apis" / "streamline_hook_resolve.cpp";
    const fs::path headerSource = fs::current_path() / "hook" / "apis" / "streamline_hook_internal.h";
    ASSERT_TRUE(fs::exists(hookSource));
    ASSERT_TRUE(fs::exists(resolveSource));
    ASSERT_TRUE(fs::exists(headerSource));

    const std::string hook = ce::test_source::ReadLogicalSource(hookSource);
    const std::string resolve = ce::test_source::ReadLogicalSource(resolveSource);
    const std::string header = ce::test_source::ReadLogicalSource(headerSource);
    ASSERT_FALSE(hook.empty());
    ASSERT_FALSE(resolve.empty());
    ASSERT_FALSE(header.empty());

    // The latch lives next to the unload-generation counter and is set by every tracked unload...
    EXPECT_NE(header.find("streamline_hook_g_StreamlineTeardownInFlight"), std::string::npos);
    EXPECT_NE(hook.find("streamline_hook_g_StreamlineTeardownInFlight.store(true"), std::string::npos);
    // ...and cleared by the next sl.* module load.
    EXPECT_NE(hook.find("streamline_hook_g_StreamlineTeardownInFlight.store(false"), std::string::npos);

    // Both resolution paths and the query guard fail closed while the teardown latch is set.
    const size_t resolveDLSSG = resolve.find("bool TryResolveDLSSGFeatureHooks(bool proactiveScan)");
    const size_t resolveReflex = resolve.find("bool TryResolveReflexFeatureHooks(bool proactiveScan)");
    ASSERT_NE(resolveDLSSG, std::string::npos);
    ASSERT_NE(resolveReflex, std::string::npos);
    EXPECT_NE(resolve.find("streamline_hook_g_StreamlineTeardownInFlight.load(", resolveDLSSG), std::string::npos);
    EXPECT_NE(resolve.find("streamline_hook_g_StreamlineTeardownInFlight.load(", resolveReflex), std::string::npos);
    const size_t guardClass = resolve.find("class ScopedStreamlineFeatureQueryGuard");
    ASSERT_NE(guardClass, std::string::npos);
    EXPECT_LT(guardClass, resolveDLSSG);

    // A returned function pointer is hooked only when no teardown was observed during the query
    // and the pointer still belongs to a loaded module (stale cached pointers must never be patched).
    EXPECT_NE(resolve.find("teardownObservedDuringQuery"), std::string::npos);
    EXPECT_NE(resolve.find("DoesAddressBelongToLoadedModule("), std::string::npos);
}

TEST(StreamlineRuntimePolicyTest, SuccessfulExplicitStreamlineEnableRetiresAbandonedFFXStartup) {
    namespace fs = std::filesystem;
    const std::string dlssg = ce::test_source::ReadLogicalSource(
        fs::current_path() / "hook" / "apis" / "streamline_hook_dlssg.cpp");
    const std::string startup = ce::test_source::ReadLogicalSource(
        fs::current_path() / "hook" / "apis" / "dx12_hook_fg_startup.cpp");
    ASSERT_FALSE(dlssg.empty());
    ASSERT_FALSE(startup.empty());

    const size_t successfulResult = dlssg.find("if (result == streamline_hook_kSlResultOk)");
    ASSERT_NE(successfulResult, std::string::npos);
    const size_t successfulEnable = dlssg.find("if (!pureObserverOnly && requestedEnabled)", successfulResult);
    ASSERT_NE(successfulEnable, std::string::npos);
    EXPECT_NE(dlssg.find("DX12_RetireProtectedOfficialFFXStartupForSuccessfulStreamlineEnable();", successfulEnable),
              std::string::npos);

    const size_t retireHelper =
        startup.find("void DX12_RetireProtectedOfficialFFXStartupForSuccessfulStreamlineEnable()");
    ASSERT_NE(retireHelper, std::string::npos);
    EXPECT_NE(startup.find("ShouldRetireProtectedOfficialFFXStartupForSuccessfulStreamlineEnable(", retireHelper),
              std::string::npos);
    EXPECT_NE(startup.find("DX12_ClearNativeFSRStartupConfigureArming(", retireHelper), std::string::npos);
}

TEST(StreamlineRuntimePolicyTest, HookSlotInvalidationMatchesUnloadedImageRange) {
    alignas(16) static unsigned char image[0x100];
    alignas(16) static unsigned char outsideImage[0x100];
    const void* base = image;
    const size_t size = sizeof(image);
    void* inRange = image + 0x40;
    void* pastEnd = image + sizeof(image);
    void* outside = outsideImage + 0x40;

    // Patched target inside the departing image.
    EXPECT_TRUE(
        ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(inRange, nullptr, base, size));
    // Saved original/export inside the departing image (import-fallback slots).
    EXPECT_TRUE(
        ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(nullptr, inRange, base, size));

    EXPECT_FALSE(
        ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(outside, pastEnd, base, size));
    EXPECT_FALSE(
        ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(nullptr, nullptr, base, size));
    EXPECT_FALSE(
        ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(inRange, inRange, nullptr, size));
    EXPECT_FALSE(
        ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(inRange, inRange, base, 0));
}

TEST(StreamlineRuntimePolicyTest, ReloadedCoreModuleMaskIsStaleWhenNoTargetBelongsToArrivingInstance) {
    EXPECT_TRUE(ce::streamline_runtime_policy::IsInstalledStreamlineModuleMaskStaleForReloadedModule(true, false));

    EXPECT_FALSE(ce::streamline_runtime_policy::IsInstalledStreamlineModuleMaskStaleForReloadedModule(true, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::IsInstalledStreamlineModuleMaskStaleForReloadedModule(false, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::IsInstalledStreamlineModuleMaskStaleForReloadedModule(false, true));
}

// Cyberpunk 20260816_045933: three sl.interposer instances existed within three seconds because
// CE's own loader redirect turned the feature-query pin into a load of the override copy. Each
// duplicate re-hooked the core exports and overwrote CE's SINGLE process-global forward pointer
// with a trampoline into that duplicate; when the duplicate was freed the slots were invalidated
// to null, and slSetTag / slSetTagForFrame / slSetD3DDevice / slEvaluateFeature then returned
// kSlResultErrorInvalidState without ever reaching Streamline. Six seconds later sl.dlss_g
// dereferenced null (READ from 0x8) on the state those calls never established.
TEST(StreamlineRuntimePolicyTest, HookSlotIsNotRetargetedWhileTheInstalledTargetIsStillMapped) {
    alignas(16) static unsigned char liveInstance[0x100];
    alignas(16) static unsigned char secondInstance[0x100];
    void* installedTarget = liveInstance + 0x40;
    void* duplicateTarget = secondInstance + 0x40;

    // A second live instance must never take over the slot.
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldRetargetStreamlineHookSlot(true, installedTarget,
                                                                                duplicateTarget, true));
    // An ordinary unload/reload generation must still re-hook the fresh instance.
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldRetargetStreamlineHookSlot(true, installedTarget,
                                                                               duplicateTarget, false));
    // First install, re-install of the same target, and a cleared slot stay unaffected.
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldRetargetStreamlineHookSlot(false, nullptr, duplicateTarget, false));
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldRetargetStreamlineHookSlot(true, nullptr, duplicateTarget, true));
    EXPECT_TRUE(
        ce::streamline_runtime_policy::ShouldRetargetStreamlineHookSlot(true, installedTarget, installedTarget, true));
}

TEST(StreamlineRuntimePolicyTest, InlineHookInstallConsultsTheRetargetGuard) {
    namespace fs = std::filesystem;
    const std::string header =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "apis" / "streamline_hook_internal.h");
    ASSERT_FALSE(header.empty());

    const size_t installer = header.find("bool InstallInlineHookOnce(void* target, void* detour");
    ASSERT_NE(installer, std::string::npos);
    const size_t guard = header.find("ShouldRetargetStreamlineHookSlot(", installer);
    ASSERT_NE(guard, std::string::npos);
    // The guard must run BEFORE the install/reconcile paths that publish a new trampoline.
    const size_t reconcile = header.find("TryGetInstalledTrampoline(", installer);
    const size_t install = header.find("InlineHook::InstallPublished(", installer);
    ASSERT_NE(reconcile, std::string::npos);
    ASSERT_NE(install, std::string::npos);
    EXPECT_LT(guard, reconcile);
    EXPECT_LT(guard, install);
    // Liveness comes from the loaded-module owner lookup, not from a stored flag.
    EXPECT_NE(header.find("DoesAddressBelongToLoadedModule(", installer), std::string::npos);
}

// The feature-query pin must resolve the module by ADDRESS. A path-based LoadLibrary is a fresh
// loader resolution that CE's own runtime-override redirect rewrites, which is how the duplicate
// sl.interposer instances above were created in the first place.
TEST(StreamlineRuntimePolicyTest, StreamlineFeatureQueryPinsByAddressNotByPath) {
    namespace fs = std::filesystem;
    const std::string resolve =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "apis" / "streamline_hook_resolve.cpp");
    ASSERT_FALSE(resolve.empty());

    const size_t pin = resolve.find("HMODULE PinLoadedStreamlineModule(HMODULE module)");
    ASSERT_NE(pin, std::string::npos);
    const size_t pinEnd = resolve.find("\n}", pin);
    ASSERT_NE(pinEnd, std::string::npos);
    const std::string body = resolve.substr(pin, pinEnd - pin);

    EXPECT_NE(body.find("GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS"), std::string::npos);
    EXPECT_EQ(body.find("LoadLibrary"), std::string::npos);
    EXPECT_EQ(body.find("GetModuleFileName"), std::string::npos);
}

// ---------------------------------------------------------------------------
// DLSSG activation-health monitor (session 20260702_094955): GTA cold-start DLSS FG reported ON
// (optionsMode=on, updateActive=1) but presents stayed at base rate all session — the health monitor must
// warn deterministically (sample streaks, not wall-clock) and only for ON-request samples so an OFF phase
// never extends or misattributes a streak.
// ---------------------------------------------------------------------------
TEST(StreamlineRuntimePolicyTest, DLSSGHealthTracksOnlySuccessfulOnRequestSamples) {
    EXPECT_TRUE(ce::streamline_runtime_policy::ShouldTrackDLSSGActivationHealthSample(true, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldTrackDLSSGActivationHealthSample(true, false));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldTrackDLSSGActivationHealthSample(false, true));
    EXPECT_FALSE(ce::streamline_runtime_policy::ShouldTrackDLSSGActivationHealthSample(false, false));
}

TEST(StreamlineRuntimePolicyTest, DLSSGInterpolationEvidenceRequiresGeneratedFramePresented) {
    // ==1: only the real frame reached presentation (the failing GTA session's steady value).
    EXPECT_FALSE(ce::streamline_runtime_policy::IsDLSSGInterpolationPresentEvidence(0));
    EXPECT_FALSE(ce::streamline_runtime_policy::IsDLSSGInterpolationPresentEvidence(1));
    // >=2: real + generated frames presented (2x FG); MFG presents more.
    EXPECT_TRUE(ce::streamline_runtime_policy::IsDLSSGInterpolationPresentEvidence(2));
    EXPECT_TRUE(ce::streamline_runtime_policy::IsDLSSGInterpolationPresentEvidence(4));
}

TEST(StreamlineRuntimePolicyTest, ConfiguredMfgFactorIsPublishedDuringHalfArmedStartup) {
    using ce::streamline_runtime_policy::ResolvePublishedDLSSFGMultiplier;

    EXPECT_EQ(0, ResolvePublishedDLSSFGMultiplier(false, 2, 4, 5));
    EXPECT_EQ(4, ResolvePublishedDLSSFGMultiplier(true, 2, 4, 5));
    EXPECT_EQ(3, ResolvePublishedDLSSFGMultiplier(true, 2, 4, 2))
        << "known runtime capability still clamps the configured factor";
    EXPECT_EQ(3, ResolvePublishedDLSSFGMultiplier(true, 3, 0, 5));
}

TEST(StreamlineRuntimePolicyTest, ReflexWrappersCoalesceNestedNativeSleepPacing) {
    namespace fs = std::filesystem;
    const std::string streamline = ce::test_source::ReadLogicalSource(
        fs::current_path() / "hook" / "apis" / "streamline_hook_api.cpp");
    const std::string reflex = ce::test_source::ReadLogicalSource(
        fs::current_path() / "hook" / "common" / "reflex_limiter.h");
    ASSERT_FALSE(streamline.empty());
    ASSERT_FALSE(reflex.empty());

    const size_t streamlineStart = streamline.find("slResult Hooked_slReflexSleep");
    const size_t streamlineEnd = streamline.find("slResult Hooked_slReflexSetOptions", streamlineStart);
    ASSERT_NE(streamlineStart, std::string::npos);
    ASSERT_NE(streamlineEnd, std::string::npos);
    const std::string streamlineBody = streamline.substr(streamlineStart, streamlineEnd - streamlineStart);
    EXPECT_NE(streamlineBody.find("BeginGameSleepBoundary(\"Streamline\")"), std::string::npos);
    EXPECT_NE(streamlineBody.find("EndGameSleepBoundary(ownsSleepBoundary"), std::string::npos);
    EXPECT_EQ(streamlineBody.find("ApplyHybridPacingBeforeNativeSleep()"), std::string::npos);

    const size_t nvapiStart = reflex.find("ReflexLimiter::ReflexDetour_Sleep");
    ASSERT_NE(nvapiStart, std::string::npos);
    const std::string nvapiBody = reflex.substr(nvapiStart);
    EXPECT_NE(nvapiBody.find("BeginGameSleepBoundary(\"NvAPI\")"), std::string::npos);
    EXPECT_NE(nvapiBody.find("EndGameSleepBoundary(ownsSleepBoundary"), std::string::npos);
}

TEST(StreamlineRuntimePolicyTest, DLSSGHealthWarnsAtStreakThenRepeatsSparsely) {
    using ce::streamline_runtime_policy::ShouldWarnDLSSGActiveButNotInterpolating;
    // Below the streak threshold: silent.
    EXPECT_FALSE(ShouldWarnDLSSGActiveButNotInterpolating(0, 8, 512));
    EXPECT_FALSE(ShouldWarnDLSSGActiveButNotInterpolating(7, 8, 512));
    // First warning exactly at the threshold.
    EXPECT_TRUE(ShouldWarnDLSSGActiveButNotInterpolating(8, 8, 512));
    // Then sparse repeats at the repeat cadence, silent in between.
    EXPECT_FALSE(ShouldWarnDLSSGActiveButNotInterpolating(9, 8, 512));
    EXPECT_FALSE(ShouldWarnDLSSGActiveButNotInterpolating(519, 8, 512));
    EXPECT_TRUE(ShouldWarnDLSSGActiveButNotInterpolating(520, 8, 512));
    EXPECT_TRUE(ShouldWarnDLSSGActiveButNotInterpolating(1032, 8, 512));
    // Degenerate configs never warn / never divide by zero.
    EXPECT_FALSE(ShouldWarnDLSSGActiveButNotInterpolating(100, 0, 512));
    EXPECT_FALSE(ShouldWarnDLSSGActiveButNotInterpolating(100, 8, 0));
    EXPECT_TRUE(ShouldWarnDLSSGActiveButNotInterpolating(8, 8, 0));
}


}  // namespace
