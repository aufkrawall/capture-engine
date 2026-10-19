// The injector never injects into CaptureEngine's own processes (llm-wiki/library/08 Q10). Until the
// runtime could be hosted by a client process this rested on the whitelist alone; a partial entry such as
// "capture" matches captureengine.exe by substring, and nothing recognised a differently named host.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "captureengine/injection/injection_policy.h"
#include "common/platform/runtime_process_identity.h"
#include "tests/source_fragment_reader.h"

namespace ri = ce::runtime_identity;
namespace policy = ce::injection_policy;

TEST(InjectionRuntimeExclusionTest, RefusesTheInjectorTheHostAndItsHelpersOnly) {
    EXPECT_STREQ(policy::RuntimeTargetRefusalReason(true, ri::Relation::Unrelated), "the injecting process itself");
    EXPECT_STREQ(policy::RuntimeTargetRefusalReason(false, ri::Relation::Host),
                 "the process hosting this CaptureEngine runtime");
    EXPECT_STREQ(policy::RuntimeTargetRefusalReason(false, ri::Relation::Helper),
                 "a helper process of this CaptureEngine runtime");
    EXPECT_EQ(policy::RuntimeTargetRefusalReason(false, ri::Relation::Unrelated), nullptr)
        << "everything else, a game the runtime launched included, stays a legitimate candidate";
}

// InjectionManager is product-only (not linked into the unit tests), so the wiring is anchored in its
// source: discovery and both injectors ask before any remote load.
TEST(InjectionRuntimeExclusionTest, EveryPathToARemoteLoadAsksFirst) {
    const std::filesystem::path injection = std::filesystem::current_path() / "captureengine" / "injection";
    const std::string inject = ce::test_source::ReadFile(injection / "injection_inject.cpp");
    const std::string manager = ce::test_source::ReadFile(injection / "injection_manager.cpp");
    const std::string exclusion = ce::test_source::ReadFile(injection / "injection_runtime_exclusion.cpp");
    ASSERT_FALSE(inject.empty());
    ASSERT_FALSE(manager.empty());
    ASSERT_FALSE(exclusion.empty());

    const size_t injectImpl = inject.find("bool InjectionManager::InjectImpl(");
    ASSERT_NE(injectImpl, std::string::npos);
    const size_t implAsk = inject.find("RefuseRuntimeProcess(pid, processName", injectImpl);
    ASSERT_NE(implAsk, std::string::npos);
    EXPECT_LT(implAsk, inject.find("OpenProcess(", injectImpl));
    EXPECT_LT(implAsk, inject.find("WaitForCreator(", injectImpl));

    const size_t injectEarly = inject.find("bool InjectionManager::InjectEarly(");
    ASSERT_NE(injectEarly, std::string::npos);
    const size_t earlyAsk = inject.find("RefuseRuntimeProcess(pid,", injectEarly);
    ASSERT_NE(earlyAsk, std::string::npos);
    EXPECT_LT(earlyAsk, inject.find("OpenProcess(", injectEarly));

    const size_t launch = manager.find("void InjectionManager::LaunchDelayedInjectionThread(");
    ASSERT_NE(launch, std::string::npos);
    const size_t launchAsk = manager.find("RefuseRuntimeProcess(pid, name", launch);
    ASSERT_NE(launchAsk, std::string::npos);
    EXPECT_LT(launchAsk, manager.find("delayedInjectionPids.insert(pid)", launch));

    // The decision is by process identity, never by an executable name.
    EXPECT_NE(exclusion.find("ClassifyAgainstRegisteredHost(pid)"), std::string::npos);
    EXPECT_NE(exclusion.find("GetCurrentProcessId()"), std::string::npos);
    EXPECT_EQ(exclusion.find("captureengine.exe\""), std::string::npos);
}
