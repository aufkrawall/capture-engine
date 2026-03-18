#include "process_ipc.h"
#include <cstring>
#include "logging.h"
#include "raii_helpers.h"

std::string g_SessionDirName;

namespace {

void CancelAndDrainOverlapped(HANDLE handle, OVERLAPPED* overlapped, HANDLE eventHandle, DWORD waitMs) {
    if (handle == INVALID_HANDLE_VALUE || !overlapped)
        return;

    if (!CancelIoEx(handle, overlapped)) {
        DWORD cancelError = GetLastError();
        if (cancelError != ERROR_NOT_FOUND && cancelError != ERROR_INVALID_HANDLE) {
            LogError("[IPC] CancelIoEx failed: %d", cancelError);
        }
    }

    HANDLE waitHandle = eventHandle ? eventHandle : overlapped->hEvent;
    if (!waitHandle)
        return;

    DWORD waitResult = WaitForSingleObject(waitHandle, waitMs);
    if (waitResult == WAIT_OBJECT_0) {
        DWORD bytesTransferred = 0;
        GetOverlappedResult(handle, overlapped, &bytesTransferred, FALSE);
    }
}

}  // namespace

// Parse --mode= from command line
ProcessMode ParseProcessMode(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--mode=", 7) == 0) {
            const char* mode = argv[i] + 7;
            if (strcmp(mode, "inject") == 0)
                return ProcessMode::Inject;
            if (strcmp(mode, "media") == 0)
                return ProcessMode::Media;
            if (strcmp(mode, "limiter") == 0)
                return ProcessMode::Limiter;
            if (strcmp(mode, "logger") == 0)
                return ProcessMode::Logger;
            if (strcmp(mode, "sensors") == 0)
                return ProcessMode::Sensors;
        }
    }
    return ProcessMode::Controller;
}

ProcessMode ParseProcessMode(LPSTR lpCmdLine) {
    if (!lpCmdLine || lpCmdLine[0] == '\0')
        return ProcessMode::Controller;

    // Simple search for --mode=
    const char* modeStr = strstr(lpCmdLine, "--mode=");
    if (!modeStr)
        return ProcessMode::Controller;

    modeStr += 7;  // Skip "--mode="
    if (strncmp(modeStr, "inject", 6) == 0)
        return ProcessMode::Inject;
    if (strncmp(modeStr, "media", 5) == 0)
        return ProcessMode::Media;
    if (strncmp(modeStr, "limiter", 7) == 0)
        return ProcessMode::Limiter;
    if (strncmp(modeStr, "logger", 6) == 0)
        return ProcessMode::Logger;
    if (strncmp(modeStr, "sensors", 7) == 0)
        return ProcessMode::Sensors;

    return ProcessMode::Controller;
}

std::string ParseSessionDir(LPSTR lpCmdLine) {
    if (!lpCmdLine || lpCmdLine[0] == '\0')
        return {};
    const char* ptr = strstr(lpCmdLine, "--session-dir=");
    if (!ptr)
        return {};
    ptr += 14;  // Skip "--session-dir="
    const char* end = ptr;
    while (*end && *end != ' ' && *end != '\t')
        end++;
    return std::string(ptr, end);
}

const char* GetLogFileName(ProcessMode mode) {
    switch (mode) {
        case ProcessMode::Inject:
            return "inject.log";
        case ProcessMode::Media:
            return "media.log";
        case ProcessMode::Limiter:
            return "limiter.log";
        case ProcessMode::Logger:
            return "logger.log";
        case ProcessMode::Sensors:
            return "sensors.log";
        case ProcessMode::Controller:
        default:
            return "captureengine.log";
    }
}

// ============================================================================
// ProcessIPCServer Implementation (Child processes)
// ============================================================================

ProcessIPCServer::ProcessIPCServer(ProcessMode mode)
    : mode(mode),
      hPipe(INVALID_HANDLE_VALUE),
      connected(false),
      lastSequence(0),
      connectEvent(NULL),
      connectOverlapped{},
      connectPending(false) {}

ProcessIPCServer::~ProcessIPCServer() {
    Shutdown();
}

void ProcessIPCServer::ResetConnectOverlappedLocked() {
    connectPending = false;
    connectOverlapped = {};
    connectOverlapped.hEvent = connectEvent;
}

void ProcessIPCServer::HandlePipeDisconnectLocked(bool logDisconnect) {
    if (logDisconnect) {
        LogInfo("[IPC] Controller disconnected");
    }

    connected.store(false, std::memory_order_release);
    if (hPipe != INVALID_HANDLE_VALUE) {
        if (connectPending) {
            CancelAndDrainOverlapped(hPipe, &connectOverlapped, connectEvent, 50);
        }
        DisconnectNamedPipe(hPipe);
    }

    ResetConnectOverlappedLocked();
}

bool ProcessIPCServer::Init() {
    std::lock_guard<std::mutex> lock(stateMutex);

    if (hPipe != INVALID_HANDLE_VALUE)
        return true;

    const wchar_t* pipeName = GetPipeName(mode);
    if (!pipeName) {
        LogError("[IPC] Invalid process mode for server");
        return false;
    }

    // Create named pipe server
    hPipe = CreateNamedPipeW(pipeName,
                             PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,  // Async for non-blocking
                             PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                             1,                       // Max instances
                             sizeof(ProcessMessage),  // Output buffer
                             sizeof(ProcessMessage),  // Input buffer
                             0,                       // Default timeout
                             NULL                     // Default security
    );

    if (hPipe == INVALID_HANDLE_VALUE) {
        LogError("[IPC] Failed to create pipe: %d", GetLastError());
        return false;
    }

    connectEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!connectEvent) {
        LogError("[IPC] Failed to create connect event: %d", GetLastError());
        CloseHandle(hPipe);
        hPipe = INVALID_HANDLE_VALUE;
        return false;
    }

    connectOverlapped = {};
    connectOverlapped.hEvent = connectEvent;

    LogInfo("[IPC] Server pipe created, waiting for connection...");
    return true;
}

void ProcessIPCServer::Shutdown() {
    std::lock_guard<std::mutex> lock(stateMutex);

    if (hPipe != INVALID_HANDLE_VALUE && connectPending) {
        CancelAndDrainOverlapped(hPipe, &connectOverlapped, connectEvent, 50);
    }
    connectPending = false;
    connected.store(false, std::memory_order_release);
    if (hPipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
        hPipe = INVALID_HANDLE_VALUE;
    }
    if (connectEvent) {
        CloseHandle(connectEvent);
        connectEvent = NULL;
    }
    connectOverlapped = {};
    connected = false;
    lastSequence = 0;
}

bool ProcessIPCServer::PollCommand(ProcessCommand& outCmd, char* outPayload, size_t payloadSize) {
    std::lock_guard<std::mutex> lock(stateMutex);

    if (hPipe == INVALID_HANDLE_VALUE)
        return false;

    // If not connected, try to accept connection (non-blocking)
    if (!connected.load(std::memory_order_acquire)) {
        if (!connectPending) {
            ResetEvent(connectEvent);
            ResetConnectOverlappedLocked();

            BOOL connectResult = ConnectNamedPipe(hPipe, &connectOverlapped);
            if (connectResult) {
                connected.store(true, std::memory_order_release);
                LogInfo("[IPC] Controller connected");
            } else {
                DWORD connectError = GetLastError();
                if (connectError == ERROR_PIPE_CONNECTED) {
                    SetEvent(connectEvent);
                    connected.store(true, std::memory_order_release);
                    LogInfo("[IPC] Controller already connected");
                } else if (connectError == ERROR_IO_PENDING) {
                    connectPending = true;
                    return false;
                } else {
                    LogError("[IPC] ConnectNamedPipe failed: %d", connectError);
                    return false;
                }
            }
        }

        if (connectPending) {
            DWORD waitResult = WaitForSingleObject(connectEvent, 0);
            if (waitResult == WAIT_TIMEOUT) {
                return false;
            }
            if (waitResult != WAIT_OBJECT_0) {
                LogError("[IPC] WaitForSingleObject on connect event failed: %d", GetLastError());
                CancelAndDrainOverlapped(hPipe, &connectOverlapped, connectEvent, 50);
                ResetConnectOverlappedLocked();
                return false;
            }

            DWORD bytesTransferred = 0;
            if (!GetOverlappedResult(hPipe, &connectOverlapped, &bytesTransferred, FALSE)) {
                DWORD connectError = GetLastError();
                if (connectError != ERROR_PIPE_CONNECTED) {
                    if (connectError == ERROR_BROKEN_PIPE || connectError == ERROR_NO_DATA) {
                        DisconnectNamedPipe(hPipe);
                    } else if (connectError == ERROR_OPERATION_ABORTED) {
                        ResetConnectOverlappedLocked();
                        return false;
                    } else {
                        LogError("[IPC] ConnectNamedPipe completion failed: %d", connectError);
                    }
                    ResetConnectOverlappedLocked();
                    return false;
                }
            }

            ResetConnectOverlappedLocked();
            connected.store(true, std::memory_order_release);
            LogInfo("[IPC] Controller connected");
        }
    }

    // Check for incoming message (non-blocking)
    DWORD bytesAvailable = 0;
    if (!PeekNamedPipe(hPipe, NULL, 0, NULL, &bytesAvailable, NULL)) {
        DWORD pipeError = GetLastError();
        if (pipeError == ERROR_BROKEN_PIPE || pipeError == ERROR_NO_DATA) {
            HandlePipeDisconnectLocked(true);
        }
        return false;
    }

    if (bytesAvailable == 0)
        return false;

    // Read message
    ProcessMessage msg = {};
    DWORD bytesRead = 0;
    if (!ReadFile(hPipe, &msg, sizeof(msg), &bytesRead, NULL)) {
        DWORD readError = GetLastError();
        if (readError == ERROR_BROKEN_PIPE || readError == ERROR_NO_DATA) {
            HandlePipeDisconnectLocked(true);
        } else {
            LogError("[IPC] ReadFile failed: %d", readError);
        }
        return false;
    }

    // Validate
    if (msg.magic != PROCESS_MSG_MAGIC) {
        LogError("[IPC] Invalid message magic: 0x%08X", msg.magic);
        return false;
    }

    lastSequence = msg.sequence;
    outCmd = msg.command;

    if (outPayload && payloadSize > 0) {
        size_t copyLen = payloadSize - 1;
        if (copyLen > sizeof(msg.payload))
            copyLen = sizeof(msg.payload);
        memcpy(outPayload, msg.payload, copyLen);
        outPayload[copyLen] = '\0';
    }

    return true;
}

bool ProcessIPCServer::SendResponse(ProcessResponse response, const char* payload) {
    std::lock_guard<std::mutex> lock(stateMutex);

    if (!connected.load(std::memory_order_acquire) || hPipe == INVALID_HANDLE_VALUE)
        return false;

    ProcessMessage msg = {};
    msg.magic = PROCESS_MSG_MAGIC;
    msg.size = sizeof(msg);
    msg.response = response;
    msg.sequence = lastSequence;

    if (payload) {
        strncpy(msg.payload, payload, sizeof(msg.payload) - 1);
    }

    DWORD bytesWritten = 0;
    if (!WriteFile(hPipe, &msg, sizeof(msg), &bytesWritten, NULL)) {
        DWORD writeError = GetLastError();
        if (writeError == ERROR_BROKEN_PIPE || writeError == ERROR_NO_DATA) {
            HandlePipeDisconnectLocked(true);
        } else {
            LogError("[IPC] WriteFile failed: %d", writeError);
        }
        return false;
    }

    return true;
}

// ============================================================================
// ProcessIPCClient Implementation (Controller)
// ============================================================================

ProcessIPCClient::ProcessIPCClient(ProcessMode targetMode)
    : targetMode(targetMode),
      hPipe(INVALID_HANDLE_VALUE),
      sequence(0) {}

ProcessIPCClient::~ProcessIPCClient() {
    Disconnect();
}

bool ProcessIPCClient::Connect(DWORD timeoutMs) {
    if (hPipe != INVALID_HANDLE_VALUE)
        return true;

    const wchar_t* pipeName = GetPipeName(targetMode);
    if (!pipeName) {
        LogError("[IPC] Invalid target mode for client");
        return false;
    }

    // Wait for pipe to become available
    if (!WaitNamedPipeW(pipeName, timeoutMs)) {
        // Pipe not available yet
        return false;
    }

    // Open pipe with FILE_FLAG_OVERLAPPED for async I/O support
    hPipe = CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

    if (hPipe == INVALID_HANDLE_VALUE) {
        LogError("[IPC] Failed to connect to pipe: %d", GetLastError());
        return false;
    }

    // Set message mode
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(hPipe, &mode, NULL, NULL)) {
        LogError("[IPC] Failed to set pipe message mode: %d", GetLastError());
        Disconnect();
        return false;
    }

    LogInfo("[IPC] Connected to %s process", targetMode == ProcessMode::Inject    ? "inject"
                                             : targetMode == ProcessMode::Media   ? "media"
                                             : targetMode == ProcessMode::Limiter ? "limiter"
                                                                                  : "unknown");
    return true;
}

void ProcessIPCClient::Disconnect() {
    if (hPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(hPipe);
        hPipe = INVALID_HANDLE_VALUE;
    }
}

bool ProcessIPCClient::SendCommand(ProcessCommand cmd, const char* payload, ProcessResponse* outResponse,
                                   DWORD timeoutMs) {
    // Try to reconnect if pipe is closed
    if (hPipe == INVALID_HANDLE_VALUE) {
        if (!Connect(1000)) {
            return false;
        }
    }

    // Build message
    ProcessMessage msg = {};
    msg.magic = PROCESS_MSG_MAGIC;
    msg.size = sizeof(msg);
    msg.command = cmd;
    msg.sequence = ++sequence;

    if (payload) {
        strncpy(msg.payload, payload, sizeof(msg.payload) - 1);
    }

    // Send with overlapped I/O
    DWORD bytesWritten = 0;
    OVERLAPPED ovWrite = {};
    ce::HandleGuard writeEvent(CreateEvent(NULL, TRUE, FALSE, NULL));
    ovWrite.hEvent = writeEvent.get();

    BOOL writeResult = WriteFile(hPipe, &msg, sizeof(msg), &bytesWritten, &ovWrite);
    if (!writeResult && GetLastError() == ERROR_IO_PENDING) {
        // Wait for write to complete
        DWORD waitResult = WaitForSingleObject(ovWrite.hEvent, timeoutMs);
        if (waitResult == WAIT_OBJECT_0) {
            if (!GetOverlappedResult(hPipe, &ovWrite, &bytesWritten, FALSE)) {
                LogError("[IPC] SendCommand WriteFile completion failed: %d - disconnecting", GetLastError());
                Disconnect();
                return false;
            }
            writeResult = TRUE;
        } else {
            CancelAndDrainOverlapped(hPipe, &ovWrite, ovWrite.hEvent, 50);
            LogError("[IPC] SendCommand WriteFile timeout - disconnecting for reconnect");
            Disconnect();  // Close and reconnect on next attempt
            return false;
        }
    }

    if (!writeResult) {
        LogError("[IPC] SendCommand WriteFile failed: %d - disconnecting", GetLastError());
        Disconnect();
        return false;
    }

    // Always read response to drain the pipe buffer (server always sends one).
    // If callers skip reading for fire-and-forget commands (outResponse==nullptr),
    // unread responses accumulate and corrupt sequence matching for later calls.
    ProcessMessage resp = {};
    DWORD bytesRead = 0;

    OVERLAPPED ovRead = {};
    ce::HandleGuard readEvent(CreateEvent(NULL, TRUE, FALSE, NULL));
    ovRead.hEvent = readEvent.get();

    BOOL readResult = ReadFile(hPipe, &resp, sizeof(resp), &bytesRead, &ovRead);
    if (!readResult && GetLastError() == ERROR_IO_PENDING) {
        DWORD waitResult = WaitForSingleObject(ovRead.hEvent, timeoutMs);
        if (waitResult == WAIT_OBJECT_0) {
            if (!GetOverlappedResult(hPipe, &ovRead, &bytesRead, FALSE)) {
                LogError("[IPC] SendCommand read completion failed: %d - disconnecting", GetLastError());
                Disconnect();
                return false;
            }
            readResult = TRUE;
        } else {
            CancelAndDrainOverlapped(hPipe, &ovRead, ovRead.hEvent, 50);
            LogError("[IPC] SendCommand read timeout - disconnecting for reconnect");
            Disconnect();  // Close and reconnect on next attempt
            return false;
        }
    }

    if (outResponse) {
        if (readResult && resp.magic == PROCESS_MSG_MAGIC && resp.sequence == sequence) {
            *outResponse = resp.response;
            return true;
        }
        LogError("[IPC] SendCommand invalid response: magic=0x%X seq=%u (expected %u)", resp.magic, resp.sequence,
                 sequence);
        return false;
    }

    return readResult ? true : false;
}

// ============================================================================
// Process Spawning Utilities
// ============================================================================

HANDLE SpawnChildProcess(ProcessMode mode, const char* configPath) {
    // Get our own executable path
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);

    // Build command line with mode flag
    char cmdLine[MAX_PATH * 2];
    const char* modeStr = "controller";
    if (mode == ProcessMode::Inject)
        modeStr = "inject";
    else if (mode == ProcessMode::Media)
        modeStr = "media";
    else if (mode == ProcessMode::Limiter)
        modeStr = "limiter";
    else if (mode == ProcessMode::Logger)
        modeStr = "logger";
    else if (mode == ProcessMode::Sensors)
        modeStr = "sensors";

    if (configPath && configPath[0]) {
        snprintf(cmdLine, sizeof(cmdLine), "\"%s\" --mode=%s --config=\"%s\"", exePath, modeStr, configPath);
    } else {
        snprintf(cmdLine, sizeof(cmdLine), "\"%s\" --mode=%s", exePath, modeStr);
    }

    // Logger and Sensors need to know the controller PID for shutdown signaling
    if (mode == ProcessMode::Logger || mode == ProcessMode::Sensors) {
        size_t len = strlen(cmdLine);
        snprintf(cmdLine + len, sizeof(cmdLine) - len, " --parent-pid=%u", GetCurrentProcessId());
    }

    // Pass session directory name so children log into the same session folder
    if (!g_SessionDirName.empty()) {
        size_t len = strlen(cmdLine);
        snprintf(cmdLine + len, sizeof(cmdLine) - len, " --session-dir=%s", g_SessionDirName.c_str());
    }

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(NULL, cmdLine, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW,  // Hidden window for child processes
                        NULL, NULL, &si, &pi)) {
        LogError("[Spawn] Failed to create %s process: %d", modeStr, GetLastError());
        return NULL;
    }

    LogInfo("[Spawn] Launched %s process (PID: %d)", modeStr, pi.dwProcessId);
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

bool WaitForChildExit(HANDLE hProcess, DWORD timeoutMs) {
    if (!hProcess)
        return true;
    DWORD result = WaitForSingleObject(hProcess, timeoutMs);
    return (result == WAIT_OBJECT_0);
}
