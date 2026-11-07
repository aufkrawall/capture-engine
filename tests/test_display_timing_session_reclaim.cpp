#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "captureengine/display_timing/display_timing_session_reclaim.h"
#include "captureengine/display_timing/display_timing_session_reclaim_environment.h"

#include "source_fragment_reader.h"

namespace {

using ce::display_timing_reclaim::ParseSessionOwnerPid;
using ce::display_timing_reclaim::ShouldReclaimAfterStartFailure;
using ce::display_timing_reclaim::ShouldStopSession;

struct FakeReclaim {
    std::vector<std::wstring> names;
    std::vector<std::wstring> stopped;
    ULONG queryStatus = ERROR_SUCCESS;
    ULONG reportedCount = 0;
    DWORD openError = ERROR_SUCCESS;
    DWORD exitQueryError = ERROR_SUCCESS;
    DWORD exitCode = 0;
    int opened = 0;
    int closed = 0;

    FakeReclaim() {
        wchar_t name[64];
        const unsigned int otherPid = GetCurrentProcessId() == 0x5840 ? 0x5844 : 0x5840;
        swprintf_s(name, L"CE_PresentProgress_%08X", otherPid);
        names.emplace_back(name);
    }

    ce::display_timing_reclaim::ReclaimEnvironment Environment() {
        return {
            [this](EVENT_TRACE_PROPERTIES** sessions, ULONG capacity, ULONG* count) {
                for (size_t i = 0; i < names.size() && i < capacity; ++i) {
                    auto* properties = sessions[i];
                    auto* name = reinterpret_cast<wchar_t*>(reinterpret_cast<std::byte*>(properties) +
                                                            properties->LoggerNameOffset);
                    const size_t chars = (properties->Wnode.BufferSize - properties->LoggerNameOffset) / sizeof(wchar_t);
                    wcscpy_s(name, chars, names[i].c_str());
                }
                *count = reportedCount ? reportedCount : static_cast<ULONG>(names.size());
                return queryStatus;
            },
            [this](uint32_t) -> HANDLE {
                ++opened;
                SetLastError(openError);
                return openError == ERROR_SUCCESS ? reinterpret_cast<HANDLE>(uintptr_t{1}) : nullptr;
            },
            [this](HANDLE, DWORD* code) -> BOOL {
                *code = exitCode;
                SetLastError(exitQueryError);
                return exitQueryError == ERROR_SUCCESS;
            },
            [this](HANDLE) { ++closed; },
            [this](const wchar_t* name) -> ULONG {
                stopped.emplace_back(name);
                return ERROR_SUCCESS;
            }};
    }
};

TEST(DisplayTimingSessionReclaimTest, PartialEnumerationUsesOnlyThePopulatedArray) {
    FakeReclaim fake;
    const std::wstring name = fake.names.front();
    fake.names.assign(64, name);
    fake.reportedCount = 81;
    fake.queryStatus = ERROR_MORE_DATA;
    EXPECT_EQ(ce::display_timing_reclaim::ReclaimLeakedSessions(1450, nullptr, fake.Environment()), 64);
    EXPECT_EQ(fake.opened, 64);
    EXPECT_EQ(fake.closed, 64);
    EXPECT_EQ(fake.stopped.size(), 64u);
}

TEST(DisplayTimingSessionReclaimTest, InaccessibleOwnerIsKeptButAnAbsentPidIsReclaimed) {
    for (DWORD error : {DWORD{ERROR_ACCESS_DENIED}, DWORD{ERROR_NOT_ENOUGH_MEMORY}, DWORD{ERROR_INVALID_PARAMETER}}) {
        FakeReclaim fake;
        fake.openError = error;
        const int expected = error == ERROR_INVALID_PARAMETER ? 1 : 0;
        EXPECT_EQ(ce::display_timing_reclaim::ReclaimLeakedSessions(0, nullptr, fake.Environment()), expected);
        EXPECT_EQ(fake.stopped.size(), static_cast<size_t>(expected));
        EXPECT_EQ(fake.closed, 0);
    }
}

TEST(DisplayTimingSessionReclaimTest, ExitQueryFailureAndLiveOwnerAreKeptWhileExitedOwnerIsReclaimed) {
    FakeReclaim fake;
    fake.exitQueryError = ERROR_ACCESS_DENIED;
    EXPECT_EQ(ce::display_timing_reclaim::ReclaimLeakedSessions(0, nullptr, fake.Environment()), 0);
    fake.exitQueryError = ERROR_SUCCESS;
    fake.exitCode = STILL_ACTIVE;
    EXPECT_EQ(ce::display_timing_reclaim::ReclaimLeakedSessions(0, nullptr, fake.Environment()), 0);
    fake.exitCode = 1;
    EXPECT_EQ(ce::display_timing_reclaim::ReclaimLeakedSessions(0, nullptr, fake.Environment()), 1);
    EXPECT_EQ(fake.opened, 3);
    EXPECT_EQ(fake.closed, 3);
    EXPECT_EQ(fake.stopped, fake.names);
}

TEST(DisplayTimingSessionReclaimTest, OwnPidReclaimsOnlyTheOpeningNameAndNeverASiblingOrForeignSession) {
    FakeReclaim fake;
    wchar_t openingName[64];
    wchar_t siblingName[64];
    swprintf_s(openingName, L"CE_PresentProgress_%08X", static_cast<unsigned int>(GetCurrentProcessId()));
    swprintf_s(siblingName, L"CE_DisplayTiming_%08X", static_cast<unsigned int>(GetCurrentProcessId()));
    fake.names = {siblingName, L"UnrelatedSession", openingName};
    EXPECT_EQ(ce::display_timing_reclaim::ReclaimLeakedSessions(0, openingName, fake.Environment()), 1);
    EXPECT_EQ(fake.opened, 0);
    EXPECT_EQ(fake.stopped, (std::vector<std::wstring>{openingName}));
}

TEST(DisplayTimingSessionReclaimTest, FailedEnumerationNeverInspectsOrStopsSessions) {
    FakeReclaim fake;
    fake.queryStatus = ERROR_ACCESS_DENIED;
    fake.reportedCount = 81;
    EXPECT_EQ(ce::display_timing_reclaim::ReclaimLeakedSessions(0, nullptr, fake.Environment()), 0);
    EXPECT_EQ(fake.opened, 0);
    EXPECT_TRUE(fake.stopped.empty());
}

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

TEST(DisplayTimingSessionReclaimTest, MalformedNameOffsetsDoNotOpenOrStopAnOwner) {
    FakeReclaim fake;
    auto env = fake.Environment();
    auto query = env.query;
    for (const ULONG offset : {0u, static_cast<unsigned int>(sizeof(EVENT_TRACE_PROPERTIES)) + 1u, 0xfffffff0u}) {
        env.query = [query, offset](EVENT_TRACE_PROPERTIES** sessions, ULONG capacity, ULONG* count) {
            const ULONG status = query(sessions, capacity, count);
            sessions[0]->LoggerNameOffset = offset;
            return status;
        };
        EXPECT_EQ(ce::display_timing_reclaim::ReclaimLeakedSessions(0, nullptr, env), 0);
    }
    EXPECT_EQ(fake.opened, 0);
    EXPECT_TRUE(fake.stopped.empty());
}

TEST(DisplayTimingSessionReclaimTest, ParsesTheOwnerPidTheServiceWrites) {
    // Exactly the format swprintf(L"CE_DisplayTiming_%08X", pid) produces.
    EXPECT_EQ(ParseSessionOwnerPid(L"CE_DisplayTiming_00005840"), 0x5840u);
    EXPECT_EQ(ParseSessionOwnerPid(L"CE_DisplayTiming_000012C8"), 0x12C8u);
    EXPECT_EQ(ParseSessionOwnerPid(L"CE_DisplayTiming_FFFFFFFF"), 0xFFFFFFFFu);
    // Also the format swprintf(L"CE_PresentProgress_%08X", pid) produces.
    EXPECT_EQ(ParseSessionOwnerPid(L"CE_PresentProgress_00005840"), 0x5840u);
    EXPECT_EQ(ParseSessionOwnerPid(L"CE_PresentProgress_000012C8"), 0x12C8u);
    EXPECT_EQ(ParseSessionOwnerPid(L"CE_PresentProgress_FFFFFFFF"), 0xFFFFFFFFu);
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

    EXPECT_EQ(ParseSessionOwnerPid(L"CE_PresentProgress_"), 0u);
    EXPECT_EQ(ParseSessionOwnerPid(L"CE_PresentProgress_1234"), 0u);
    EXPECT_EQ(ParseSessionOwnerPid(L"CE_PresentProgress_000012c8"), 0u);
    EXPECT_EQ(ParseSessionOwnerPid(L"CE_PresentProgress_0000ZZZZ"), 0u);
    EXPECT_EQ(ParseSessionOwnerPid(L"CE_PresentProgress_000012C80"), 0u);
    EXPECT_EQ(ParseSessionOwnerPid(L"CE_PresentProgress_00000000"), 0u);
    EXPECT_EQ(ParseSessionOwnerPid(L"XCE_PresentProgress_000012C8"), 0u);
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
    // Own pid + being opened = true (pid reuse after a kill of the exact same session name).
    EXPECT_TRUE(ShouldStopSession(kCurrent, kCurrent, /*ownerAlive=*/true, /*isTheSessionBeingOpened=*/true));
    // Own pid + other name = false (regression test for the trap: another name under our pid is a live sibling).
    EXPECT_FALSE(ShouldStopSession(kCurrent, kCurrent, /*ownerAlive=*/true, /*isTheSessionBeingOpened=*/false));
    // Dead owner = true (ours to take back).
    EXPECT_TRUE(ShouldStopSession(0x5840, kCurrent, /*ownerAlive=*/false, /*isTheSessionBeingOpened=*/false));
    EXPECT_TRUE(ShouldStopSession(0x5840, kCurrent, /*ownerAlive=*/false, /*isTheSessionBeingOpened=*/true));
    // Live owner (different pid) = false (another CaptureEngine is using its session).
    EXPECT_FALSE(ShouldStopSession(0x5840, kCurrent, /*ownerAlive=*/true, /*isTheSessionBeingOpened=*/false));
    EXPECT_FALSE(ShouldStopSession(0x5840, kCurrent, /*ownerAlive=*/true, /*isTheSessionBeingOpened=*/true));
    // Owner 0 = false (not one of ours).
    EXPECT_FALSE(ShouldStopSession(0, kCurrent, /*ownerAlive=*/false, /*isTheSessionBeingOpened=*/false));
    EXPECT_FALSE(ShouldStopSession(0, kCurrent, /*ownerAlive=*/false, /*isTheSessionBeingOpened=*/true));
}

// The recovery is worth nothing if the service does not attempt it, and the failure
// message is what tells a user why their frame-time graph changed shape.
TEST(DisplayTimingSessionReclaimTest, TheServiceReclaimsAndThenRetriesTheSession) {
    const std::string service = ReadSource("captureengine/display_timing/display_timing_service.cpp");
    ASSERT_FALSE(service.empty());
    // The service reaches ETW only through the startup unit, which is the only
    // caller of the reclaiming open below.
    EXPECT_NE(service.find("OpenSessionAndEnableProviders(&session_, sessionName_)"), std::string::npos);
    EXPECT_EQ(service.find("StartTraceW("), std::string::npos);

    const std::string startup = ReadSource("captureengine/display_timing/display_timing_startup.cpp");
    ASSERT_FALSE(startup.empty());
    // The session is opened only through the reclaiming path, never StartTraceW
    // directly, or a leak would still be able to accumulate.
    EXPECT_NE(startup.find("OpenSessionWithReclaim(session, sessionName)"), std::string::npos);
    EXPECT_EQ(startup.find("StartTraceW("), std::string::npos);
    // The exhausted-budget case says so instead of reporting a bare error number: the
    // symptom a user sees is a changed frame-time graph, not an error.
    EXPECT_NE(startup.find("ETW session budget is"), std::string::npos);

    const std::string reclaim = ReadSource("captureengine/display_timing/display_timing_session_reclaim.cpp");
    ASSERT_FALSE(reclaim.empty());

    // The sweep runs BEFORE the session is opened, so a leak cannot accumulate at all,
    // and again after a refusal so a session freed in between is still picked up.
    const size_t sweep = reclaim.find("ReclaimLeakedSessions(0, sessionName)");
    const size_t firstStart = reclaim.find("StartTraceW(sessionHandle, sessionName, properties.Get())", sweep);
    const size_t reclaimGate = reclaim.find("ShouldReclaimAfterStartFailure(status)", firstStart);
    const size_t reclaimCall = reclaim.find("ReclaimLeakedSessions(status, sessionName)", reclaimGate);
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
    EXPECT_NE(reclaim.find("ShouldStopSession(ownerPid, currentPid, ownerAlive, isTheSessionBeingOpened)"), std::string::npos);
    EXPECT_NE(reclaim.find("ParseSessionOwnerPid(name)"), std::string::npos);
    EXPECT_NE(reclaim.find("EVENT_TRACE_CONTROL_STOP"), std::string::npos);
}

}  // namespace
