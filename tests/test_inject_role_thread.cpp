#include <gtest/gtest.h>

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include "captureengine/roles/role_entries.h"
#include "common/config/config.h"
#include "common/graphics/vulkan_layer_target_list.h"
#include "common/ipc/role_channel_memory.h"
#include "common/ipc/role_context.h"
#include "common/ipc/shared_defs.h"
#include "common/logging/logging.h"
#include "common/overlay/inject_overlay_policy.h"
#include "common/platform/runtime_process_identity.h"

namespace fs = std::filesystem;
namespace targets = ce::vulkan_layer_targets;

// The real inject role, run the way the runtime's thread host runs it (DR-37, DR-38, DR-40): on its own thread, with a
// thread-mode context, over the in-memory command channel. The configuration has no whitelist, so the role never builds
// an InjectionManager and no process on the machine is touched. Session names and the Vulkan target list live in the
// unit-test namespace (CE_UNIT_TESTS), so a CaptureEngine running on this machine is neither reached nor changed.

namespace {

// A generous bound on any reply or exit. It only turns a hung role into a failure: nothing waits on time.
constexpr DWORD kReplyBoundMs = 30000;
constexpr auto kExitBound = std::chrono::seconds(30);

// A fresh directory for one test, removed afterwards.
class TempRoot {
public:
    TempRoot() {
        static unsigned long long sequence = 0;
        path_ = fs::temp_directory_path() / ("ce_inject_role_" + std::to_string(GetCurrentProcessId()) + "_" +
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

// A manual-reset event the test sets to stop the role, as the runtime's thread host does.
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

// The role runs on its own thread and its exit code is collected here.
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

    // The role's exit code, or nullopt when it did not exit within the bound. Repeated calls return the same code: the
    // future is consumed by the first exit, so a second get would be undefined.
    std::optional<int> WaitForExit() {
        if (!exitCode_.valid())
            return exited_;
        if (exitCode_.wait_for(kExitBound) != std::future_status::ready)
            return std::nullopt;
        exited_ = exitCode_.get();
        thread_.join();
        return exited_;
    }

private:
    std::promise<int> exit_;
    std::future<int> exitCode_;
    std::optional<int> exited_;
    std::thread thread_;
};

std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool Contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

// The configuration the role reads. It has no whitelist section, so both lists stay empty and BuildInjectorConfigState
// leaves injection off. The benchmark folder is a plain path inside the test root: the template's default lives under
// %DOCUMENTS%, which publishing the configuration would create. Trace is the default level; info keeps the log small
// and the persisted target list exactly the (empty) whitelist.
void WriteConfig(const fs::path& path, const fs::path& benchmarkDir) {
    std::ofstream out(path, std::ios::binary);
    out << "[Logging]\r\nlog_level=info\r\n[Benchmark]\r\noutput_dir=" << benchmarkDir.string() << "\r\n";
}

// The list value's content, read without writing anything. nullopt when the value or its key is absent.
std::optional<std::wstring> ReadListValue(const wchar_t* key) {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key, targets::kRegistryValue, RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &bytes) !=
        ERROR_SUCCESS)
        return std::nullopt;
    std::wstring data(bytes / sizeof(wchar_t) + 1, L'\0');
    DWORD size = static_cast<DWORD>(data.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, key, targets::kRegistryValue, RRF_RT_REG_MULTI_SZ, nullptr, data.data(),
                     &size) != ERROR_SUCCESS)
        return std::nullopt;
    data.resize(size / sizeof(wchar_t));
    return data;
}

bool KeyExists(const wchar_t* key) {
    HKEY handle = nullptr;
    const LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &handle);
    if (status == ERROR_SUCCESS)
        RegCloseKey(handle);
    return status == ERROR_SUCCESS;
}

// The discovery advertisement as a read-only consumer sees it: the mapping's magic and inject PID. nullopt when no
// mapping of that name exists.
std::optional<std::pair<uint32_t, uint32_t>> ReadDiscovery() {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
    if (!mapping)
        return std::nullopt;
    std::optional<std::pair<uint32_t, uint32_t>> advertised;
    const auto* view =
        static_cast<const DiscoveryInfo*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo)));
    if (view) {
        advertised = std::make_pair(view->GetMagic(), view->GetInjectPid());
        UnmapViewOfFile(view);
    }
    CloseHandle(mapping);
    return advertised;
}

std::wstring TestExecutablePath() {
    wchar_t path[MAX_PATH * 4] = {};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return path;
}

}  // namespace

class InjectRoleThreadTest : public ::testing::Test {
protected:
    void SetUp() override {
        Log_Init(processLog_.string(), LogLevel::Debug);
        ce::runtime_identity::ClearHostForTests();
        productBefore_ = ReadListValue(targets::kProductRegistryKey);
        utKeyExisted_ = KeyExists(targets::kRegistryKey);
    }

    void TearDown() override {
        StopRole();
        ce::runtime_identity::ClearHostForTests();
        // The test removes only what it wrote in its own key: the value, and the key when this test created it.
        targets::DeletePersistedTargetList();
        if (!utKeyExisted_ && std::wstring(targets::kRegistryKey) != targets::kProductRegistryKey)
            RegDeleteKeyW(HKEY_CURRENT_USER, targets::kRegistryKey);
        EXPECT_TRUE(ReadListValue(targets::kProductRegistryKey) == productBefore_)
            << "the role changed the product's persisted Vulkan target list";
        Log_Shutdown();
    }

    // Starts the role on its own thread. Returns false when the channel, the sink or the injection safety check fails.
    bool StartRole() {
        WriteConfig(configPath_, root_.path() / "benchmarks");
        AppConfig loaded;
        LoadConfig(configPath_.string(), loaded);
        EXPECT_TRUE(loaded.gameWhitelist.empty() && loaded.overlayWhitelist.empty());
        // The safety precondition: with this configuration the role cannot construct an InjectionManager.
        if (BuildInjectorConfigState(loaded).allowInjection)
            return false;
        config_ = loaded;

        pair_ = ce::role::MakeInMemoryChannel(ProcessMode::Inject, GetCurrentProcessId());
        if (!pair_.host || !pair_.role)
            return false;
        context_.mode = ProcessMode::Inject;
        context_.ownsProcess = false;
        context_.processTuning = false;
        context_.controllerPid = GetCurrentProcessId();
        context_.packageDir = (root_.path() / "package").wstring();
        context_.roleHostPath = TestExecutablePath();
        context_.configPath = configPath_.string();
        context_.logsRoot = (root_.path() / "logs").string();
        context_.channel = pair_.role.get();
        context_.stopEvent = stop_.get();
        context_.logSink = ce::log::OpenSink(sinkPath_.string());
        if (!context_.logSink)
            return false;
        role_ = std::make_unique<RoleThread>([this] { return ce::role::RunInjectRole(context_, config_); });
        return true;
    }

    // Stops a running role the way a host does, then waits for it. A disconnect and the stop event both end the role.
    void StopRole() {
        if (!role_)
            return;
        if (pair_.host)
            pair_.host->Disconnect();
        stop_.Set();
        role_->WaitForExit();
        role_.reset();
        context_.logSink.reset();
    }

    ce::role::HelperClient& host() {
        return *pair_.host;
    }

    // Declared after root_, which they live in.
    TempRoot root_;
    fs::path configPath_ = root_.path() / "config.ini";
    fs::path sinkPath_ = root_.path() / "inject.log";
    fs::path processLog_ = root_.path() / "process.log";

    StopEvent stop_;
    ce::role::InMemoryChannelPair pair_;
    ce::role::RoleContext context_;
    AppConfig config_;
    std::unique_ptr<RoleThread> role_;
    std::optional<std::wstring> productBefore_;
    bool utKeyExisted_ = false;
};

// The Vulkan target list is written under the test key and never the product key: the claim this file rests on.
TEST_F(InjectRoleThreadTest, PingIsAnsweredWithPongAndReloadConfigWithAck) {
    ASSERT_TRUE(StartRole());
    ProcessResponse response = ProcessResponse::None;
    ASSERT_TRUE(host().SendCommand(ProcessCommand::Ping, nullptr, &response, kReplyBoundMs));
    EXPECT_EQ(response, ProcessResponse::Pong);

    // Startup published the whitelist cache, which persists the target list under the test key. The whitelist is
    // empty, so the list names no executable; RegGetValue may hand back the terminator doubled, so the test checks
    // for content rather than the exact bytes.
    ASSERT_STRNE(targets::kRegistryKey, targets::kProductRegistryKey);
    const auto persisted = ReadListValue(targets::kRegistryKey);
    ASSERT_TRUE(persisted.has_value()) << "the role must write the test key\n" << ReadFile(sinkPath_);
    EXPECT_EQ(persisted.value_or(std::wstring{}).find_first_not_of(L'\0'), std::wstring::npos)
        << "an empty whitelist persists no executable";
    EXPECT_TRUE(ReadListValue(targets::kProductRegistryKey) == productBefore_)
        << "the role must not write the product key";

    response = ProcessResponse::None;
    ASSERT_TRUE(host().SendCommand(ProcessCommand::ReloadConfig, nullptr, &response, kReplyBoundMs));
    EXPECT_EQ(response, ProcessResponse::Ack);
}

TEST_F(InjectRoleThreadTest, ShutdownCommandEndsTheRoleWithExitCodeZero) {
    ASSERT_TRUE(StartRole());
    ProcessResponse response = ProcessResponse::None;
    ASSERT_TRUE(host().SendCommand(ProcessCommand::Ping, nullptr, &response, kReplyBoundMs));  // startup has finished

    response = ProcessResponse::None;
    ASSERT_TRUE(host().SendCommand(ProcessCommand::Shutdown, nullptr, &response, kReplyBoundMs));
    EXPECT_EQ(response, ProcessResponse::Ack);
    const std::optional<int> exitCode = role_->WaitForExit();
    ASSERT_TRUE(exitCode.has_value()) << "the role did not exit after a Shutdown command";
    EXPECT_EQ(exitCode.value_or(-1), 0);
}

TEST_F(InjectRoleThreadTest, StopEventAloneEndsTheRole) {
    ASSERT_TRUE(StartRole());
    ProcessResponse response = ProcessResponse::None;
    ASSERT_TRUE(host().SendCommand(ProcessCommand::Ping, nullptr, &response, kReplyBoundMs));

    stop_.Set();
    const std::optional<int> exitCode = role_->WaitForExit();
    ASSERT_TRUE(exitCode.has_value()) << "the role did not exit after its stop event";
    EXPECT_EQ(exitCode.value_or(-1), 0);
}

TEST_F(InjectRoleThreadTest, DiscoveryNamesThisProcessWhileRunningAndIsWithdrawnAtExit) {
    ASSERT_TRUE(StartRole());
    ProcessResponse response = ProcessResponse::None;
    ASSERT_TRUE(host().SendCommand(ProcessCommand::Ping, nullptr, &response, kReplyBoundMs));

    // The unit-test name space: the product's discovery mapping is Local\CE_Disc_71 and must stay apart from it.
    EXPECT_TRUE(std::wstring(SHARED_MEM_DISCOVERY).starts_with(L"Local\\CE_UT_")) << SHARED_MEM_DISCOVERY;
    const auto advertised = ReadDiscovery();
    ASSERT_TRUE(advertised.has_value()) << "the discovery mapping must exist while the role runs";
    const auto [magic, injectPid] = advertised.value_or(std::pair<uint32_t, uint32_t>{0, 0});
    EXPECT_EQ(magic, DISCOVERY_MAGIC);
    EXPECT_EQ(injectPid, GetCurrentProcessId());

    response = ProcessResponse::None;
    ASSERT_TRUE(host().SendCommand(ProcessCommand::Shutdown, nullptr, &response, kReplyBoundMs));
    const std::optional<int> exitCode = role_->WaitForExit();
    ASSERT_TRUE(exitCode.has_value());
    EXPECT_EQ(exitCode.value_or(-1), 0);

    // Teardown withdraws the advertisement (magic and PID cleared) before it closes its handle. A mapping that is
    // already gone counts as withdrawn too.
    const auto after = ReadDiscovery();
    if (after) {
        EXPECT_EQ(after->first, 0u);
        EXPECT_EQ(after->second, 0u);
    }
}

TEST_F(InjectRoleThreadTest, RoleLinesGoToItsSinkFileAndNotTheProcessLog) {
    ASSERT_TRUE(StartRole());
    ProcessResponse response = ProcessResponse::None;
    ASSERT_TRUE(host().SendCommand(ProcessCommand::Ping, nullptr, &response, kReplyBoundMs));
    response = ProcessResponse::None;
    ASSERT_TRUE(host().SendCommand(ProcessCommand::Shutdown, nullptr, &response, kReplyBoundMs));
    const std::optional<int> exitCode = role_->WaitForExit();
    ASSERT_TRUE(exitCode.has_value());
    EXPECT_EQ(exitCode.value_or(-1), 0);
    context_.logSink.reset();
    Log_Shutdown();

    const std::string sink = ReadFile(sinkPath_);
    const std::string process = ReadFile(processLog_);
    EXPECT_TRUE(Contains(sink, "[Inject] Process started")) << sink;
    EXPECT_TRUE(Contains(sink, "[Inject] Injection manager SKIPPED")) << sink;
    EXPECT_TRUE(Contains(sink, "[Inject] Injector shutdown complete.")) << sink;
    EXPECT_FALSE(Contains(process, "[Inject]")) << "the role's lines reached the process log";
    EXPECT_FALSE(Contains(process, "[InjectLifecycle]")) << "the role's lines reached the process log";
    EXPECT_TRUE(Contains(process, "[BUILD] Version="))
        << "the process log must be live for the check above to mean anything";
}

TEST_F(InjectRoleThreadTest, ProcessRegistersTheControllerAsTheRuntimeHost) {
    ASSERT_TRUE(StartRole());
    ProcessResponse response = ProcessResponse::None;
    ASSERT_TRUE(host().SendCommand(ProcessCommand::Ping, nullptr, &response, kReplyBoundMs));

    const ce::runtime_identity::HostIdentity registered = ce::runtime_identity::RegisteredHost();
    EXPECT_EQ(registered.pid, GetCurrentProcessId());
    EXPECT_EQ(registered.roleHostImage, TestExecutablePath());
    EXPECT_TRUE(ce::runtime_identity::IsRegisteredRuntimeProcess(GetCurrentProcessId()));

    response = ProcessResponse::None;
    ASSERT_TRUE(host().SendCommand(ProcessCommand::Shutdown, nullptr, &response, kReplyBoundMs));
    const std::optional<int> exitCode = role_->WaitForExit();
    ASSERT_TRUE(exitCode.has_value());
    EXPECT_EQ(exitCode.value_or(-1), 0);
}
