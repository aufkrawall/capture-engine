#include <gtest/gtest.h>

#include <windows.h>
#include <winternl.h>

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
#include "common/ipc/av_sync_latency_channel.h"
#include "common/ipc/media_finalization.h"
#include "common/ipc/role_channel_memory.h"
#include "common/ipc/role_context.h"
#include "common/ipc/shared_defs.h"
#include "common/logging/logging.h"

namespace fs = std::filesystem;
namespace targets = ce::vulkan_layer_targets;

// The real media role, run the way the runtime's thread host runs it (RoleThreadRuns::Spawn for ChildRole::Media): on its
// own thread, with a thread-mode context, over the in-memory command channel, its result slot and latency channel handles
// duplicated from the spawner's. Session names live in the unit-test namespace (CE_UNIT_TESTS), so a CaptureEngine running
// on this machine is neither reached nor changed.
//
// Nothing here starts a recording or capture, injects, or opens an audio endpoint. The configuration has no enabled audio
// source (LoadSafeConfig checks it), and the render-latency probe is off (audio_latency_autodetect=0): the probe is the one
// step that plays a marker tone, and MeasureRenderLatencyOnce returns before calling it. Case 1 only: a product
// mediaengine.dll is refused by the unit-test build identity (mediaengine_loader.cpp, MediaEngineBuildIdentityMatches).

namespace {

// A generous bound on the role's exit. It only turns a hung role into a failure: nothing waits on time.
constexpr auto kExitBound = std::chrono::seconds(30);

// A fresh directory for one test, removed afterwards.
class TempRoot {
public:
    TempRoot() {
        static unsigned long long sequence = 0;
        path_ = fs::temp_directory_path() / ("ce_media_role_" + std::to_string(GetCurrentProcessId()) + "_" +
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

// The product's persisted Vulkan target list, read without writing anything. nullopt when the value or its key is absent.
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

// Closes a handle the test still owns, if the role has not closed it already. GetHandleInformation tells the two apart.
void CloseIfOpen(HANDLE handle) {
    DWORD flags = 0;
    if (handle && GetHandleInformation(handle, &flags))
        CloseHandle(handle);
}

// How many handles name the object `handle` names. The role's copy is one of them, so the count falls by one when the
// role closes it. Unlike probing the copy's handle value, which the process may hand out again once it is closed, the
// count is not disturbed by handles other threads open meanwhile. nullopt when the query fails.
std::optional<ULONG> ObjectHandleCount(HANDLE handle) {
    using NtQueryObjectFn = NTSTATUS(NTAPI*)(HANDLE, OBJECT_INFORMATION_CLASS, PVOID, ULONG, PULONG);
    static const auto query =
        reinterpret_cast<NtQueryObjectFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject"));
    PUBLIC_OBJECT_BASIC_INFORMATION info{};
    if (!query || !handle || query(handle, ObjectBasicInformation, &info, sizeof(info), nullptr) < 0)
        return std::nullopt;
    return info.HandleCount;
}

// The configuration the role reads. Every output and cache folder lives in the test root; the system audio source and the
// microphone are off, and the render-latency probe is disabled, so MediaEngine_Init creates no audio capture and no probe
// runs. info keeps the sink small.
void WriteConfig(const fs::path& path, const fs::path& root) {
    std::ofstream out(path, std::ios::binary);
    out << "[Logging]\r\nlog_level=info\r\n"
        << "[SystemAudio]\r\nenabled=0\r\n"
        << "[Microphone]\r\nenabled=0\r\n"
        << "[AudioSync]\r\naudio_latency_autodetect=0\r\n"
        << "[Output]\r\noutput_dir=" << (root / "captures").string() << "\r\n"
        << "[Benchmark]\r\noutput_dir=" << (root / "benchmarks").string() << "\r\n";
}

bool NoAudioSourceEnabled(const AppConfig& config) {
    for (const AudioConfig& source : config.audioSources) {
        if (source.enabled)
            return false;
    }
    return true;
}

std::wstring TestExecutablePath() {
    wchar_t path[MAX_PATH * 4] = {};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return path;
}

// Duplicates a handle into this process, as the thread host hands a handle to the role.
HANDLE DuplicateHere(HANDLE source) {
    HANDLE duplicate = nullptr;
    if (!source || !DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &duplicate, 0, FALSE,
                                    DUPLICATE_SAME_ACCESS)) {
        return nullptr;
    }
    return duplicate;
}

}  // namespace

class MediaRoleThreadTest : public ::testing::Test {
protected:
    void SetUp() override {
        Log_Init(processLog_.string(), LogLevel::Debug);
        productBefore_ = ReadListValue(targets::kProductRegistryKey);
    }

    void TearDown() override {
        StopRole();
        // A handle the role was given is the role's to close. Only a start that never reached the role leaves them here.
        if (!roleTookHandles_) {
            CloseIfOpen(resultCopy_);
            CloseIfOpen(latencyCopy_);
        }
        // The session latency channel is process-lifetime; the test releases the one it created.
        ce::av_sync::ReleaseSessionLatencyChannel();
        EXPECT_TRUE(ReadListValue(targets::kProductRegistryKey) == productBefore_)
            << "the media role changed the product's persisted Vulkan target list";
        Log_Shutdown();
    }

    // Loads the configuration and fails the test unless it is the audio-free, probe-free one this file relies on.
    bool LoadSafeConfig() {
        WriteConfig(configPath_, root_.path());
        LoadConfig(configPath_.string(), config_);
        // The safety preconditions, checked on the configuration the role will get (not on the file text).
        EXPECT_TRUE(NoAudioSourceEnabled(config_)) << "a test configuration must not enable any audio source";
        EXPECT_FALSE(config_.audioLatencyAutodetect) << "the render-latency probe must stay off";
        EXPECT_EQ(config_.audioCaptureLatencyMs, 0.0f) << "a manual latency override would also skip the probe";
        return NoAudioSourceEnabled(config_) && !config_.audioLatencyAutodetect;
    }

    // Starts the media role on its own thread, as RoleThreadRuns::Spawn does: the in-memory channel, a stop event, the
    // session log sink, and the result slot and session latency channel handles duplicated for the role. `packageDir`
    // is the package the role loads mediaengine.dll from. Returns false when the channel or the sink cannot be created.
    bool StartRole(const fs::path& packageDir) {
        if (!LoadSafeConfig())
            return false;
        pair_ = ce::role::MakeInMemoryChannel(ProcessMode::Media, GetCurrentProcessId());
        if (!pair_.host || !pair_.role)
            return false;

        // The thread host's copies of the spawner's handles: the result slot the child reports into, and the controller-
        // lifetime latency channel the child maps (role_thread_runs.cpp).
        resultSlot_ = ce::media_result::ResultSlot::Create();
        if (!resultSlot_)
            return false;
        resultOriginal_ = static_cast<HANDLE>(resultSlot_->ChildHandle());
        latencyOriginal_ = static_cast<HANDLE>(ce::av_sync::GetSessionLatencyChannelChildHandle());
        resultHandlesBefore_ = ObjectHandleCount(resultOriginal_);
        latencyHandlesBefore_ = ObjectHandleCount(latencyOriginal_);
        if (!resultHandlesBefore_ || !latencyHandlesBefore_)
            return false;
        resultCopy_ = DuplicateHere(resultOriginal_);
        latencyCopy_ = DuplicateHere(latencyOriginal_);
        if (!resultCopy_ || !latencyCopy_)
            return false;

        context_.mode = ProcessMode::Media;
        context_.ownsProcess = false;
        context_.processTuning = false;
        context_.controllerPid = GetCurrentProcessId();
        context_.packageDir = packageDir.wstring();
        context_.roleHostPath = TestExecutablePath();
        context_.configPath = configPath_.string();
        context_.logsRoot = (root_.path() / "logs").string();
        context_.channel = pair_.role.get();
        context_.stopEvent = stop_.get();
        context_.resultSlotHandle = resultCopy_;
        context_.latencyChannelHandle = latencyCopy_;
        context_.logSink = ce::log::OpenSink(sinkPath_.string());
        if (!context_.logSink)
            return false;
        roleTookHandles_ = true;
        role_ = std::make_unique<RoleThread>([this] {
            ce::log::SinkScope sink(context_.logSink);
            return ce::role::RunMediaRole(context_, config_);
        });
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

    // Closes the sink (the role is over), then the process log, so both can be read.
    void FlushLogs() {
        context_.logSink.reset();
        Log_Shutdown();
    }

    // The role closed the copies it was given: each object is back to the handles the test itself holds.
    void ExpectRoleClosedItsHandles() {
        EXPECT_EQ(ObjectHandleCount(resultOriginal_), resultHandlesBefore_)
            << "the role must close the result slot handle it was given";
        EXPECT_EQ(ObjectHandleCount(latencyOriginal_), latencyHandlesBefore_)
            << "the role must close the latency channel handle it was given";
    }

    // Declared after root_, which they live in.
    TempRoot root_;
    fs::path configPath_ = root_.path() / "config.ini";
    fs::path sinkPath_ = root_.path() / "media.log";
    fs::path processLog_ = root_.path() / "process.log";
    fs::path packageDir_ = root_.path() / "package";

    StopEvent stop_;
    ce::role::InMemoryChannelPair pair_;
    ce::role::RoleContext context_;
    AppConfig config_;
    std::unique_ptr<RoleThread> role_;
    std::unique_ptr<ce::media_result::ResultSlot> resultSlot_;
    HANDLE resultOriginal_ = nullptr;
    HANDLE latencyOriginal_ = nullptr;
    std::optional<ULONG> resultHandlesBefore_;
    std::optional<ULONG> latencyHandlesBefore_;
    HANDLE resultCopy_ = nullptr;
    HANDLE latencyCopy_ = nullptr;
    bool roleTookHandles_ = false;
    std::optional<std::wstring> productBefore_;
};

// The package has no mediaengine.dll: the role's first load fails, so Init returns the init-failure code without any
// further work. The role closes the result slot and latency channel handles it was given, leaves no thread running, and
// writes its lines to its own sink.
TEST_F(MediaRoleThreadTest, MissingMediaEngineDllFailsInitPromptlyAndClosesTheHandlesItWasGiven) {
    fs::create_directories(packageDir_);
    ASSERT_TRUE(StartRole(packageDir_));
    const std::optional<int> exitCode = role_->WaitForExit();
    ASSERT_TRUE(exitCode.has_value()) << "the role did not return after its init failure";
    EXPECT_EQ(exitCode.value_or(-1), 1);
    ExpectRoleClosedItsHandles();
    FlushLogs();

    const std::string sink = ReadFile(sinkPath_);
    const std::string process = ReadFile(processLog_);
    EXPECT_TRUE(Contains(sink, "[Media] Failed to load mediaengine.dll")) << sink;
    EXPECT_TRUE(Contains(sink, "[Media] Process exiting")) << sink;
    EXPECT_FALSE(Contains(process, "[Media]")) << "the role's lines reached the process log";
    EXPECT_TRUE(Contains(process, "[BUILD] Version="))
        << "the process log must be live for the check above to mean anything";
    // Nothing was written beside the package: the failed start creates no output or log folder under it.
    EXPECT_TRUE(fs::is_empty(packageDir_)) << "the failed start wrote into the package directory";
    EXPECT_FALSE(fs::exists(root_.path() / "captures"));
}

// A start the role refuses before it maps anything (no package directory) still owns the handles the host gave it: they
// are closed on the way out, as the other init failure closes them.
TEST_F(MediaRoleThreadTest, RefusedStartWithoutPackageDirClosesTheHandlesItWasGiven) {
    ASSERT_TRUE(StartRole(fs::path()));
    const std::optional<int> exitCode = role_->WaitForExit();
    ASSERT_TRUE(exitCode.has_value());
    EXPECT_EQ(exitCode.value_or(-1), 1);
    ExpectRoleClosedItsHandles();
}
