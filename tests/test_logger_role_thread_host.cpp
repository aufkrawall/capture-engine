#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <utility>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>

#include "captureengine/diagnostics/logger_service.h"
#include "common/config/config.h"
#include "common/ipc/process_ipc.h"
#include "common/ipc/role_context.h"
#include "common/ipc/shared_defs.h"
#include "common/logging/log_ring.h"
#include "runtime/children/host_children.h"

// The real logger role on the supervisor's thread transport (DR-37, DR-38). The production entry drains the
// machine-wide named rings, which a running CaptureEngine may own, so the entry here drains an in-memory ring source
// instead. Every wait is event driven and bounded; nothing sleeps.

namespace fs = std::filesystem;

namespace {
using namespace ce::runtime;
using ce::role::RoleContext;

// A generous bound on a wait for the logger. It only turns a stuck logger into a failure: a healthy one never
// reaches it.
constexpr DWORD kEventBoundMs = 30000;

// What the logger entry saw of its context. Written before the entry signals `g_started`, read after that signal.
struct EntryObservation {
    bool ownsProcess = true;
    bool hasStopEvent = false;
    bool hasLogSink = false;
    uint32_t controllerPid = 0;
};

// RoleEntry is a plain function pointer, so the entry reaches its fixture through globals. Each test sets them
// before it starts the logger; only the logger thread touches them until the test has observed that thread's exit.
LoggerRingSource* g_rings = nullptr;
HANDLE g_started = nullptr;       // signalled by the entry once it runs
HANDLE g_threadHandle = nullptr;  // a duplicate of the logger thread's handle, so the test can wait for its exit
EntryObservation g_observed;
std::atomic<int> g_entries{0};

// The logger role over the test's ring source. The production entry differs only in the source it passes.
int TestLoggerEntry(RoleContext& context, const AppConfig& config) {
    ++g_entries;
    g_observed.ownsProcess = context.ownsProcess;
    g_observed.hasStopEvent = context.stopEvent != nullptr;
    g_observed.hasLogSink = context.logSink != nullptr;
    g_observed.controllerPid = context.controllerPid;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_threadHandle,
                    SYNCHRONIZE | THREAD_QUERY_INFORMATION, FALSE, 0);
    SetEvent(g_started);
    return logger_service::RunLoggerWithRings(context, config, *g_rings);
}

// Rings and a discovery record standing in for the named shared mappings. The logger thread is the only user while it
// runs; the test adds rings and publishes discovery before it starts the logger, and reads the results after it exited.
// Records are pushed while the logger runs, the one producer and one consumer the real ring has.
class FakeRings final : public LoggerRingSource {
public:
    bool ReadDiscovery(LoggerDiscovery& discovery) override {
        if (!discovery_)
            return false;
        discovery = *discovery_;
        return true;
    }

    LoggerRing OpenRing(uint32_t pid) override {
        const auto it = rings_.find(pid);
        if (it == rings_.end())
            return {};
        return LoggerRing{it->second.get(), nullptr};
    }

    void CloseRing(LoggerRing& ring) override {
        if (ring.shm)
            ++closedRings;
        ring = {};
    }

    void Publish(uint32_t pid, const std::string& sessionLogsPath) {
        LoggerDiscovery discovery;
        discovery.injectPid = pid;
        std::snprintf(discovery.logsPath, sizeof(discovery.logsPath), "%s", sessionLogsPath.c_str());
        discovery_ = discovery;
    }

    // A ring that passes the logger's ABI and version check, as the hook's producer initializes it.
    SharedMemoryLayout& AddRing(uint32_t pid) {
        auto ring = std::make_unique<SharedMemoryLayout>();
        ring->SetMagic(SHARED_MEMORY_MAGIC);
        ring->SetVersion(SHARED_MEMORY_VERSION);
        ring->structSize.store(sizeof(SharedMemoryLayout), std::memory_order_release);
        ring->abiSignature.store(SHARED_MEMORY_ABI_SIGNATURE, std::memory_order_release);
        SharedMemoryLayout& reference = *ring;
        rings_[pid] = std::move(ring);
        return reference;
    }

    int closedRings = 0;

private:
    std::optional<LoggerDiscovery> discovery_;
    std::map<uint32_t, std::unique_ptr<SharedMemoryLayout>> rings_;
};

void PushRecord(SharedMemoryLayout& ring, const std::string& filename, const std::string& message) {
    ASSERT_EQ(ce::logging::TryPushLogRecord(ring.logs, filename, message), ce::logging::LogRingPush::Published);
}

// A fresh directory for one test, removed afterwards.
class TempRoot {
public:
    TempRoot() {
        static unsigned long long sequence = 0;
        path_ = fs::temp_directory_path() / ("ce_logger_thread_host_" + std::to_string(GetCurrentProcessId()) + "_" +
                                             std::to_string(GetTickCount64()) + "_" + std::to_string(++sequence));
        fs::create_directories(path_);
    }
    ~TempRoot() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TempRoot(const TempRoot&) = delete;
    TempRoot& operator=(const TempRoot&) = delete;
    const fs::path& path() const {
        return path_;
    }

private:
    fs::path path_;
};

std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::set<std::string> EntryNames(const fs::path& directory) {
    std::set<std::string> names;
    for (const auto& entry : fs::directory_iterator(directory))
        names.insert(entry.path().filename().string());
    return names;
}

// A supervisor whose logger runs TestLoggerEntry on the thread transport.
std::unique_ptr<ChildSupervisor> MakeLoggerSupervisor(const fs::path& sessionDir, const fs::path& logsRoot) {
    ThreadRoleSetup setup;
    setup.entries.logger = TestLoggerEntry;
    setup.transports.logger = HelperTransport::Thread;
    // Inject and media stay on the thread transport with no entry, so no process can be spawned from this supervisor.
    setup.transports.inject = HelperTransport::Thread;
    setup.transports.media = HelperTransport::Thread;
    setup.sessionDir = sessionDir.string();
    setup.logsRoot = logsRoot.string();
    setup.currentConfig = [] { return AppConfig(); };
    return std::make_unique<ChildSupervisor>("fixture.ini", nullptr, nullptr, nullptr, std::move(setup));
}

bool WaitForStart() {
    return WaitForSingleObject(g_started, kEventBoundMs) == WAIT_OBJECT_0;
}

bool WaitForThreadExit() {
    return g_threadHandle && WaitForSingleObject(g_threadHandle, kEventBoundMs) == WAIT_OBJECT_0;
}

DWORD ExitCodeOf(HANDLE thread) {
    DWORD exitCode = STILL_ACTIVE;
    GetExitCodeThread(thread, &exitCode);
    return exitCode;
}

// The logger thread's own log file, named as the logger process names its log.
std::string RoleLogName() {
    return GetProcessLogFileName(ProcessMode::Logger, "", GetCurrentProcessId());
}

class LoggerThreadHostTest : public testing::Test {
protected:
    void SetUp() override {
        g_started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ASSERT_NE(g_started, nullptr);
        g_threadHandle = nullptr;
        g_observed = {};
        g_entries = 0;
    }
    void TearDown() override {
        if (g_threadHandle)
            CloseHandle(g_threadHandle);
        if (g_started)
            CloseHandle(g_started);
        g_threadHandle = nullptr;
        g_started = nullptr;
        g_rings = nullptr;
    }
};
}  // namespace

TEST_F(LoggerThreadHostTest, EnsureStartsTheLoggerThreadWithItsOwnSessionLogFile) {
    TempRoot root;
    const fs::path session = root.path() / "session";
    fs::create_directories(session);
    FakeRings rings;
    g_rings = &rings;
    const auto supervisor = MakeLoggerSupervisor(session, root.path() / "logs");
    ChildSupervisor& children = *supervisor;

    ASSERT_TRUE(children.Ensure(HostChild::Logger));
    EXPECT_TRUE(children.Present(HostChild::Logger));
    EXPECT_TRUE(children.Running(HostChild::Logger));
    EXPECT_TRUE(children.Ready(HostChild::Logger)) << "a logger has no channel: running is ready";
    ASSERT_TRUE(WaitForStart());
    EXPECT_EQ(g_entries.load(), 1);
    EXPECT_FALSE(g_observed.ownsProcess) << "a thread-hosted logger never owns a process";
    EXPECT_TRUE(g_observed.hasStopEvent);
    EXPECT_TRUE(g_observed.hasLogSink);
    EXPECT_EQ(g_observed.controllerPid, GetCurrentProcessId());
    EXPECT_TRUE(fs::exists(session / RoleLogName()));

    // Ensure on a running logger starts no second thread.
    EXPECT_TRUE(children.Ensure(HostChild::Logger));
    EXPECT_EQ(g_entries.load(), 1);

    EXPECT_TRUE(children.Shutdown());
    ASSERT_TRUE(WaitForThreadExit());
    EXPECT_EQ(ExitCodeOf(g_threadHandle), 0u);
    EXPECT_NE(ReadFile(session / RoleLogName())
                  .find("[Logger] Dedicated logging service started"),
              std::string::npos);
}

TEST_F(LoggerThreadHostTest, RingRecordsOfAnAttachedSessionLandInItsFilesBesideTheLoggerLog) {
    TempRoot root;
    const fs::path session = root.path() / "session";
    fs::create_directories(session);
    FakeRings rings;
    g_rings = &rings;
    SharedMemoryLayout& ring = rings.AddRing(4242);
    rings.Publish(4242, session.string());
    const auto supervisor = MakeLoggerSupervisor(session, root.path() / "logs");
    ChildSupervisor& children = *supervisor;

    ASSERT_TRUE(children.Ensure(HostChild::Logger));
    ASSERT_TRUE(WaitForStart());
    PushRecord(ring, "hook_debug.log", "first record");
    PushRecord(ring, "custom.log", "second record");
    PushRecord(ring, "hook_debug.log", "third record");

    EXPECT_TRUE(children.Shutdown());
    ASSERT_TRUE(WaitForThreadExit());
    EXPECT_EQ(ExitCodeOf(g_threadHandle), 0u);
    EXPECT_EQ(EntryNames(session), std::set<std::string>({RoleLogName(), "custom.log", "hook_debug.log"}));
    EXPECT_EQ(ReadFile(session / "hook_debug.log"), "first record\r\nthird record\r\n");
    EXPECT_EQ(ReadFile(session / "custom.log"), "second record\r\n");
    EXPECT_EQ(ring.logs.readIndex.load(), ring.logs.writeIndex.load());
    EXPECT_EQ(rings.closedRings, 1);
    const std::string roleLog = ReadFile(session / RoleLogName());
    EXPECT_NE(roleLog.find("[Logger] Discovered new session: PID 4242"), std::string::npos);
    EXPECT_NE(roleLog.find("[Logger] Stop requested by the runtime"), std::string::npos);
}

TEST_F(LoggerThreadHostTest, ReconfigureWithLoggerOffStopsTheThreadThroughItsStopEvent) {
    TempRoot root;
    const fs::path session = root.path() / "session";
    fs::create_directories(session);
    FakeRings rings;
    g_rings = &rings;
    const auto supervisor = MakeLoggerSupervisor(session, root.path() / "logs");
    ChildSupervisor& children = *supervisor;

    ASSERT_TRUE(children.Ensure(HostChild::Logger));
    ASSERT_TRUE(WaitForStart());

    children.ReconfigureServices(AuxiliaryServices{});
    EXPECT_FALSE(children.Present(HostChild::Logger)) << "the retired logger is no longer the active identity";
    EXPECT_FALSE(children.Running(HostChild::Logger));
    // The retired thread ends on its stop event alone; nothing else was signalled.
    ASSERT_TRUE(WaitForThreadExit());
    EXPECT_EQ(ExitCodeOf(g_threadHandle), 0u);
    EXPECT_NE(ReadFile(session / RoleLogName())
                  .find("[Logger] Stop requested by the runtime"),
              std::string::npos);

    // Shutdown collects the exited thread and has nothing left to wait for.
    EXPECT_TRUE(children.Shutdown());
    EXPECT_EQ(rings.closedRings, 0) << "no session was attached, so no ring was released";
}

TEST_F(LoggerThreadHostTest, ShutdownStopsTheRunningLoggerAndReleasesItsRingsAndSessionFile) {
    TempRoot root;
    const fs::path session = root.path() / "session";
    fs::create_directories(session);
    FakeRings rings;
    g_rings = &rings;
    rings.AddRing(77);
    rings.Publish(77, session.string());
    const auto supervisor = MakeLoggerSupervisor(session, root.path() / "logs");
    ChildSupervisor& children = *supervisor;

    ASSERT_TRUE(children.Ensure(HostChild::Logger));
    ASSERT_TRUE(WaitForStart());

    EXPECT_TRUE(children.Shutdown());
    ASSERT_TRUE(WaitForThreadExit());
    EXPECT_EQ(ExitCodeOf(g_threadHandle), 0u);
    EXPECT_FALSE(children.Present(HostChild::Logger));
    EXPECT_FALSE(children.Running(HostChild::Logger));
    EXPECT_FALSE(children.Ensure(HostChild::Logger)) << "a shut-down supervisor starts no logger";
    EXPECT_EQ(rings.closedRings, 1);
    // Nothing retains the role's sink: the file is no longer open, so it can be removed.
    std::error_code error;
    EXPECT_TRUE(fs::remove(session / RoleLogName(), error))
        << error.message();
}

TEST_F(LoggerThreadHostTest, WithoutASessionDirectoryTheLoggerWritesNoRoleFile) {
    TempRoot root;
    FakeRings rings;
    g_rings = &rings;
    const auto supervisor = MakeLoggerSupervisor(fs::path(), root.path() / "logs");
    ChildSupervisor& children = *supervisor;

    ASSERT_TRUE(children.Ensure(HostChild::Logger));
    ASSERT_TRUE(WaitForStart());
    EXPECT_FALSE(g_observed.hasLogSink);

    EXPECT_TRUE(children.Shutdown());
    ASSERT_TRUE(WaitForThreadExit());
    EXPECT_EQ(ExitCodeOf(g_threadHandle), 0u);
    EXPECT_TRUE(EntryNames(root.path()).empty());
}
