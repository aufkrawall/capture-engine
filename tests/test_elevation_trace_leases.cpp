#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "common/ipc/elevation_protocol.h"
#include "common/ipc/elevation_trace_lease.h"
#include "source_fragment_reader.h"

using ce::elevation::TraceLeaseCounter;

// Regression: process discovery leased the service's display trace, so for as
// long as CaptureEngine ran the DXGI present, DxgKrnl packet/flip/vsync and
// Win32k input providers were on for every process on the machine, game or
// not. Process discovery now leases the session for the process provider alone;
// display providers follow the display leases.
TEST(ElevationTraceLeases, ProcessLeaseAloneNeverEnablesDisplayProviders) {
    TraceLeaseCounter leases;
    const auto process = leases.PlanAcquire(/*display=*/false);
    EXPECT_TRUE(process.startSession);
    EXPECT_FALSE(process.enableDisplay);
    leases.CommitAcquire(false);

    const auto display = leases.PlanAcquire(true);
    EXPECT_FALSE(display.startSession) << "the session is already open";
    EXPECT_TRUE(display.enableDisplay) << "the first display lease turns the display half on";
    leases.CommitAcquire(true);

    const auto secondDisplay = leases.PlanAcquire(true);
    EXPECT_FALSE(secondDisplay.startSession);
    EXPECT_FALSE(secondDisplay.enableDisplay);
    leases.CommitAcquire(true);

    auto release = leases.Release(true);
    EXPECT_FALSE(release.disableDisplay) << "one display lease is still held";
    EXPECT_FALSE(release.stopSession);
    release = leases.Release(true);
    EXPECT_TRUE(release.disableDisplay) << "the last display lease turns the display half off";
    EXPECT_FALSE(release.stopSession) << "process discovery still holds the session";
    release = leases.Release(false);
    EXPECT_FALSE(release.disableDisplay);
    EXPECT_TRUE(release.stopSession);
    EXPECT_EQ(leases.users(), 0u);
}

TEST(ElevationTraceLeases, LastLeaseStopsTheSessionWithoutASeparateDisable) {
    TraceLeaseCounter leases;
    EXPECT_TRUE(leases.PlanAcquire(true).startSession);
    EXPECT_TRUE(leases.PlanAcquire(true).enableDisplay);
    leases.CommitAcquire(true);
    leases.CommitAcquire(false);

    auto release = leases.Release(false);
    EXPECT_FALSE(release.disableDisplay) << "the display lease keeps its providers";
    EXPECT_FALSE(release.stopSession);
    release = leases.Release(true);
    EXPECT_FALSE(release.disableDisplay) << "stopping the session drops every provider anyway";
    EXPECT_TRUE(release.stopSession);
}

TEST(ElevationTraceLeases, UnmatchedReleasesChangeNothing) {
    TraceLeaseCounter leases;
    auto release = leases.Release(true);
    EXPECT_FALSE(release.disableDisplay || release.stopSession);
    release = leases.Release(false);
    EXPECT_FALSE(release.disableDisplay || release.stopSession);

    leases.CommitAcquire(true);
    release = leases.Release(false);
    EXPECT_FALSE(release.disableDisplay || release.stopSession) << "no process lease was held";
    EXPECT_EQ(leases.users(), 1u);
    EXPECT_EQ(leases.displayUsers(), 1u);

    leases.CommitAcquire(false);
    leases.Reset();
    EXPECT_EQ(leases.users(), 0u);
    EXPECT_EQ(leases.displayUsers(), 0u);
    EXPECT_TRUE(leases.PlanAcquire(false).startSession);
}

TEST(ElevationTraceLeases, ProcessLeaseOperationsAreAppendedAndAdvertised) {
    using namespace ce::elevation;
    Header header;
    header.sequence = 1;
    for (auto operation : {Operation::AcquireProcessTrace, Operation::ReleaseProcessTrace}) {
        header.operation = operation;
        header.size = 0;
        EXPECT_TRUE(ValidateRequest(header, nullptr, 0));
    }
    // Wire values of the existing operations are unchanged for older peers.
    EXPECT_EQ(static_cast<uint32_t>(Operation::Status), 6u);
    EXPECT_EQ(static_cast<uint32_t>(Operation::AcquireProcessTrace), 7u);
    EXPECT_EQ(static_cast<uint32_t>(Operation::ReleaseProcessTrace), 8u);
    EXPECT_EQ(static_cast<uint32_t>(Operation::RestoreConfig), 9u);
    header.operation = static_cast<Operation>(10);
    EXPECT_FALSE(ValidateHeader(header, false));

    const Capabilities capabilities;
    EXPECT_NE(capabilities.features & kProcessOnlyTraceCapability, 0u) << "a current service advertises it";
    EXPECT_EQ(kProcessOnlyTraceCapability & (kSensorCapability | kDisplayTraceCapability | kProcessStartTraceCapability),
              0u);
}

TEST(ElevationTraceLeases, ServiceAndClientUseTheSplitProviderGroups) {
    const auto root = std::filesystem::current_path();
    const std::string service = ce::test_source::ReadFile(root / "elevationservice" / "service_trace.cpp");
    const std::string serve = ce::test_source::ReadFile(root / "elevationservice" / "service_main.cpp");
    const std::string client = ce::test_source::ReadFile(root / "captureengine" / "elevation" / "elevation_client.cpp");
    ASSERT_FALSE(service.empty());
    ASSERT_FALSE(serve.empty());
    ASSERT_FALSE(client.empty());

    const size_t start = service.find("DWORD TraceManager::StartSessionLocked() {");
    const size_t display = service.find("DWORD TraceManager::EnableDisplayLocked() {");
    ASSERT_NE(start, std::string::npos);
    ASSERT_NE(display, std::string::npos);
    const std::string startBody = service.substr(start, display - start);
    EXPECT_NE(startBody.find("ProviderGroup::ProcessStart"), std::string::npos);
    EXPECT_EQ(startBody.find("ProviderGroup::Display"), std::string::npos)
        << "opening the session must not trace display activity";
    EXPECT_NE(service.find("ProviderGroup::Display", display), std::string::npos);
    EXPECT_NE(service.find("DisableDisplayTimingProviders(session_)"), std::string::npos);

    EXPECT_NE(serve.find("traces.Acquire(TraceKind::ProcessStart)"), std::string::npos);
    EXPECT_NE(serve.find("traces.Acquire(TraceKind::Display)"), std::string::npos);

    // Only a service that advertises the operation is sent it: an older one
    // drops the connection on an unknown operation.
    const size_t gate = client.find("(features_ & kProcessOnlyTraceCapability) != 0");
    const size_t send = client.find("Exchange(Operation::AcquireProcessTrace");
    ASSERT_NE(gate, std::string::npos);
    ASSERT_NE(send, std::string::npos);
    EXPECT_LT(gate, send);
}
