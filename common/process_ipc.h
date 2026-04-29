#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

// Process modes - determined by command-line flag
enum class ProcessMode { Controller = 0, Inject = 1, Media = 2, Limiter = 3, Logger = 4, Sensors = 5 };

// Commands sent from Controller to child processes
enum class ProcessCommand : uint32_t {
    None = 0,
    Shutdown = 1,        // Graceful exit
    StartRecording = 2,  // Begin capture/encoding
    StopRecording = 3,   // End capture/encoding
    ReloadConfig = 4,    // Hot-reload config from disk
    Ping = 5             // Health check (expects Pong response)
};

// Responses from child processes to Controller
enum class ProcessResponse : uint32_t {
    None = 0,
    Ack = 1,    // Command acknowledged
    Pong = 2,   // Response to Ping
    Error = 3,  // Command failed
    RecordingStarted = 4,
    RecordingStopped = 5
};

// Status reported by child processes
struct ProcessStatus {
    bool alive;
    uint32_t uptimeMs;
    ProcessMode mode;
    bool isRecording;
    char lastError[128];
};

// Message header for named pipe communication
struct ProcessMessage {
    uint32_t magic;  // 0xCAPE for validation
    uint32_t size;   // Total message size including header
    union {
        ProcessCommand command;
        ProcessResponse response;
    };
    uint32_t sequence;  // For request/response matching
    char payload[256];  // Optional payload (config path, error message, etc.)
};

constexpr uint32_t PROCESS_MSG_MAGIC = 0x43415045;  // "CAPE"

// Named pipe names for each process type
constexpr const wchar_t* PIPE_NAME_INJECT = L"\\\\.\\pipe\\CaptureEngine_Inject";
constexpr const wchar_t* PIPE_NAME_MEDIA = L"\\\\.\\pipe\\CaptureEngine_Media";
constexpr const wchar_t* PIPE_NAME_LIMITER = L"\\\\.\\pipe\\CaptureEngine_Limiter";

// Get pipe name for a process mode
inline const wchar_t* GetPipeName(ProcessMode mode) {
    switch (mode) {
        case ProcessMode::Inject:
            return PIPE_NAME_INJECT;
        case ProcessMode::Media:
            return PIPE_NAME_MEDIA;
        case ProcessMode::Limiter:
            return PIPE_NAME_LIMITER;
        default:
            return nullptr;
    }
}

// Parse command-line to determine process mode
// Returns Controller if no --mode flag found
ProcessMode ParseProcessMode(int argc, char* argv[]);
ProcessMode ParseProcessMode(LPSTR lpCmdLine);

// Get log file name for a process mode
const char* GetLogFileName(ProcessMode mode);

// Server side (child processes) - listens for commands from controller
class ProcessIPCServer {
public:
    ProcessIPCServer(ProcessMode mode);
    ProcessIPCServer(ProcessMode mode, const wchar_t* pipeNameOverride);
    ~ProcessIPCServer();

    bool Init();
    void Shutdown();

    // Non-blocking check for incoming command
    // Returns true if a command was received
    bool PollCommand(ProcessCommand& outCmd, char* outPayload = nullptr, size_t payloadSize = 0);

    // Send response back to controller
    bool SendResponse(ProcessResponse response, const char* payload = nullptr);

    // Check if controller is connected
    bool IsConnected() const {
        return connected.load(std::memory_order_acquire);
    }

private:
    const wchar_t* ResolvePipeName() const;
    void ResetConnectOverlappedLocked();
    void HandlePipeDisconnectLocked(bool logDisconnect);

    ProcessMode mode;
    std::wstring pipeNameOverride;
    HANDLE hPipe;
    std::atomic<bool> connected;
    uint32_t lastSequence;
    HANDLE connectEvent;
    OVERLAPPED connectOverlapped;
    bool connectPending;
    mutable std::mutex stateMutex;
};

// Client side (controller) - sends commands to child processes
class ProcessIPCClient {
public:
    ProcessIPCClient(ProcessMode targetMode);
    ProcessIPCClient(ProcessMode targetMode, const wchar_t* pipeNameOverride);
    ~ProcessIPCClient();

    bool Connect(DWORD timeoutMs = 5000);
    void Disconnect();

    // Send command and optionally wait for response
    bool SendCommand(ProcessCommand cmd, const char* payload = nullptr, ProcessResponse* outResponse = nullptr,
                     DWORD timeoutMs = 1000);

    // Check connection status
    bool IsConnected() const {
        return hPipe != INVALID_HANDLE_VALUE;
    }

private:
    const wchar_t* ResolvePipeName() const;

    ProcessMode targetMode;
    std::wstring pipeNameOverride;
    HANDLE hPipe;
    uint32_t sequence;
};

// Global session directory name (e.g., "20260318_200247").
// Set by Controller at startup, parsed from --session-dir= by children.
extern std::string g_SessionDirName;

// Parse --session-dir=<name> from WinMain lpCmdLine string.
// Returns the session dir name or empty string if not present.
std::string ParseSessionDir(LPSTR lpCmdLine);

// Utility: Spawn a child process with specified mode
// Returns process handle on success, NULL on failure
HANDLE SpawnChildProcess(ProcessMode mode, const char* configPath);

// Utility: Wait for child process to exit (with timeout)
// Returns true if process exited, false on timeout
bool WaitForChildExit(HANDLE hProcess, DWORD timeoutMs);
