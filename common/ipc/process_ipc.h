#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

// Value 3 belonged to the retired limiter process; it stays unassigned so the
// remaining values never change meaning.
enum class ProcessMode : uint32_t { Controller = 0, Inject = 1, Media = 2, Logger = 4, Sensors = 5 };

enum class ProcessCommand : uint16_t {
    None = 0,
    Shutdown = 1,
    StartRecording = 2,
    StopRecording = 3,
    ReloadConfig = 4,
    Ping = 5,
    ToggleOverlay = 6,
    ToggleBenchmark = 7,
};

enum class ProcessResponse : uint16_t {
    None = 0,
    Ack = 1,
    Pong = 2,
    Error = 3,
    RecordingStarted = 4,
    RecordingStopped = 5,
};

enum class ProcessMessageKind : uint16_t {
    Startup = 1,
    Command = 2,
    Response = 3,
};

struct ProcessStatus {
    bool alive;
    uint32_t uptimeMs;
    ProcessMode mode;
    bool isRecording;
    char lastError[128];
};

using ProcessChannelNonce = std::array<uint8_t, 16>;

constexpr uint32_t PROCESS_MSG_MAGIC = 0x43415045;  // "CAPE"
constexpr uint16_t PROCESS_PROTOCOL_VERSION = 2;
constexpr size_t PROCESS_MAX_PAYLOAD = 256;

#pragma pack(push, 1)
struct ProcessMessage {
    uint32_t magic = PROCESS_MSG_MAGIC;
    uint16_t version = PROCESS_PROTOCOL_VERSION;
    uint16_t headerSize = 52;
    uint32_t totalSize = 52;
    ProcessMessageKind kind = ProcessMessageKind::Startup;
    uint16_t opcode = 0;
    ProcessMode senderMode = ProcessMode::Controller;
    uint64_t sequence = 0;
    uint32_t senderPid = 0;
    uint32_t payloadSize = 0;
    ProcessChannelNonce nonce{};
    char payload[PROCESS_MAX_PAYLOAD]{};
};
#pragma pack(pop)

static_assert(offsetof(ProcessMessage, payload) == 52, "ProcessMessage header ABI changed");
static_assert(sizeof(ProcessMessage) == 308, "ProcessMessage ABI changed");

bool ValidateProcessMessage(const ProcessMessage& message, size_t bytesRead, ProcessMessageKind expectedKind,
                            ProcessMode expectedSenderMode, uint32_t expectedSenderPid,
                            const ProcessChannelNonce& expectedNonce, uint64_t expectedSequence,
                            bool requireExactSequence);

ProcessMode ParseProcessMode(int argc, char* argv[]);
ProcessMode ParseProcessMode(LPCSTR commandLine);
const char* GetLogFileName(ProcessMode mode);
std::string GetProcessLogFileName(ProcessMode mode, const std::string& recordingId, uint32_t processId);

namespace ce::role {
// Role side of a helper's command channel (DR-37): the authenticated pipe in a helper process
// (ProcessIPCServer) or an in-memory peer when the role runs on a runtime thread. One command is in
// flight at a time; the role polls it and replies from the polling thread.
class RoleChannel {
public:
    virtual ~RoleChannel() = default;
    virtual bool PollCommand(ProcessCommand& command, char* payload = nullptr, size_t payloadSize = 0) = 0;
    virtual bool SendResponse(ProcessResponse response, const char* payload = nullptr) = 0;
    // Waits up to `timeoutMs` for a command to arrive; true when one may be ready to poll.
    virtual bool WaitForCommand(DWORD timeoutMs) = 0;
    virtual bool IsConnected() const = 0;
    // The host is gone or broke the channel: the role must stop.
    virtual bool HasFatalDisconnect() const = 0;
    // The runtime's process id (the controller of a helper process; the own process in-process).
    virtual uint32_t ControllerPid() const = 0;
};

// Host side of the same channel, used by the supervisor (ProcessIPCClient or the in-memory peer).
class HelperClient {
public:
    virtual ~HelperClient() = default;
    virtual bool SendCommand(ProcessCommand command, const char* payload = nullptr, ProcessResponse* response = nullptr,
                             DWORD timeoutMs = 1000) = 0;
    virtual bool IsConnected() const = 0;
    virtual void Disconnect() = 0;
};
}  // namespace ce::role
class ProcessIPCServer final : public ce::role::RoleChannel {
public:
    explicit ProcessIPCServer(ProcessMode mode);
    ~ProcessIPCServer() override;

    ProcessIPCServer(const ProcessIPCServer&) = delete;
    ProcessIPCServer& operator=(const ProcessIPCServer&) = delete;

    bool Init();
    void Shutdown();
    bool PollCommand(ProcessCommand& command, char* payload = nullptr, size_t payloadSize = 0) override;
    bool SendResponse(ProcessResponse response, const char* payload = nullptr) override;
    // Waits up to `timeoutMs` for the controller to ring this child's doorbell after writing a command
    // (an inherited auto-reset event, `--ipc-wake`); true when it rang. Without a doorbell it sleeps the
    // timeout. A ring carries no data: the pipe stays the only channel, PollCommand reads what it signals.
    bool WaitForCommand(DWORD timeoutMs) override;

    bool IsConnected() const override {
        return connected_.load(std::memory_order_acquire);
    }
    bool HasFatalDisconnect() const override {
        return fatalDisconnect_.load(std::memory_order_acquire);
    }
    // Controller PID from the startup arguments, accepted only after it matched the
    // authenticated pipe server process. 0 until ReadStartupArguments has validated it.
    uint32_t ControllerPid() const override {
        return controllerPid_;
    }

private:
    bool ReadStartupArguments();
    bool SendStartupHandshake();
    void MarkDisconnected(DWORD error, const char* operation);

    ProcessMode mode_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE wake_ = nullptr;  // the doorbell, set once by Init
    std::atomic<bool> connected_{false};
    std::atomic<bool> fatalDisconnect_{false};
    uint64_t lastSequence_ = 0;
    uint32_t controllerPid_ = 0;
    ProcessChannelNonce nonce_{};
    std::mutex mutex_;
};

// A reply that misses its window does not always mean a dead child. For
// commands that change nothing the controller depends on afterwards, the late
// reply is discarded when it arrives and the channel stays up; only a second
// miss while one is still outstanding proves the child is not serving its pipe.
// A state-changing command (start/stop recording, overlay toggles, shutdown)
// keeps the old rule, because the child may have acted on it while the
// controller concluded it had not - tearing the channel down makes the child
// unwind exactly that. Before this, one slow ReloadConfig ack killed the media
// child and with it a live recording.
enum class ReplyTimeoutOutcome : uint8_t {
    kKeepChannel,
    kBreakChannel,
};
ReplyTimeoutOutcome ClassifyReplyTimeout(ProcessCommand command, bool earlierReplyStillMissing);

class ProcessIPCClient final : public ce::role::HelperClient {
public:
    explicit ProcessIPCClient(ProcessMode targetMode);
    ~ProcessIPCClient() override;

    ProcessIPCClient(const ProcessIPCClient&) = delete;
    ProcessIPCClient& operator=(const ProcessIPCClient&) = delete;

    // Also creates the child's doorbell (an inheritable auto-reset event, `--ipc-wake`), rung after every
    // command written, so a child that waits for it serves the command at once instead of at its next
    // poll. Until 2026-10-18 the inject child polled every 250 ms (100 ms with a game) while the
    // controller's main thread waited for its ack: config reloads froze tray and hotkeys 250-313 ms.
    bool PrepareChildEndpoint(HANDLE& childEndpoint, std::wstring& childArguments);
    // The doorbell's handle for the spawn's inherited-handle list; nullptr without one.
    HANDLE ChildWakeHandle();
    bool CompleteChildSpawn(uint32_t childPid, DWORD timeoutMs = 5000);
    void Disconnect() override;
    bool SendCommand(ProcessCommand command, const char* payload = nullptr, ProcessResponse* response = nullptr,
                     DWORD timeoutMs = 1000) override;

    bool IsConnected() const override {
        return connected_.load(std::memory_order_acquire);
    }

private:
    bool ReadMessageWithTimeout(ProcessMessage& message, DWORD& bytesRead, DWORD timeoutMs, bool* timedOut = nullptr);
    bool WriteMessageWithTimeout(const ProcessMessage& message, DWORD timeoutMs);
    void BreakChannelLocked(const char* reason);

    ProcessMode targetMode_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE wake_ = nullptr;  // the child's doorbell
    std::atomic<bool> connected_{false};
    uint64_t sequence_ = 0;
    // Sequence of a tolerated command whose reply never arrived (0 = none). Its
    // reply, if it still comes, is discarded by the next command.
    uint64_t unansweredSequence_ = 0;
    uint32_t expectedChildPid_ = 0;
    ProcessChannelNonce nonce_{};
    std::mutex mutex_;
};

extern std::string g_SessionDirName;
extern std::string g_RecordingId;
std::string ParseSessionDir(LPCSTR commandLine);
std::string ParseRecordingId(LPCSTR commandLine);
// Handle to the controller's session A/V latency channel, inherited by a media child. nullptr
// when absent or malformed; see common/ipc/av_sync_latency_channel.h.
void* ParseInheritedLatencyChannelHandle();
// Handle to the per-child result slot (common/ipc/media_finalization.h) the spawner inherited to this
// media child; nullptr when absent or malformed.
void* ParseInheritedMediaResultHandle();
// The pure core of both parsers, for tests: the value of the one `<prefix><handle>` argument in
// `arguments` (index 0 is the program name), or nullptr when absent, repeated, malformed or invalid.
void* ParseInheritedHandleArgument(const wchar_t* const* arguments, int argumentCount, const wchar_t* prefix);

namespace ce::media_result {
class ResultSlot;
}

// configPath is active-code-page text used by the INI loader. A supplied executable
// is a resolved runtime helper, permitting hosts other than captureengine.exe. A media child given a
// `mediaResult` slot inherits it (`--media-result-handle=`) and reports its finalization outcome and
// output path through it; the caller keeps the slot for the life of the child and reads it after exit.
HANDLE SpawnChildProcess(ProcessMode mode, const char* configPath, ProcessIPCClient* ipcClient = nullptr,
                         const wchar_t* executable = nullptr,
                         const ce::media_result::ResultSlot* mediaResult = nullptr);
bool WaitForChildExit(HANDLE process, DWORD timeoutMs);
