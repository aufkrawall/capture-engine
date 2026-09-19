#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "source_fragment_reader.h"

#include "../common/config.h"
#include "../hook/common/ngx_ota_policy.h"

namespace {

using ce::ngx_ota::DisableUpdaterEnvironmentValue;
using ce::ngx_ota::ImageBaseName;
using ce::ngx_ota::IsNgxUpdaterImage;
using ce::ngx_ota::ModeName;
using ce::ngx_ota::ShouldClearStreamlineOtaPreferences;
using ce::ngx_ota::ShouldRefuseUpdaterLaunch;
using ce::ngx_ota::ShouldSuppressConfiguredRuntimeOverrides;
using ce::ngx_ota::WritesEnvironment;

TEST(NgxOtaPolicy, ParsesTheThreeDocumentedModes) {
    EXPECT_EQ(ParseNgxOtaMode("default"), kNgxOtaModeDefault);
    EXPECT_EQ(ParseNgxOtaMode(""), kNgxOtaModeDefault);
    EXPECT_EQ(ParseNgxOtaMode("off"), kNgxOtaModeOff);
    EXPECT_EQ(ParseNgxOtaMode("OFF"), kNgxOtaModeOff);
    EXPECT_EQ(ParseNgxOtaMode("on"), kNgxOtaModeOn);
    EXPECT_EQ(ParseNgxOtaMode(" on "), kNgxOtaModeOn);
    EXPECT_EQ(ParseNgxOtaMode("\"on\""), kNgxOtaModeOn);
    EXPECT_EQ(ParseNgxOtaMode("0"), kNgxOtaModeOff);
    EXPECT_EQ(ParseNgxOtaMode("1"), kNgxOtaModeOn);
}

// A typo must not silently force an NGX policy in either direction: both
// forced modes change what the driver does, so only an explicit value may
// select one.
TEST(NgxOtaPolicy, UnrecognizedValuesFallBackToDefaultRatherThanAForcedMode) {
    for (const char* value : {"yes", "enable", "disabled", "auto", "2", "of", "onn"}) {
        EXPECT_EQ(ParseNgxOtaMode(value), kNgxOtaModeDefault) << value;
    }
}

TEST(NgxOtaPolicy, ParsesNgxLogLevels) {
    EXPECT_EQ(ParseNgxLogLevel("default"), kNgxLogLevelDefault);
    EXPECT_EQ(ParseNgxLogLevel(""), kNgxLogLevelDefault);
    EXPECT_EQ(ParseNgxLogLevel("off"), kNgxLogLevelOff);
    EXPECT_EQ(ParseNgxLogLevel("on"), kNgxLogLevelOn);
    EXPECT_EQ(ParseNgxLogLevel("verbose"), kNgxLogLevelVerbose);
    EXPECT_EQ(ParseNgxLogLevel("nonsense"), kNgxLogLevelDefault);
}

// `default` means "do not write the variable at all", which has no environment
// representation - writing "0" would be an explicit off, not an absence.
TEST(NgxOtaPolicy, LogLevelEnvironmentValueMapsOntoNgxOwnScale) {
    EXPECT_EQ(NgxLogLevelEnvironmentValue(kNgxLogLevelDefault), nullptr);
    EXPECT_STREQ(NgxLogLevelEnvironmentValue(kNgxLogLevelOff), "0");
    EXPECT_STREQ(NgxLogLevelEnvironmentValue(kNgxLogLevelOn), "1");
    EXPECT_STREQ(NgxLogLevelEnvironmentValue(kNgxLogLevelVerbose), "2");
}

TEST(NgxOtaPolicy, ImageBaseNameHandlesBothSeparatorsAndALeadingQuote) {
    EXPECT_STREQ(ImageBaseName("C:\\Windows\\System32\\nvngx_update.exe"), "nvngx_update.exe");
    EXPECT_STREQ(ImageBaseName("C:/drivers/nvngx_update.exe"), "nvngx_update.exe");
    EXPECT_STREQ(ImageBaseName("\"C:\\a b\\nvngx_update.exe\" -bootstrap"), "nvngx_update.exe\" -bootstrap");
    EXPECT_STREQ(ImageBaseName("nvngx_update.exe"), "nvngx_update.exe");
    EXPECT_STREQ(ImageBaseName(static_cast<const char*>(nullptr)), "");
}

// `_nvngx.dll` may pass the updater as lpApplicationName or only inside
// lpCommandLine, so both spellings have to resolve identically.
TEST(NgxOtaPolicy, RecognizesTheUpdaterAsPathOrCommandLine) {
    EXPECT_TRUE(IsNgxUpdaterImage("nvngx_update.exe"));
    EXPECT_TRUE(IsNgxUpdaterImage("NVNGX_UPDATE.EXE"));
    EXPECT_TRUE(IsNgxUpdaterImage(
        "C:\\Windows\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_b20cc8aeaed64fc2\\nvngx_update.exe"));
    EXPECT_TRUE(IsNgxUpdaterImage("\"C:\\drivers\\nvngx_update.exe\" -bootstrap -api 1"));
    EXPECT_TRUE(IsNgxUpdaterImage("C:\\drivers\\nvngx_update.exe -forced_update"));
}

// `HookedCreateProcessW` used to convert its argument into a MAX_PATH narrow
// buffer and decide on that. WideCharToMultiByte writes NOTHING when the
// destination is too small, so a command line longer than 260 characters left
// the buffer empty and the updater went unrecognized - a silent escape, not an
// error. The decision is now made on the caller's own wide string, so the same
// input must resolve identically at both widths and at any length.
TEST(NgxOtaPolicy, WideAndNarrowSpellingsAgreeIncludingBeyondMaxPath) {
    EXPECT_TRUE(IsNgxUpdaterImage(L"nvngx_update.exe"));
    EXPECT_TRUE(IsNgxUpdaterImage(L"NVNGX_UPDATE.EXE"));
    EXPECT_TRUE(IsNgxUpdaterImage(L"C:/drivers/nvngx_update.exe -forced_update"));
    EXPECT_FALSE(IsNgxUpdaterImage(L"nvngx_updater.exe"));
    EXPECT_FALSE(IsNgxUpdaterImage(L""));
    EXPECT_FALSE(IsNgxUpdaterImage(static_cast<const wchar_t*>(nullptr)));
    EXPECT_STREQ(ImageBaseName(L"C:\\a\\nvngx_update.exe"), L"nvngx_update.exe");

    // The real shape: a quoted DriverStore path plus the arguments the driver
    // passes (`-api update -cmsid ... -feature ... -bootstrap -gpuarch ...`),
    // padded past MAX_PATH. The old narrow path saw "" for this.
    const std::wstring longCommandLine =
        L"\"C:\\Windows\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_" +
        std::wstring(300, L'b') + L"\\nvngx_update.exe\" -api update -cmsid 101654711 -feature deepdvc -bootstrap";
    ASSERT_GT(longCommandLine.size(), 260u);
    EXPECT_TRUE(IsNgxUpdaterImage(longCommandLine.c_str()));

    // And the narrow spelling of the same string still agrees, so the two
    // overloads cannot drift apart.
    const std::string narrowEquivalent(longCommandLine.begin(), longCommandLine.end());
    EXPECT_TRUE(IsNgxUpdaterImage(narrowEquivalent.c_str()));
}

// The refusal must never reach a different NVIDIA binary, least of all one whose
// name merely starts or ends the same way.
TEST(NgxOtaPolicy, DoesNotMatchAnyNeighbouringImage) {
    EXPECT_FALSE(IsNgxUpdaterImage("nvngx_update_helper.exe"));
    EXPECT_FALSE(IsNgxUpdaterImage("nvngx_updater.exe"));
    EXPECT_FALSE(IsNgxUpdaterImage("my_nvngx_update.exe.exe"));
    EXPECT_FALSE(IsNgxUpdaterImage("nvngx.dll"));
    EXPECT_FALSE(IsNgxUpdaterImage("nvcontainer.exe"));
    EXPECT_FALSE(IsNgxUpdaterImage("update.exe"));
    EXPECT_FALSE(IsNgxUpdaterImage(""));
    EXPECT_FALSE(IsNgxUpdaterImage(static_cast<const char*>(nullptr)));
}

TEST(NgxOtaPolicy, OnlyOffRefusesTheUpdaterLaunch) {
    EXPECT_TRUE(ShouldRefuseUpdaterLaunch(kNgxOtaModeOff, /*targetIsUpdater=*/true));
    EXPECT_FALSE(ShouldRefuseUpdaterLaunch(kNgxOtaModeDefault, true));
    EXPECT_FALSE(ShouldRefuseUpdaterLaunch(kNgxOtaModeOn, true));
    // No mode may ever refuse a process that is not the updater.
    for (uint8_t mode : {kNgxOtaModeDefault, kNgxOtaModeOff, kNgxOtaModeOn}) {
        EXPECT_FALSE(ShouldRefuseUpdaterLaunch(mode, /*targetIsUpdater=*/false)) << ModeName(mode);
    }
}

// `off` publishes the suppression, `on` clears an inherited one, and `default`
// leaves the environment exactly as the process received it.
TEST(NgxOtaPolicy, EnvironmentValueDistinguishesSuppressClearAndLeaveAlone) {
    EXPECT_STREQ(DisableUpdaterEnvironmentValue(kNgxOtaModeOff), "1");
    EXPECT_STREQ(DisableUpdaterEnvironmentValue(kNgxOtaModeOn), "");
    EXPECT_EQ(DisableUpdaterEnvironmentValue(kNgxOtaModeDefault), nullptr);

    EXPECT_TRUE(WritesEnvironment(kNgxOtaModeOff));
    EXPECT_TRUE(WritesEnvironment(kNgxOtaModeOn));
    EXPECT_FALSE(WritesEnvironment(kNgxOtaModeDefault));
}

// The two forced modes act on opposite sides and must never overlap: `on` stands
// CE's own DLL overrides down so the driver's OTA files load, while `off` clears
// Streamline's OTA preference bits so they do not.
TEST(NgxOtaPolicy, ForcedModesActOnOppositeMechanismsAndNeverBoth) {
    EXPECT_TRUE(ShouldSuppressConfiguredRuntimeOverrides(kNgxOtaModeOn));
    EXPECT_FALSE(ShouldSuppressConfiguredRuntimeOverrides(kNgxOtaModeOff));
    EXPECT_FALSE(ShouldSuppressConfiguredRuntimeOverrides(kNgxOtaModeDefault));

    EXPECT_TRUE(ShouldClearStreamlineOtaPreferences(kNgxOtaModeOff));
    EXPECT_FALSE(ShouldClearStreamlineOtaPreferences(kNgxOtaModeOn));
    EXPECT_FALSE(ShouldClearStreamlineOtaPreferences(kNgxOtaModeDefault));

    for (uint8_t mode : {kNgxOtaModeDefault, kNgxOtaModeOff, kNgxOtaModeOn}) {
        EXPECT_FALSE(ShouldSuppressConfiguredRuntimeOverrides(mode) && ShouldClearStreamlineOtaPreferences(mode))
            << ModeName(mode);
    }
}

// `default` is the shipped behaviour and must remain completely inert: no
// refusal, no environment write, no override suppression, no preference edit.
TEST(NgxOtaPolicy, DefaultModeChangesNothing) {
    EXPECT_FALSE(ShouldRefuseUpdaterLaunch(kNgxOtaModeDefault, true));
    EXPECT_EQ(DisableUpdaterEnvironmentValue(kNgxOtaModeDefault), nullptr);
    EXPECT_FALSE(ShouldSuppressConfiguredRuntimeOverrides(kNgxOtaModeDefault));
    EXPECT_FALSE(ShouldClearStreamlineOtaPreferences(kNgxOtaModeDefault));
    EXPECT_FALSE(WritesEnvironment(kNgxOtaModeDefault));
}

TEST(NgxOtaPolicy, ModeAndRefusalTablesCoverEveryDefinedValue) {
    EXPECT_TRUE(IsNgxOtaMode(kNgxOtaModeDefault));
    EXPECT_TRUE(IsNgxOtaMode(kNgxOtaModeOn));
    EXPECT_FALSE(IsNgxOtaMode(static_cast<uint8_t>(kNgxOtaModeOn + 1)));
    EXPECT_TRUE(IsNgxLogLevel(kNgxLogLevelVerbose));
    EXPECT_FALSE(IsNgxLogLevel(static_cast<uint8_t>(kNgxLogLevelVerbose + 1)));

    EXPECT_STREQ(ModeName(kNgxOtaModeDefault), "default");
    EXPECT_STREQ(ModeName(kNgxOtaModeOff), "off");
    EXPECT_STREQ(ModeName(kNgxOtaModeOn), "on");
}

// Every refusal reason the hook can publish must have user-facing text, or the
// injector would report an empty explanation.
TEST(RuntimeOverrideRefusal, EveryReasonExceptNoneHasExplanatoryText) {
    EXPECT_STREQ(RuntimeOverrideRefusalText(kRuntimeOverrideRefusalNone), "");
    for (uint32_t reason : {kRuntimeOverrideRefusalForeignStreamlineCore, kRuntimeOverrideRefusalGenerationMismatch,
                            kRuntimeOverrideRefusalDuplicateModule, kRuntimeOverrideRefusalNgxOtaForcedOn}) {
        const char* text = RuntimeOverrideRefusalText(reason);
        ASSERT_NE(text, nullptr) << reason;
        EXPECT_GT(std::string(text).size(), 20u) << reason;
    }
    // An unknown reason must read as "nothing to report" rather than fabricate
    // an explanation.
    EXPECT_STREQ(RuntimeOverrideRefusalText(9999u), "");
}


// The slInit route is a source-level contract as much as a behavioural one: it
// shipped once as dead code because it was wired to the wrong place and used
// the wrong hooking mechanism, and neither mistake could fail a unit test or a
// build. Session 20260918_221342 is the evidence - the route registered at
// 22:13:54.785 while the runtime had already resolved sl.common, which loads
// from inside slInit, at 22:13:53.952. These pin the two properties that make
// the difference so a refactor cannot quietly undo them.
TEST(NgxOtaSlInitRoute, InstallsFromTheConfigLoadPathRatherThanGenerationClassification) {
    namespace fs = std::filesystem;
    const std::string hookThread =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "main_hookthread.cpp");
    ASSERT_FALSE(hookThread.empty());

    const size_t publish = hookThread.find("ce::ngx_ota::PublishPolicy(");
    ASSERT_NE(publish, std::string::npos) << "the NGX policy publication must exist";
    const size_t install = hookThread.find("ce::streamline_ota::InstallSlInitRouteIfConfigured()", publish);
    EXPECT_NE(install, std::string::npos)
        << "the slInit route must be installed from the config-load path, immediately after the policy exists";

    // The old wiring hung off the ABI-sensitive generation classification, which
    // runs off GetProcAddress observation and lands after slInit.
    const std::string install_unit =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "apis" / "streamline_hook_install.cpp");
    ASSERT_FALSE(install_unit.empty());
    EXPECT_EQ(install_unit.find("streamline_ota::"), std::string::npos)
        << "the slInit route must not be tied to the hook-time generation classification again";
}

TEST(NgxOtaSlInitRoute, PatchesTheImportTableAndNotOnlyTheDynamicRoute) {
    namespace fs = std::filesystem;
    const std::string route =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "apis" / "streamline_ota_preferences.cpp");
    ASSERT_FALSE(route.empty());

    // A title that links sl.interposer statically - Alan Wake 2 does - calls
    // slInit through its own import table. A GetProcAddress-time route alone
    // never sees that call.
    EXPECT_NE(route.find("PatchIATAllModules(\"sl.interposer.dll\", \"slInit\""), std::string::npos)
        << "the static-import path must be covered by an IAT patch";
    EXPECT_NE(route.find("RegisterDynamicHookFiltered(\"slInit\""), std::string::npos)
        << "the GetProcAddress path must stay covered too";

    // The original has to be resolved before any slot is repointed, or a call
    // arriving mid-install finds nothing to forward to.
    const size_t resolveOriginal = route.find("GetProcAddress(interposer, \"slInit\")");
    const size_t patch = route.find("PatchIATAllModules(\"sl.interposer.dll\"");
    ASSERT_NE(resolveOriginal, std::string::npos);
    ASSERT_NE(patch, std::string::npos);
    EXPECT_LT(resolveOriginal, patch) << "the original must be resolved before the first slot is repointed";

    // Generation is resolved from the loaded module, not passed in.
    EXPECT_NE(route.find("LiveGenerationFromLoadedInterposer()"), std::string::npos)
        << "the route must resolve the generation from the mapped interposer itself";
}

// ERROR_PARTIAL_COPY became reachable when the process-start source stopped
// being a 0.5 s WMI poll, because CE now looks while the target's PEB module
// list is still being built. It must be retried like ACCESS_DENIED.
TEST(NgxOtaSlInitRoute, ModuleProbeRetriesThePartialCopyRace) {
    namespace fs = std::filesystem;
    const std::string manager =
        ce::test_source::ReadLogicalSource(fs::current_path() / "captureengine" / "injection_manager.cpp");
    ASSERT_FALSE(manager.empty());
    EXPECT_NE(manager.find("ERROR_PARTIAL_COPY"), std::string::npos)
        << "the early-enumeration race must be recognized, not just ACCESS_DENIED";
    // The old message promised timing behaviour that does not exist:
    // ShouldInjectAfterGraphicsProbe ignores d3d12Loaded and injects either way.
    // Matched on the format-string fragment rather than the bare phrase: the
    // fix quotes the old wording in a comment to explain what changed, and a
    // test that cannot tell code from commentary is a test that fails for the
    // wrong reason.
    EXPECT_EQ(manager.find("\"continuing with conservative non-D3D12 injection timing\""), std::string::npos)
        << "the log must not claim a timing path that ShouldInjectAfterGraphicsProbe does not implement";
}


// The refusal is only as good as the moment the mode becomes knowable.
//
// Session 20260918_223542 created nine nvngx_update.exe processes at 22:35:48,
// every one of them parented to the game, while CE published its policy at
// 22:35:49.072 and only began refusing at 22:35:49.170. The CreateProcess hook
// was already installed the whole time - what it lacked was an answer, because
// CurrentMode returned "default" until the hook thread's own config load. The
// injector had published the resolved value at 22:35:42.842, before the game
// existed.
TEST(NgxOtaEarlyMode, ModeResolvesFromSharedMemoryBeforeTheHookThreadPublishes) {
    namespace fs = std::filesystem;
    const std::string runtime =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "common" / "ngx_ota_runtime.cpp");
    ASSERT_FALSE(runtime.empty());

    const size_t current = runtime.find("uint8_t CurrentMode()");
    ASSERT_NE(current, std::string::npos);
    // CurrentMode delegates to the shared early resolve, which is also what
    // DllMain calls; follow that one hop rather than requiring the read to be
    // inlined here, but still require it to be reached.
    const size_t delegate = runtime.find("ResolveEarlyModeFromPublishedConfig()", current);
    EXPECT_NE(delegate, std::string::npos)
        << "CurrentMode must resolve the injector's published mode rather than answering default until told";
    const size_t resolver = runtime.find("uint8_t ResolveEarlyModeFromPublishedConfig()");
    ASSERT_NE(resolver, std::string::npos);
    EXPECT_NE(runtime.find("ReadModeFromSharedMemory(", resolver), std::string::npos)
        << "and that resolve must be the shared-memory read, not a second source of truth";

    // It must read the published mode, which is the resolved profile's value,
    // not re-parse config.ini from a path.
    EXPECT_NE(runtime.find("graphicsConfig.ngxOtaMode"), std::string::npos)
        << "the early answer must come from the published resolved config";

    // Reachable from the DllMain-time CreateProcess path, so it must not do
    // anything that takes the loader lock.
    EXPECT_EQ(runtime.find("LoadLibrary"), std::string::npos)
        << "the early resolve runs on a loader-lock-reachable path and must not load anything";
    EXPECT_NE(runtime.find("OpenFileMappingW"), std::string::npos)
        << "the early resolve should use shared memory, which needs no loader lock";
}

// "Installed" and "effective" were indistinguishable in session 20260918_223542:
// the route went in at 22:35:49.085 with the IAT patched, the OTA core still won
// at 22:35:49.249, and nothing was logged either way because the hook only
// reported when it actually cleared bits.
TEST(NgxOtaSlInitRoute, EveryOutcomeOfTheHookIsDistinguishableInTheLog) {
    namespace fs = std::filesystem;
    const std::string route =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "apis" / "streamline_ota_preferences.cpp");
    ASSERT_FALSE(route.empty());

    // Entry is counted before any early return, or "did the call reach CE"
    // cannot be answered for the paths that return early.
    const size_t hook = route.find("Hooked_slInit(");
    ASSERT_NE(hook, std::string::npos);
    const size_t entryCount = route.find("g_EntryCount.fetch_add", hook);
    const size_t firstReturn = route.find("return original(", hook);
    ASSERT_NE(entryCount, std::string::npos) << "slInit entries must be counted";
    ASSERT_NE(firstReturn, std::string::npos);
    EXPECT_LT(entryCount, firstReturn) << "the entry must be recorded before any path can return";

    // Each outcome has to be separable from the others.
    EXPECT_NE(route.find("NOT recognized"), std::string::npos) << "a rejected struct layout must say so";
    EXPECT_NE(route.find("already disabled by the game"), std::string::npos)
        << "an already-clear flag set must be distinguishable from never being seen";
    EXPECT_NE(route.find("cleared eAllowOTA"), std::string::npos) << "the success case must stay reported";

    EXPECT_NE(route.find("bool WasSlInitObserved()"), std::string::npos);
    EXPECT_NE(route.find("bool WasSlInitRouteInstalled()"), std::string::npos);
}

// The verdict has to be stated where it is decidable - at the moment the foreign
// core is observed - or the log leaves the reader to infer it, which is exactly
// what cost a session.
TEST(NgxOtaSlInitRoute, ForeignCoreObservationReportsWhyTheStripDidNotPrevent) {
    namespace fs = std::filesystem;
    const std::string redirect =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "main_redirect.cpp");
    ASSERT_FALSE(redirect.empty());

    const size_t latch = redirect.find("g_ForeignStreamlineCoreObserved.exchange(true");
    ASSERT_NE(latch, std::string::npos);
    const size_t verdict = redirect.find("WasSlInitRouteInstalled()", latch);
    EXPECT_NE(verdict, std::string::npos)
        << "the foreign-core observation must pair itself with whether the slInit route was installed and used";
    EXPECT_NE(redirect.find("WasSlInitObserved()", latch), std::string::npos)
        << "and with whether the call actually came through CE";
}


// Where this is installed has now been wrong twice, each time for a different
// reason, and each time the symptom was silence rather than a failure:
//
//   1. Off the hook-time generation classification (22:13:54.785), which runs
//      after slInit entirely.
//   2. Off the hook thread's config load (22:47:47.688), 551 ms after CE's own
//      DllMain at 22:47:47.137 - and the game's slInit landed in that gap,
//      giving "installed=1, seen through CE=0".
//
// DllMain is the earliest point CE exists in the process, and the route needs
// nothing beyond that: the generation comes from the mapped interposer's file
// version, the mode from the injector's published shared memory. Both reads.
TEST(NgxOtaSlInitRoute, InstalledFromDllMainBesideTheLoaderHooks) {
    namespace fs = std::filesystem;
    const std::string dllMain =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "main_dllmain.cpp");
    ASSERT_FALSE(dllMain.empty());

    const size_t loaderHooks = dllMain.find("InstallKernel32LoaderHooks(\"DllMain\")");
    ASSERT_NE(loaderHooks, std::string::npos) << "the DllMain loader-hook install must exist";
    const size_t route = dllMain.find("ce::streamline_ota::InstallSlInitRouteIfConfigured()", loaderHooks);
    EXPECT_NE(route, std::string::npos)
        << "the slInit route must be installed from DllMain, not left to the hook thread";

    // It must come after the loader hooks: those are what let CE observe and
    // redirect module loads at all, and they are the cheaper of the two.
    EXPECT_GT(route, loaderHooks);

    // The graphics IAT work must not stand between them - that is the 330 ms of
    // patching that already cost the loader hooks their early window once.
    const size_t wrapperHooks = dllMain.find("InitializeWrapperHooks()");
    if (wrapperHooks != std::string::npos) {
        EXPECT_LT(route, wrapperHooks)
            << "the slInit route must precede the graphics IAT work, like the loader hooks do";
    }
}

// Refusing a launch is the backstop, not the goal. `__NGX_DISABLE_UPDATER` is
// the only mechanism that stops the NGX core from *attempting* one, and it is
// only read once - so publishing it from the hook thread's config load was
// always too late by construction. Session 20260918_224737: DllMain at
// 22:47:47.137, the variable published at 22:47:47.670.
TEST(NgxOtaEarlyMode, DisableUpdaterEnvironmentIsPublishedFromDllMainNotTheHookThread) {
    namespace fs = std::filesystem;
    const std::string dllMain =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "main_dllmain.cpp");
    ASSERT_FALSE(dllMain.empty());

    const size_t loaderHooks = dllMain.find("InstallKernel32LoaderHooks(\"DllMain\")");
    ASSERT_NE(loaderHooks, std::string::npos);
    const size_t early = dllMain.find("ce::ngx_ota::ApplyEarlyPolicyFromPublishedConfig()", loaderHooks);
    EXPECT_NE(early, std::string::npos)
        << "the OTA environment must be applied from DllMain, where it can still precede the NGX core's read";

    // Behind the CreateProcess hook, so a launch arriving between the two is
    // still refused, and ahead of the graphics IAT work, which cost the loader
    // hooks 330 ms once already.
    EXPECT_GT(early, loaderHooks);
    const size_t wrapperHooks = dllMain.find("InitializeWrapperHooks()");
    if (wrapperHooks != std::string::npos) {
        EXPECT_LT(early, wrapperHooks);
    }

    const std::string runtime =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "common" / "ngx_ota_runtime.cpp");
    ASSERT_FALSE(runtime.empty());
    const size_t apply = runtime.find("void ApplyEarlyPolicyFromPublishedConfig()");
    ASSERT_NE(apply, std::string::npos);
    EXPECT_NE(runtime.find("ApplyUpdaterEnvironment(", apply), std::string::npos)
        << "the early path must write the environment, not merely resolve the mode";
}

// Applying the injector's mode in DllMain creates a case that did not exist
// when only the hook thread wrote the variable: the injector publishes the
// resolved profile, the hook thread parses the local config.ini, and those can
// disagree. If the authoritative answer turns out to be `default`, CE's earlier
// write has to be undone - `default` is pinned inert everywhere else, and a
// leftover suppression would make it quietly mean `off`.
TEST(NgxOtaEarlyMode, DefaultUndoesAnEarlierWriteInsteadOfLeavingSuppressionBehind) {
    namespace fs = std::filesystem;
    const std::string runtime =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "common" / "ngx_ota_runtime.cpp");
    ASSERT_FALSE(runtime.empty());

    const size_t apply = runtime.find("void ApplyUpdaterEnvironment(");
    ASSERT_NE(apply, std::string::npos);

    // The inherited value must be captured before CE's first write can replace
    // it, or "restore" would restore CE's own suppression.
    const size_t capture = runtime.find("CaptureInheritedDisableUpdaterOnce()", apply);
    const size_t write = runtime.find("WriteEnvironmentVariable(kDisableUpdaterVariable", apply);
    ASSERT_NE(capture, std::string::npos) << "the inherited value must be captured";
    ASSERT_NE(write, std::string::npos);
    EXPECT_LT(capture, write) << "the capture must precede any write CE makes";

    EXPECT_NE(runtime.find("g_InheritedPresent ? g_InheritedValue : nullptr"), std::string::npos)
        << "restoring must reproduce absence as absence, not as an empty-but-present variable";

    // And the hook thread's publication must still reach the writer for
    // `default`, or there is nothing to undo it with.
    const size_t publish = runtime.find("void PublishPolicy(");
    ASSERT_NE(publish, std::string::npos);
    EXPECT_NE(runtime.find("firstApplication || previousOta != resolvedOta", publish), std::string::npos)
        << "a first publication must call the environment writer even when it resolves to default";
}

// The old lazy resolve latched a one-shot flag BEFORE reading, so a second
// thread arriving during the read was told "default" - and "default" on an
// nvngx_update.exe launch is precisely the answer this path exists to avoid.
// It also cached that answer when no CE host had published yet.
TEST(NgxOtaEarlyMode, EarlyResolveNeitherRacesNorCachesAnUnansweredRead) {
    namespace fs = std::filesystem;
    const std::string runtime =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "common" / "ngx_ota_runtime.cpp");
    ASSERT_FALSE(runtime.empty());

    EXPECT_EQ(runtime.find("g_EarlyModeResolved.exchange("), std::string::npos)
        << "latching the flag before the read hands a concurrent caller the default answer";

    const size_t resolve = runtime.find("uint8_t ResolveEarlyModeFromPublishedConfig()");
    ASSERT_NE(resolve, std::string::npos);
    const size_t answered = runtime.find("if (!answered) {", resolve);
    const size_t store = runtime.find("g_EarlyModeResolved.store(true", resolve);
    ASSERT_NE(answered, std::string::npos) << "an unanswered read must be distinguishable from a published default";
    ASSERT_NE(store, std::string::npos);
    EXPECT_LT(answered, store) << "only a host that actually answered may be cached";
}

// CE's CreateProcess hook is an IAT snapshot: DllMain, then once more on the
// hook thread. `_nvngx.dll` and `nvngx.dll` both import CreateProcessA/W and
// map when the game initialises DLSS, which a title with an in-game toggle does
// long after both passes. The loader half of this is gated on configured path
// overrides; the process-creation half must not be, because ngx_ota=off and
// child injection are independent of those.
TEST(NgxOtaLateModules, CreateProcessImportsArePatchedOnEveryLateLoadedModule) {
    namespace fs = std::filesystem;
    const std::string detect =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "main_overlay_detect.cpp");
    ASSERT_FALSE(detect.empty());
    EXPECT_NE(detect.find("PatchProcessCreationIatForLateLoadedModule(module, moduleNameOrPath)"), std::string::npos)
        << "the module-load notification must repair the CreateProcess snapshot, not only the loader one";

    const std::string redirect =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "main_redirect.cpp");
    ASSERT_FALSE(redirect.empty());
    const size_t fn = redirect.find("void PatchProcessCreationIatForLateLoadedModule(");
    ASSERT_NE(fn, std::string::npos);
    const size_t end = redirect.find("\n}\n", fn);
    ASSERT_NE(end, std::string::npos);
    const std::string body = redirect.substr(fn, end - fn);

    EXPECT_EQ(body.find("NeedsLoaderRedirectionHook()"), std::string::npos)
        << "gating this on configured DLL overrides would leave a plain ngx_ota=off profile uncovered";
    EXPECT_NE(body.find("\"CreateProcessA\""), std::string::npos);
    EXPECT_NE(body.find("\"CreateProcessW\""), std::string::npos);

    // A repointed slot with no resolvable original fails every launch from that
    // module, so the guard has to precede the first patch.
    const size_t guard = body.find("GetOriginalCreateProcessA()");
    const size_t firstPatch = body.find("IATHook::PatchIAT(");
    ASSERT_NE(guard, std::string::npos) << "the original must be resolvable before any slot is repointed";
    ASSERT_NE(firstPatch, std::string::npos);
    EXPECT_LT(guard, firstPatch);
}

// The refusal decision must not pass through a fixed-size narrow conversion.
TEST(NgxOtaLateModules, CreateProcessWDecidesOnTheWideStringBeforeConverting) {
    namespace fs = std::filesystem;
    const std::string injection =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "main_injection.cpp");
    ASSERT_FALSE(injection.empty());

    const size_t hook = injection.find("BOOL WINAPI HookedCreateProcessW(");
    ASSERT_NE(hook, std::string::npos);
    const size_t refuse = injection.find("ce::ngx_ota::ShouldRefuseProcessLaunch(ngxTarget)", hook);
    const size_t convert = injection.find("WideCharToMultiByte(", hook);
    ASSERT_NE(refuse, std::string::npos) << "the wide overload must be the one the W hook calls";
    ASSERT_NE(convert, std::string::npos);
    EXPECT_LT(refuse, convert) << "the NGX decision must precede any narrowing of the caller's string";
}

}  // namespace
