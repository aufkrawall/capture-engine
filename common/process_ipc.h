#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

enum class ProcessMode : uint32_t { Controller = 0, Inject = 1, Media = 2, Limiter = 3, Logger = 4, Sensors = 5 };

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
ProcessMode ParseProcessMode(LPSTR commandLine);
const char* GetLogFileName(ProcessMode mode);
std::string GetProcessLogFileName(ProcessMode mode, const std::string& recordingId, uint32_t processId);

class ProcessIPCServer {
public:
    explicit ProcessIPCServer(ProcessMode mode);
    ~ProcessIPCServer();

    ProcessIPCServer(const ProcessIPCServer&) = delete;
    ProcessIPCServer& operator=(const ProcessIPCServer&) = delete;

    bool Init();
    void Shutdown();
    bool PollCommand(ProcessCommand& command, char* payload = nullptr, size_t payloadSize = 0);
    bool SendResponse(ProcessResponse response, const char* payload = nullptr);

    bool IsConnected() const {
        return connected_.load(std::memory_order_acquire);
    }
    bool HasFatalDisconnect() const {
        return fatalDisconnect_.load(std::memory_order_acquire);
    }
    // Controller PID from the startup arguments, accepted only after it matched the
    // authenticated pipe server process. 0 until ReadStartupArguments has validated it.
    uint32_t ControllerPid() const {
        return controllerPid_;
    }

private:
    bool ReadStartupArguments();
    bool SendStartupHandshake();
    void MarkDisconnected(DWORD error, const char* operation);

    ProcessMode mode_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::atomic<bool> connected_{false};
    std::atomic<bool> fatalDisconnect_{false};
    uint64_t lastSequence_ = 0;
    uint32_t controllerPid_ = 0;
    ProcessChannelNonce nonce_{};
    std::mutex mutex_;
};

class ProcessIPCClient {
public:
    explicit ProcessIPCClient(ProcessMode targetMode);
    ~ProcessIPCClient();

    ProcessIPCClient(const ProcessIPCClient&) = delete;
    ProcessIPCClient& operator=(const ProcessIPCClient&) = delete;

    bool PrepareChildEndpoint(HANDLE& childEndpoint, std::wstring& childArguments);
    bool CompleteChildSpawn(uint32_t childPid, DWORD timeoutMs = 5000);
    void Disconnect();
    bool SendCommand(ProcessCommand command, const char* payload = nullptr, ProcessResponse* response = nullptr,
                     DWORD timeoutMs = 1000);

    bool IsConnected() const {
        return connected_.load(std::memory_order_acquire);
    }

private:
    bool ReadMessageWithTimeout(ProcessMessage& message, DWORD& bytesRead, DWORD timeoutMs);
    bool WriteMessageWithTimeout(const ProcessMessage& message, DWORD timeoutMs);

    ProcessMode targetMode_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::atomic<bool> connected_{false};
    uint64_t sequence_ = 0;
    uint32_t expectedChildPid_ = 0;
    ProcessChannelNonce nonce_{};
    std::mutex mutex_;
};

extern std::string g_SessionDirName;
extern std::string g_RecordingId;
std::string ParseSessionDir(LPSTR commandLine);
std::string ParseRecordingId(LPSTR commandLine);

HANDLE SpawnChildProcess(ProcessMode mode, const char* configPath, ProcessIPCClient* ipcClient = nullptr);
bool WaitForChildExit(HANDLE process, DWORD timeoutMs);
