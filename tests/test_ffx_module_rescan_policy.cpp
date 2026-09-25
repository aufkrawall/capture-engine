#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../hook/common/ffx_module_rescan_policy.h"
#include "source_fragment_reader.h"

// GTA session 20260925_225006: the hook thread repeated the FFX install pass's IAT walks and cached-slot scan
// (lock cmpxchg over every 8-byte slot of GTA's 39 MB .data) once a second for the whole FSR FG session,
// ~115 ms each time, after the first pass had already routed everything. The expensive half now runs only
// when something it could find has changed.

namespace {

using ce::ffx_module_rescan::Commit;
using ce::ffx_module_rescan::Decide;
using ce::ffx_module_rescan::Inputs;
using ce::ffx_module_rescan::Reason;
using ce::ffx_module_rescan::State;

const void* const kRuntime = reinterpret_cast<const void*>(0x7FF000000000ull);
const void* const kReloadedRuntime = reinterpret_cast<const void*>(0x7FF100000000ull);

Inputs Live(std::uint64_t moduleSet, std::uint64_t evidence, const void* runtime = kRuntime) {
    Inputs inputs;
    inputs.runtimeModule = runtime;
    inputs.loaderNotificationsLive = true;
    inputs.moduleSetGeneration = moduleSet;
    inputs.unroutedCallEvidence = evidence;
    return inputs;
}

TEST(FFXModuleRescanPolicy, FirstPassAlwaysSweeps) {
    State state;
    EXPECT_EQ(Decide(state, Live(5, 0)), Reason::kNewRuntimeModule);
}

TEST(FFXModuleRescanPolicy, SteadyStateSkipsTheSweep) {
    State state;
    Commit(state, Live(5, 0));
    for (int pass = 0; pass < 100; ++pass) {
        EXPECT_EQ(Decide(state, Live(5, 0)), Reason::kNone);
    }
}

TEST(FFXModuleRescanPolicy, ModuleLoadOrUnloadTriggersOneSweep) {
    State state;
    Commit(state, Live(5, 0));
    ASSERT_EQ(Decide(state, Live(6, 0)), Reason::kModuleSetChanged);
    Commit(state, Live(6, 0));
    EXPECT_EQ(Decide(state, Live(6, 0)), Reason::kNone);
}

TEST(FFXModuleRescanPolicy, UnroutedCallTriggersASweep) {
    State state;
    Commit(state, Live(5, 0));
    EXPECT_EQ(Decide(state, Live(5, 1)), Reason::kUnroutedCallObserved);
}

TEST(FFXModuleRescanPolicy, RuntimeReloadAtANewBaseSweeps) {
    State state;
    Commit(state, Live(5, 0));
    EXPECT_EQ(Decide(state, Live(5, 0, kReloadedRuntime)), Reason::kNewRuntimeModule);
}

TEST(FFXModuleRescanPolicy, WithoutLoaderNotificationsEveryPassSweeps) {
    State state;
    Inputs blind = Live(1, 0);
    blind.loaderNotificationsLive = false;
    Commit(state, blind);
    EXPECT_EQ(Decide(state, blind), Reason::kLoaderNotificationsUnavailable);
}

TEST(FFXModuleRescanPolicy, ChangeDuringASweepIsNotLost) {
    // Inputs are sampled before the sweep and committed after it: a module that loads mid-sweep leaves the
    // live generation ahead of the committed one, so the next pass sweeps again.
    State state;
    const Inputs sampled = Live(5, 0);
    Commit(state, sampled);
    EXPECT_EQ(Decide(state, Live(6, 0)), Reason::kModuleSetChanged);
}

std::string ReadSource(const char* relativePath) {
    return ce::test_source::ReadFile(std::filesystem::current_path() / relativePath);
}

std::string Between(const std::string& source, const std::string& begin, const std::string& end) {
    const size_t start = source.find(begin);
    if (start == std::string::npos) {
        return {};
    }
    const size_t stop = source.find(end, start + begin.size());
    return source.substr(start, stop == std::string::npos ? std::string::npos : stop - start);
}

TEST(FFXModuleRescanSource, BothSweepsAreGatedAndInputsAreSampledBeforeThem) {
    const std::string install = ReadSource("hook/apis/ffx_hook_install.cpp");
    const std::string body = Between(install, "bool ffx_hook_InstallHooksForModule(", "\nbool WriteFfxExportEntryByte(");
    ASSERT_FALSE(body.empty());

    const size_t decide = body.find("ffx_hook_DecideModuleSweep(hModule)");
    const size_t firstIat = body.find("PatchIATAllModules(");
    const size_t refresh = body.find("ce::ffx_cached_pointer_router::Refresh(");
    const size_t commit = body.find("ffx_hook_CompleteModuleSweep(sweep");
    ASSERT_NE(decide, std::string::npos);
    ASSERT_NE(firstIat, std::string::npos);
    ASSERT_NE(refresh, std::string::npos);
    ASSERT_NE(commit, std::string::npos);
    EXPECT_LT(decide, firstIat);
    EXPECT_LT(refresh, commit);

    size_t searchFrom = 0;
    int gatedIatWalks = 0;
    while ((searchFrom = body.find("if (allowIATHooks && runSweeps) {", searchFrom)) != std::string::npos) {
        ++gatedIatWalks;
        ++searchFrom;
    }
    EXPECT_EQ(gatedIatWalks, 3);
    EXPECT_EQ(body.find("if (allowIATHooks) {"), std::string::npos) << "an ungated IAT walk would run every second";
    const std::string beforeRefresh = body.substr(0, refresh);
    EXPECT_NE(beforeRefresh.rfind("if (runSweeps) {"), std::string::npos);
}

TEST(FFXModuleRescanSource, SweepBookkeepingSamplesBeforeAndCommitsOnlyCompletedSweeps) {
    const std::string sweep = ReadSource("hook/apis/ffx_hook_module_sweep.cpp");
    ASSERT_FALSE(sweep.empty());
    const std::string decide = Between(sweep, "FfxModuleSweep ffx_hook_DecideModuleSweep(", "\n}\n");
    const std::string complete = Between(sweep, "void ffx_hook_CompleteModuleSweep(", "\n}\n");
    ASSERT_FALSE(decide.empty());
    ASSERT_FALSE(complete.empty());
    EXPECT_NE(decide.find("module_address_cache::ModuleSetGeneration()"), std::string::npos);
    EXPECT_NE(decide.find("ffx_hook_g_UnroutedCallEvidence.load("), std::string::npos);
    EXPECT_NE(decide.find("ce::ffx_module_rescan::Decide("), std::string::npos);
    EXPECT_EQ(decide.find("Commit("), std::string::npos);
    const size_t skipped = complete.find("if (!sweep.Run())");
    const size_t commit = complete.find("ce::ffx_module_rescan::Commit(");
    ASSERT_NE(skipped, std::string::npos);
    ASSERT_NE(commit, std::string::npos);
    EXPECT_LT(skipped, commit) << "a skipped pass must not advance the committed inputs";
}

TEST(FFXModuleRescanSource, BothBreakpointPathsRecordUnroutedCallEvidence) {
    const std::string context = ReadSource("hook/apis/ffx_hook_context.cpp");
    const std::string install = ReadSource("hook/apis/ffx_hook_install.cpp");
    const std::string create =
        Between(context, "ffxReturnCode_t Hooked_ffxCreateContext(", "ffxReturnCode_t Hooked_ffxDestroyContext(");
    const std::string configureVeh = Between(install, "LONG WINAPI FfxConfigureBreakpointVEH(", "\n}\n");
    ASSERT_FALSE(create.empty());
    ASSERT_FALSE(configureVeh.empty());
    EXPECT_NE(create.find("ffx_hook_g_UnroutedCallEvidence.fetch_add"), std::string::npos);
    EXPECT_NE(configureVeh.find("ffx_hook_g_UnroutedCallEvidence.fetch_add"), std::string::npos);
}

TEST(FFXModuleRescanSource, CachedSlotScanReadsWithoutALockedWriteBack) {
    const std::string router = ReadSource("hook/apis/ffx_cached_pointer_router.cpp");
    const std::string scan = Between(router, "size_t RouteWritableRange(", "\n}\n");
    ASSERT_FALSE(scan.empty());
    EXPECT_EQ(scan.find("InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(slot), nullptr, nullptr)"),
              std::string::npos);
    // The exchange that actually installs a route stays interlocked.
    EXPECT_NE(scan.find("route.replacement, route.original"), std::string::npos);
}

}  // namespace
