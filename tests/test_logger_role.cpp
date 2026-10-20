#include <gtest/gtest.h>

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include "captureengine/diagnostics/logger_service.h"
#include "captureengine/roles/role_entries.h"
#include "common/ipc/role_context.h"
#include "common/ipc/shared_defs.h"
#include "common/logging/log_ring.h"
#include "common/logging/logging.h"

namespace fs = std::filesystem;

namespace {

// A generous bound on a role's exit. It only turns a hung role into a failure: nothing waits on time.
constexpr auto kExitBound = std::chrono::seconds(30);

// Rings and a discovery record standing in for the named shared mappings. The role thread is the only
// user while the role runs; the test reads the fake after the role has exited.
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

    // A ring that passes the role's ABI and version check, as the hook's producer initializes it.
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

// The role runs on its own thread, as the runtime's thread host runs it, and its exit code is collected here.
class RoleThread {
public:
    explicit RoleThread(std::function<int()> role)
        : exitCode_(exit_.get_future()), thread_([this, role = std::move(role)] { exit_.set_value(role()); }) {}
    RoleThread(const RoleThread&) = delete;
    RoleThread& operator=(const RoleThread&) = delete;
    ~RoleThread() {
        if (thread_.joinable())
            thread_.join();
    }

    // The role's exit code, or nullopt when it did not exit within the bound.
    std::optional<int> WaitForExit() {
        if (exitCode_.wait_for(kExitBound) != std::future_status::ready)
            return std::nullopt;
        const int code = exitCode_.get();
        thread_.join();
        return code;
    }

private:
    std::promise<int> exit_;
    std::future<int> exitCode_;
    std::thread thread_;
};

// A manual-reset event the test sets to stop the role, as the runtime does.
class StopEvent {
public:
    StopEvent() : handle_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~StopEvent() {
        if (handle_)
            CloseHandle(handle_);
    }
    StopEvent(const StopEvent&) = delete;
    StopEvent& operator=(const StopEvent&) = delete;
    HANDLE get() const {
        return handle_;
    }
    void Set() {
        SetEvent(handle_);
    }

private:
    HANDLE handle_;
};

// A fresh directory for one test, removed afterwards.
class TempRoot {
public:
    TempRoot() {
        static unsigned long long sequence = 0;
        path_ = fs::temp_directory_path() / ("ce_logger_role_" + std::to_string(GetCurrentProcessId()) + "_" +
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

// A thread-hosted context (DR-39): no process effects, the runtime's stop event, the runtime's logs root.
// The controller PID is the test's own process, which a thread-hosted logger must never wait on.
ce::role::RoleContext ThreadContext(HANDLE stopEvent, const fs::path& logsRoot) {
    ce::role::RoleContext context;
    context.ownsProcess = false;
    context.processTuning = false;
    context.stopEvent = stopEvent;
    context.controllerPid = GetCurrentProcessId();
    context.logsRoot = logsRoot.string();
    return context;
}

// The directory of this test executable: the logger must never create a `logs` folder there in thread mode.
fs::path ExecutableDirectory() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path).parent_path();
}

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

}  // namespace

// The logger's stop is the stop event alone for a thread-hosted role, and the log rings it drains produce
// the same files they produce in process mode.

TEST(LoggerRoleTest, StopEventEndsTheThreadRoleWithoutCreatingAnythingElse) {
    TempRoot root;
    StopEvent stop;
    FakeRings rings;
    AppConfig config;
    ce::role::RoleContext context = ThreadContext(stop.get(), root.path() / "logs");
    context.logSink = ce::log::OpenSink((root.path() / "role.log").string());
    ASSERT_NE(context.logSink, nullptr);
    const bool executableLogsExisted = fs::exists(ExecutableDirectory() / "logs");

    RoleThread role([&] { return logger_service::RunLoggerWithRings(context, config, rings); });
    stop.Set();
    const std::optional<int> exitCode = role.WaitForExit();

    ASSERT_TRUE(exitCode.has_value()) << "the role did not exit after its stop event";
    EXPECT_EQ(exitCode.value_or(-1), 0);
    EXPECT_EQ(rings.closedRings, 0);
    EXPECT_EQ(EntryNames(root.path()), std::set<std::string>({"role.log"}));
    EXPECT_EQ(fs::exists(ExecutableDirectory() / "logs"), executableLogsExisted);
    EXPECT_NE(ReadFile(root.path() / "role.log").find("[Logger] Dedicated logging service started"),
              std::string::npos);
}

TEST(LoggerRoleTest, RingRecordsLandInTheDiscoveredSessionFilesAndTheRoleSink) {
    TempRoot root;
    StopEvent stop;
    // The stop is already set: the role makes one pass over the rings, then its final drain, and exits.
    stop.Set();
    FakeRings rings;
    SharedMemoryLayout& ring = rings.AddRing(4242);
    PushRecord(ring, "hook_debug.log", "first record");
    PushRecord(ring, "custom.log", "second record");
    PushRecord(ring, "hook_debug.log", "third record");
    const fs::path session = root.path() / "session";
    rings.Publish(4242, session.string());
    AppConfig config;
    ce::role::RoleContext context = ThreadContext(stop.get(), root.path() / "logs");
    context.logSink = ce::log::OpenSink((root.path() / "role.log").string());

    RoleThread role([&] { return logger_service::RunLoggerWithRings(context, config, rings); });
    const std::optional<int> exitCode = role.WaitForExit();

    ASSERT_TRUE(exitCode.has_value()) << "the role did not exit after its stop event";
    EXPECT_EQ(exitCode.value_or(-1), 0);
    EXPECT_EQ(EntryNames(session), std::set<std::string>({"custom.log", "hook_debug.log"}));
    EXPECT_EQ(ReadFile(session / "hook_debug.log"), "first record\r\nthird record\r\n");
    EXPECT_EQ(ReadFile(session / "custom.log"), "second record\r\n");
    EXPECT_EQ(ring.logs.readIndex.load(), ring.logs.writeIndex.load());
    EXPECT_EQ(rings.closedRings, 1);
    const std::string roleLog = ReadFile(root.path() / "role.log");
    EXPECT_NE(roleLog.find("[Logger] Discovered new session: PID 4242"), std::string::npos);
    EXPECT_NE(roleLog.find("[Logger] Stop requested by the runtime"), std::string::npos);
    EXPECT_FALSE(fs::exists(root.path() / "logs"));
}

TEST(LoggerRoleTest, EmptyDiscoveryPathFallsBackToTheRuntimeLogsRoot) {
    TempRoot root;
    StopEvent stop;
    stop.Set();
    FakeRings rings;
    PushRecord(rings.AddRing(7), "fallback.log", "fallback record");
    rings.Publish(7, "");
    AppConfig config;
    const fs::path logsRoot = root.path() / "logs";
    ce::role::RoleContext context = ThreadContext(stop.get(), logsRoot);
    const bool executableLogsExisted = fs::exists(ExecutableDirectory() / "logs");

    RoleThread role([&] { return logger_service::RunLoggerWithRings(context, config, rings); });
    const std::optional<int> exitCode = role.WaitForExit();

    ASSERT_TRUE(exitCode.has_value()) << "the role did not exit after its stop event";
    EXPECT_EQ(exitCode.value_or(-1), 0);
    EXPECT_EQ(ReadFile(logsRoot / "fallback.log"), "fallback record\r\n");
    EXPECT_EQ(fs::exists(ExecutableDirectory() / "logs"), executableLogsExisted);
}

TEST(LoggerRoleTest, ThreadRoleWithoutStopEventDoesNotStart) {
    TempRoot root;
    FakeRings rings;
    PushRecord(rings.AddRing(9), "hook_debug.log", "never drained");
    rings.Publish(9, (root.path() / "session").string());
    AppConfig config;
    ce::role::RoleContext context = ThreadContext(nullptr, root.path() / "logs");
    context.logSink = ce::log::OpenSink((root.path() / "role.log").string());

    RoleThread role([&] { return logger_service::RunLoggerWithRings(context, config, rings); });
    const std::optional<int> exitCode = role.WaitForExit();

    ASSERT_TRUE(exitCode.has_value());
    EXPECT_EQ(exitCode.value_or(-1), 1);
    EXPECT_EQ(rings.closedRings, 0);
    EXPECT_FALSE(fs::exists(root.path() / "session"));
    EXPECT_NE(ReadFile(root.path() / "role.log").find("no stop event"), std::string::npos);
}

TEST(LoggerRoleTest, LoggingOffStartsNothing) {
    TempRoot root;
    FakeRings rings;
    PushRecord(rings.AddRing(11), "hook_debug.log", "never drained");
    rings.Publish(11, (root.path() / "session").string());
    AppConfig config;
    config.logLevel = LogLevel::Off;
    ce::role::RoleContext context = ThreadContext(nullptr, root.path() / "logs");

    EXPECT_EQ(logger_service::RunLoggerWithRings(context, config, rings), 0);
    EXPECT_EQ(rings.closedRings, 0);
    EXPECT_TRUE(EntryNames(root.path()).empty());
}

// The process host's logger never runs orphaned: a controller that is already gone ends the role at once,
// before any ring is attached.
TEST(LoggerRoleTest, ProcessRoleExitsWhenItsControllerIsAlreadyGone) {
    TempRoot root;
    FakeRings rings;
    PushRecord(rings.AddRing(13), "hook_debug.log", "never drained");
    rings.Publish(13, (root.path() / "session").string());
    AppConfig config;
    ce::role::RoleContext context;
    context.ownsProcess = true;
    context.controllerPid = 0xFFFFFFF0u;  // no process has this ID
    context.logsRoot = (root.path() / "logs").string();
    context.logSink = ce::log::OpenSink((root.path() / "role.log").string());

    RoleThread role([&] { return ce::role::RunLoggerRole(context, config); });
    const std::optional<int> exitCode = role.WaitForExit();

    ASSERT_TRUE(exitCode.has_value());
    EXPECT_EQ(exitCode.value_or(-1), 1);
    EXPECT_EQ(rings.closedRings, 0);
    EXPECT_FALSE(fs::exists(root.path() / "session"));
}

// The shared-ring source resolves discovery in the test namespace (session_namespace.h), so a running
// CaptureEngine's rings are never drained here and the test runs on every machine.
TEST(LoggerRoleTest, RunLoggerRoleOverSharedRingsStopsOnItsStopEvent) {
    TempRoot root;
    StopEvent stop;
    AppConfig config;
    ce::role::RoleContext context = ThreadContext(stop.get(), root.path() / "logs");
    context.logSink = ce::log::OpenSink((root.path() / "role.log").string());
    const bool executableLogsExisted = fs::exists(ExecutableDirectory() / "logs");

    RoleThread role([&] { return ce::role::RunLoggerRole(context, config); });
    stop.Set();
    const std::optional<int> exitCode = role.WaitForExit();

    ASSERT_TRUE(exitCode.has_value()) << "the role did not exit after its stop event";
    EXPECT_EQ(exitCode.value_or(-1), 0);
    EXPECT_EQ(EntryNames(root.path()), std::set<std::string>({"role.log"}));
    EXPECT_EQ(fs::exists(ExecutableDirectory() / "logs"), executableLogsExisted);
}
