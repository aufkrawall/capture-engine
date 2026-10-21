#include <gtest/gtest.h>

#include "common/config/config.h"
#include "include/cengine/cengine.h"
#include "runtime/core/helper_transport.h"

namespace {
using ce::engine::ChooseHelperTransports;
using ce::engine::DescribeHelperTransports;
using ce::engine::HelperTransportChoice;
using ce::engine::IsolationSource;
using ce::runtime::HelperTransport;
using ce::runtime::RoleEntries;

constexpr HelperIsolation kAuto = HelperIsolation::kAuto;
constexpr HelperIsolation kOn = HelperIsolation::kOn;
constexpr HelperIsolation kOff = HelperIsolation::kOff;

// A function pointer that is not null: the production table is empty, so the tests need a non-null entry to exercise
// the thread transport. Never called: the selection only looks at the pointer.
int FakeEntry(ce::role::RoleContext&, const AppConfig&) {
    return 0;
}

RoleEntries AllThreadCapable() {
    RoleEntries entries;
    entries.inject = FakeEntry;
    entries.media = FakeEntry;
    entries.logger = FakeEntry;
    return entries;
}

TEST(HelperTransportTest, WithoutEntriesEveryRoleIsAProcess) {
    const HelperTransportChoice choice = ChooseHelperTransports(0, kAuto, RoleEntries{});
    EXPECT_EQ(choice.transports.inject, HelperTransport::Process);
    EXPECT_EQ(choice.transports.media, HelperTransport::Process);
    EXPECT_EQ(choice.transports.logger, HelperTransport::Process);
    EXPECT_FALSE(choice.isolated);
    EXPECT_EQ(choice.source, IsolationSource::Default);
}

TEST(HelperTransportTest, AnInProcessRuntimeRunsEachThreadCapableRoleOnAThread) {
    const HelperTransportChoice choice = ChooseHelperTransports(0, kAuto, AllThreadCapable());
    EXPECT_EQ(choice.transports.inject, HelperTransport::Thread);
    EXPECT_EQ(choice.transports.media, HelperTransport::Thread);
    EXPECT_EQ(choice.transports.logger, HelperTransport::Thread);
    EXPECT_FALSE(choice.isolated);
    EXPECT_EQ(choice.source, IsolationSource::Default);
}

TEST(HelperTransportTest, ANullEntryKeepsItsRoleAProcessWhileTheOthersRunOnThreads) {
    RoleEntries entries;
    entries.inject = FakeEntry;
    const HelperTransportChoice choice = ChooseHelperTransports(0, kAuto, entries);
    EXPECT_EQ(choice.transports.inject, HelperTransport::Thread);
    EXPECT_EQ(choice.transports.media, HelperTransport::Process);
    EXPECT_EQ(choice.transports.logger, HelperTransport::Process);
}

TEST(HelperTransportTest, TheIsolationFeatureMakesEveryRoleAProcess) {
    const HelperTransportChoice choice = ChooseHelperTransports(CE_FEATURE_ISOLATED_HELPERS, kAuto, AllThreadCapable());
    EXPECT_EQ(choice.transports.inject, HelperTransport::Process);
    EXPECT_EQ(choice.transports.media, HelperTransport::Process);
    EXPECT_EQ(choice.transports.logger, HelperTransport::Process);
    EXPECT_TRUE(choice.isolated);
    EXPECT_EQ(choice.source, IsolationSource::Feature);
}

TEST(HelperTransportTest, TheIsolationSettingMakesEveryRoleAProcess) {
    const HelperTransportChoice choice = ChooseHelperTransports(0, kOn, AllThreadCapable());
    EXPECT_EQ(choice.transports.inject, HelperTransport::Process);
    EXPECT_EQ(choice.transports.media, HelperTransport::Process);
    EXPECT_EQ(choice.transports.logger, HelperTransport::Process);
    EXPECT_TRUE(choice.isolated);
    EXPECT_EQ(choice.source, IsolationSource::Setting);
}

TEST(HelperTransportTest, AnExplicitSettingIsTheSourceEvenWhenTheFeatureAgrees) {
    const HelperTransportChoice choice =
        ChooseHelperTransports(CE_FEATURE_ISOLATED_HELPERS, kOn, AllThreadCapable());
    EXPECT_TRUE(choice.isolated);
    EXPECT_EQ(choice.source, IsolationSource::Setting);
}

// DR-49: the app sets the feature; isolated_helpers=off runs it in-process for testing.
TEST(HelperTransportTest, TheOffSettingOverridesTheIsolationFeature) {
    const HelperTransportChoice choice =
        ChooseHelperTransports(CE_FEATURE_ISOLATED_HELPERS, kOff, AllThreadCapable());
    EXPECT_FALSE(choice.isolated);
    EXPECT_EQ(choice.source, IsolationSource::Setting);
    EXPECT_EQ(choice.transports.inject, HelperTransport::Thread);
    EXPECT_EQ(choice.transports.media, HelperTransport::Thread);
    EXPECT_EQ(choice.transports.logger, HelperTransport::Thread);
}

TEST(HelperTransportTest, TheOffSettingKeepsAnSdkClientOnThreads) {
    const HelperTransportChoice choice = ChooseHelperTransports(0, kOff, AllThreadCapable());
    EXPECT_FALSE(choice.isolated);
    EXPECT_EQ(choice.source, IsolationSource::Setting);
    EXPECT_EQ(choice.transports.media, HelperTransport::Thread);
}

TEST(HelperTransportTest, TheOffSettingCannotGiveARoleWithoutAThreadEntryAThread) {
    const HelperTransportChoice choice = ChooseHelperTransports(0, kOff, RoleEntries{});
    EXPECT_FALSE(choice.isolated);
    EXPECT_EQ(choice.transports.inject, HelperTransport::Process);
    EXPECT_EQ(choice.transports.media, HelperTransport::Process);
    EXPECT_EQ(choice.transports.logger, HelperTransport::Process);
}

TEST(HelperTransportTest, ProcessTuningDoesNotIsolate) {
    const HelperTransportChoice choice = ChooseHelperTransports(CE_FEATURE_PROCESS_TUNING, kAuto, AllThreadCapable());
    EXPECT_FALSE(choice.isolated);
    EXPECT_EQ(choice.transports.inject, HelperTransport::Thread);
    EXPECT_EQ(choice.transports.media, HelperTransport::Thread);
    EXPECT_EQ(choice.transports.logger, HelperTransport::Thread);
}

TEST(HelperTransportTest, TheCreateLineNamesEveryTransportAndTheSource) {
    EXPECT_EQ(DescribeHelperTransports(ChooseHelperTransports(0, kAuto, AllThreadCapable())),
              "[Runtime] helper transports: inject=thread media=thread logger=thread sensors=process isolated=0 "
              "(default)");
    EXPECT_EQ(DescribeHelperTransports(ChooseHelperTransports(CE_FEATURE_ISOLATED_HELPERS, kAuto, AllThreadCapable())),
              "[Runtime] helper transports: inject=process media=process logger=process sensors=process isolated=1 "
              "(feature)");
    EXPECT_EQ(DescribeHelperTransports(ChooseHelperTransports(0, kOn, RoleEntries{})),
              "[Runtime] helper transports: inject=process media=process logger=process sensors=process isolated=1 "
              "(setting)");
}

}  // namespace
