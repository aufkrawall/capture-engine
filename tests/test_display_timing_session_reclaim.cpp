#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../captureengine/display_timing_session_reclaim.h"

#include "source_fragment_reader.h"

namespace {

using ce::display_timing_reclaim::ParseSessionOwnerPid;
using ce::display_timing_reclaim::ShouldReclaimAfterStartFailure;
using ce::display_timing_reclaim::ShouldStopSession;

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

TEST(DisplayTimingSessionReclaimTest, ParsesTheOwnerPidTheServiceWrites) {
    // Exactly the format swprintf(L"CE_DisplayTiming_%08X", pid) produces.
    EXPECT_EQ(ParseSessionOwnerPid(L"CE_DisplayTiming_00005840"), 0x5840u);
    EXPECT_EQ(ParseSessionOwnerPid(L"CE_DisplayTiming_000012C8"), 0x12C8u);
    EXPECT_EQ(ParseSessionOwnerPid(L"CE_DisplayTiming_FFFFFFFF"), 0xFFFFFFFFu);
}

// Stopping a stranger's trace session would be a serious bug, so anything that is
// not exactly our name is refused rather than guessed at.
TEST(DisplayTimingSessionReclaimTest, RefusesEverySessionNameThatIsNotOurs) {
    EXPECT_EQ(ParseSessionOwnerPid(nullptr), 0u);
    EXPECT_EQ(ParseSessionOwnerPid(L""), 0u);
    EXPECT_EQ(ParseSessionOwnerPid(L"Eventlog-Security"), 0u);
    EXPECT_EQ(ParseSessionOwnerPid(L"CE_DisplayTiming_"), 0u);           // no pid
    EXPECT_EQ(ParseSessionOwnerPid(L"CE_DisplayTiming_1234"), 0u);       // not eight digits
    EXPECT_EQ(ParseSessionOwnerPid(L"CE_DisplayTiming_000012c8"), 0u);   // lowercase is not our format
    EXPECT_EQ(ParseSessionOwnerPid(L"CE_DisplayTiming_0000ZZZZ"), 0u);   // not hexadecimal
    EXPECT_EQ(ParseSessionOwnerPid(L"CE_DisplayTiming_000012C80"), 0u);  // longer than the format writes
    EXPECT_EQ(ParseSessionOwnerPid(L"CE_DisplayTiming_00000000"), 0u);   // pid 0 is not a process
    EXPECT_EQ(ParseSessionOwnerPid(L"XCE_DisplayTiming_000012C8"), 0u);  // prefix must start the name
}

TEST(DisplayTimingSessionReclaimTest, ReclaimsOnlyAfterTheTwoLeakedSessionStatuses) {
    EXPECT_TRUE(ShouldReclaimAfterStartFailure(1450));  // ERROR_NO_SYSTEM_RESOURCES
    EXPECT_TRUE(ShouldReclaimAfterStartFailure(183));   // ERROR_ALREADY_EXISTS
    EXPECT_FALSE(ShouldReclaimAfterStartFailure(0));
    EXPECT_FALSE(ShouldReclaimAfterStartFailure(5));    // ERROR_ACCESS_DENIED
    EXPECT_FALSE(ShouldReclaimAfterStartFailure(87));   // ERROR_INVALID_PARAMETER
}

TEST(DisplayTimingSessionReclaimTest, StopsOnlyOurSessionsWhoseOwnerIsGone) {
    constexpr uint32_t kCurrent = 0x1234;
    // Owner dead: ours to take back.
    EXPECT_TRUE(ShouldStopSession(0x5840, kCurrent, /*ownerProcessAlive=*/false));
    // Our own process id from a previous life (pid reuse after a kill).
    EXPECT_TRUE(ShouldStopSession(kCurrent, kCurrent, /*ownerProcessAlive=*/true));
    // Another CaptureEngine that is still running is using its session.
    EXPECT_FALSE(ShouldStopSession(0x5840, kCurrent, /*ownerProcessAlive=*/true));
    // Not one of ours at all.
    EXPECT_FALSE(ShouldStopSession(0, kCurrent, /*ownerProcessAlive=*/false));
}

// The recovery is worth nothing if the service does not attempt it, and the failure
// message is what tells a user why their frame-time graph changed shape.
TEST(DisplayTimingSessionReclaimTest, TheServiceReclaimsAndThenRetriesTheSession) {
    const std::string service = ReadSource("captureengine/display_timing_service.cpp");
    ASSERT_FALSE(service.empty());
    // The service reaches ETW only through the startup unit, which is the only
    // caller of the reclaiming open below.
    EXPECT_NE(service.find("OpenSessionAndEnableProviders(&session_, sessionName_)"), std::string::npos);
    EXPECT_EQ(service.find("StartTraceW("), std::string::npos);

    const std::string startup = ReadSource("captureengine/display_timing_startup.cpp");
    ASSERT_FALSE(startup.empty());
    // The session is opened only through the reclaiming path, never StartTraceW
    // directly, or a leak would still be able to accumulate.
    EXPECT_NE(startup.find("OpenSessionWithReclaim(session, sessionName)"), std::string::npos);
    EXPECT_EQ(startup.find("StartTraceW("), std::string::npos);
    // The exhausted-budget case says so instead of reporting a bare error number: the
    // symptom a user sees is a changed frame-time graph, not an error.
    EXPECT_NE(startup.find("ETW session budget is"), std::string::npos);

    const std::string reclaim = ReadSource("captureengine/display_timing_session_reclaim.cpp");
    ASSERT_FALSE(reclaim.empty());

    // The sweep runs BEFORE the session is opened, so a leak cannot accumulate at all,
    // and again after a refusal so a session freed in between is still picked up.
    const size_t sweep = reclaim.find("ReclaimLeakedSessions(0)");
    const size_t firstStart = reclaim.find("StartTraceW(sessionHandle, sessionName, properties.Get())", sweep);
    const size_t reclaimGate = reclaim.find("ShouldReclaimAfterStartFailure(status)", firstStart);
    const size_t reclaimCall = reclaim.find("ReclaimLeakedSessions(status)", reclaimGate);
    const size_t retry = reclaim.find("StartTraceW(sessionHandle, sessionName, retryProperties.Get())", reclaimCall);
    ASSERT_NE(sweep, std::string::npos);
    ASSERT_NE(firstStart, std::string::npos);
    ASSERT_NE(reclaimGate, std::string::npos);
    ASSERT_NE(reclaimCall, std::string::npos);
    ASSERT_NE(retry, std::string::npos);
    EXPECT_LT(sweep, firstStart);
    EXPECT_LT(firstStart, reclaimGate);
    EXPECT_LT(reclaimGate, reclaimCall);
    EXPECT_LT(reclaimCall, retry);
    // A live owner is consulted before anything is stopped, and only via the policy.
    EXPECT_NE(reclaim.find("PROCESS_QUERY_LIMITED_INFORMATION"), std::string::npos);
    EXPECT_NE(reclaim.find("ShouldStopSession(ownerPid, currentPid, ownerAlive)"), std::string::npos);
    EXPECT_NE(reclaim.find("ParseSessionOwnerPid(name)"), std::string::npos);
    EXPECT_NE(reclaim.find("EVENT_TRACE_CONTROL_STOP"), std::string::npos);
}

}  // namespace
