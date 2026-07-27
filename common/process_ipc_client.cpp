// The ProcessIPCClient endpoint and child-process spawning: anonymous-pipe
// endpoint preparation, the startup handshake, and process launch/teardown.
// Message validation and the server endpoint live in process_ipc.cpp.

#include "process_ipc_internal.h"

#include "logging.h"
#include "restricted_child_process.h"

#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

ProcessIPCClient::ProcessIPCClient(ProcessMode targetMode) : targetMode_(targetMode) {}

ProcessIPCClient::~ProcessIPCClient() {
    Disconnect();
}

bool ProcessIPCClient::PrepareChildEndpoint(HANDLE& childEndpoint, std::wstring& childArguments) {
    std::lock_guard<std::mutex> lock(mutex_);
    childEndpoint = INVALID_HANDLE_VALUE;
    childArguments.clear();
    if (!IsIpcMode(targetMode_) || !FillNonce(nonce_))
        return false;
    if (pipe_ != INVALID_HANDLE_VALUE)
        CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
    connected_.store(false, std::memory_order_release);
    sequence_ = 0;
    expectedChildPid_ = 0;

    PipeSecurity security;
    if (!BuildPipeSecurity(security))
        return false;
    const std::wstring nonceHex = NonceToHex(nonce_);
    wchar_t pipeName[256]{};
    _snwprintf_s(pipeName, std::size(pipeName), _TRUNCATE, L"\\\\.\\pipe\\CE_%08X_%016llX_%ls", GetCurrentProcessId(),
                 static_cast<unsigned long long>(g_pipeNameSequence.fetch_add(1, std::memory_order_relaxed)),
                 nonceHex.c_str());
    pipe_ = CreateNamedPipeW(pipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                             PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                             sizeof(ProcessMessage), sizeof(ProcessMessage), 0, &security.attributes);
    if (pipe_ == INVALID_HANDLE_VALUE)
        return false;

    OVERLAPPED connectOverlapped{};
    connectOverlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!connectOverlapped.hEvent) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
        return false;
    }
    BOOL connectStarted = ConnectNamedPipe(pipe_, &connectOverlapped);
    const DWORD connectError = connectStarted ? ERROR_SUCCESS : GetLastError();
    if (!connectStarted && connectError != ERROR_IO_PENDING && connectError != ERROR_PIPE_CONNECTED) {
        CloseHandle(connectOverlapped.hEvent);
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
        return false;
    }

    childEndpoint =
        CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD childReadMode = PIPE_READMODE_MESSAGE;
    if (childEndpoint == INVALID_HANDLE_VALUE ||
        !SetNamedPipeHandleState(childEndpoint, &childReadMode, nullptr, nullptr) ||
        !SetHandleInformation(childEndpoint, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
        if (childEndpoint != INVALID_HANDLE_VALUE)
            CloseHandle(childEndpoint);
        childEndpoint = INVALID_HANDLE_VALUE;
        CancelOverlapped(pipe_, connectOverlapped);
        CloseHandle(connectOverlapped.hEvent);
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
        return false;
    }
    bool connected = connectStarted || connectError == ERROR_PIPE_CONNECTED;
    if (!connected && WaitForSingleObject(connectOverlapped.hEvent, 5000) == WAIT_OBJECT_0) {
        DWORD ignored = 0;
        connected = GetOverlappedResult(pipe_, &connectOverlapped, &ignored, FALSE) != FALSE;
    }
    if (!connected) {
        CancelOverlapped(pipe_, connectOverlapped);
        CloseHandle(childEndpoint);
        childEndpoint = INVALID_HANDLE_VALUE;
        CloseHandle(connectOverlapped.hEvent);
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
        return false;
    }
    CloseHandle(connectOverlapped.hEvent);

    wchar_t arguments[320]{};
    _snwprintf_s(arguments, std::size(arguments), _TRUNCATE,
                 L"--ipc-handle=0x%llX --ipc-controller-pid=%lu --ipc-nonce=%ls",
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(childEndpoint)),
                 static_cast<unsigned long>(GetCurrentProcessId()), nonceHex.c_str());
    childArguments = arguments;
    return true;
}

bool ProcessIPCClient::ReadMessageWithTimeout(ProcessMessage& message, DWORD& bytesRead, DWORD timeoutMs) {
    message = {};
    bytesRead = 0;
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent)
        return false;
    BOOL read = ReadFile(pipe_, &message, sizeof(message), &bytesRead, &overlapped);
    if (!read && GetLastError() == ERROR_IO_PENDING) {
        const DWORD waitResult = WaitForSingleObject(overlapped.hEvent, timeoutMs);
        if (waitResult == WAIT_OBJECT_0)
            read = GetOverlappedResult(pipe_, &overlapped, &bytesRead, FALSE);
        else
            CancelOverlapped(pipe_, overlapped);
    }
    CloseHandle(overlapped.hEvent);
    return read != FALSE;
}

bool ProcessIPCClient::WriteMessageWithTimeout(const ProcessMessage& message, DWORD timeoutMs) {
    DWORD bytesWritten = 0;
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent)
        return false;
    BOOL written = WriteFile(pipe_, &message, message.totalSize, &bytesWritten, &overlapped);
    if (!written && GetLastError() == ERROR_IO_PENDING) {
        const DWORD waitResult = WaitForSingleObject(overlapped.hEvent, timeoutMs);
        if (waitResult == WAIT_OBJECT_0)
            written = GetOverlappedResult(pipe_, &overlapped, &bytesWritten, FALSE);
        else
            CancelOverlapped(pipe_, overlapped);
    }
    CloseHandle(overlapped.hEvent);
    return written != FALSE && bytesWritten == message.totalSize;
}

bool ProcessIPCClient::CompleteChildSpawn(uint32_t childPid, DWORD timeoutMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pipe_ == INVALID_HANDLE_VALUE || childPid == 0)
        return false;
    expectedChildPid_ = childPid;

    ProcessMessage handshake{};
    DWORD bytesRead = 0;
    if (!ReadMessageWithTimeout(handshake, bytesRead, timeoutMs) ||
        !ValidateProcessMessage(handshake, bytesRead, ProcessMessageKind::Startup, targetMode_, childPid, nonce_, 0,
                                true)) {
        LogError("[IPC] Invalid or missing startup handshake from %s PID %lu", ModeName(targetMode_),
                 static_cast<unsigned long>(childPid));
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
        return false;
    }
    connected_.store(true, std::memory_order_release);
    LogInfo("[IPC] Authenticated inherited %s endpoint (PID %lu)", ModeName(targetMode_),
            static_cast<unsigned long>(childPid));
    return true;
}

void ProcessIPCClient::Disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_.store(false, std::memory_order_release);
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(pipe_, nullptr);
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    expectedChildPid_ = 0;
}

bool ProcessIPCClient::SendCommand(ProcessCommand command, const char* payload, ProcessResponse* response,
                                   DWORD timeoutMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_.load(std::memory_order_acquire) || pipe_ == INVALID_HANDLE_VALUE ||
        !IsValidCommand(static_cast<uint16_t>(command)) ||
        (payload && strnlen(payload, PROCESS_MAX_PAYLOAD) == PROCESS_MAX_PAYLOAD)) {
        return false;
    }
    if (sequence_ == std::numeric_limits<uint64_t>::max()) {
        connected_.store(false, std::memory_order_release);
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
        return false;
    }
    const uint64_t sequence = ++sequence_;
    const ProcessMessage request =
        BuildMessage(ProcessMessageKind::Command, static_cast<uint16_t>(command), ProcessMode::Controller, sequence,
                     GetCurrentProcessId(), nonce_, payload);
    if (!ValidateOpcodePayload(request))
        return false;
    ProcessMessage reply{};
    DWORD bytesRead = 0;
    if (!WriteMessageWithTimeout(request, timeoutMs) || !ReadMessageWithTimeout(reply, bytesRead, timeoutMs) ||
        !ValidateProcessMessage(reply, bytesRead, ProcessMessageKind::Response, targetMode_, expectedChildPid_, nonce_,
                                sequence, true) ||
        !IsResponseAllowed(command, static_cast<ProcessResponse>(reply.opcode))) {
        connected_.store(false, std::memory_order_release);
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
        LogWarn("[IPC] %s command channel broke; a fresh child is required", ModeName(targetMode_));
        return false;
    }
    if (response)
        *response = static_cast<ProcessResponse>(reply.opcode);
    return true;
}

HANDLE SpawnChildProcess(ProcessMode mode, const char* configPath, ProcessIPCClient* ipcClient) {
    if (IsIpcMode(mode) && !ipcClient) {
        LogError("[Spawn] Missing IPC client for %s child", ModeName(mode));
        return nullptr;
    }

    std::wstring executablePath(32768, L'\0');
    const DWORD executableLength =
        GetModuleFileNameW(nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    if (executableLength == 0 || executableLength >= executablePath.size())
        return nullptr;
    executablePath.resize(executableLength);

    HANDLE childEndpoint = INVALID_HANDLE_VALUE;
    std::wstring ipcArguments;
    std::vector<HANDLE> inheritedHandles;
    if (ipcClient) {
        if (!ipcClient->PrepareChildEndpoint(childEndpoint, ipcArguments)) {
            LogError("[Spawn] Failed to create inherited %s IPC endpoint", ModeName(mode));
            return nullptr;
        }
        inheritedHandles.push_back(childEndpoint);
    }

    std::wstring commandLine = QuoteCommandLineArgument(executablePath) + L" --mode=" + ModeNameWide(mode);
    if (configPath && *configPath) {
        const std::wstring wideConfig = Utf8ToWide(configPath);
        if (wideConfig.empty()) {
            if (childEndpoint != INVALID_HANDLE_VALUE)
                CloseHandle(childEndpoint);
            if (ipcClient)
                ipcClient->Disconnect();
            return nullptr;
        }
        commandLine += L" --config=" + QuoteCommandLineArgument(wideConfig);
    }
    if (mode == ProcessMode::Logger || mode == ProcessMode::Sensors)
        commandLine += L" --parent-pid=" + std::to_wstring(GetCurrentProcessId());
    if (!g_SessionDirName.empty())
        commandLine += L" --session-dir=" + QuoteCommandLineArgument(Utf8ToWide(g_SessionDirName.c_str()));
    if (mode == ProcessMode::Media && !g_RecordingId.empty())
        commandLine += L" --recording-id=" + QuoteCommandLineArgument(Utf8ToWide(g_RecordingId.c_str()));
    if (!ipcArguments.empty())
        commandLine += L" " + ipcArguments;

    ce::process::RestrictedChildProcess child;
    DWORD error = ERROR_SUCCESS;
    const bool launched = ce::process::LaunchRestrictedChildProcess(
        executablePath, commandLine, std::filesystem::path(executablePath).parent_path().wstring(), inheritedHandles,
        CREATE_NO_WINDOW, child, error);
    if (childEndpoint != INVALID_HANDLE_VALUE) {
        SetHandleInformation(childEndpoint, HANDLE_FLAG_INHERIT, 0);
        CloseHandle(childEndpoint);
    }
    if (!launched) {
        if (ipcClient)
            ipcClient->Disconnect();
        LogError("[Spawn] Failed to create %s process: %lu", ModeName(mode), static_cast<unsigned long>(error));
        return nullptr;
    }

    if (ipcClient && !ipcClient->CompleteChildSpawn(child.processId, 5000)) {
        ipcClient->Disconnect();
        if (WaitForSingleObject(child.processHandle, 2000) != WAIT_OBJECT_0)
            TerminateProcess(child.processHandle, ERROR_ACCESS_DENIED);
        LogError("[Spawn] %s child failed inherited-channel authentication", ModeName(mode));
        return nullptr;
    }

    LogInfo("[Spawn] Launched %s process (PID: %lu)", ModeName(mode), static_cast<unsigned long>(child.processId));
    HANDLE process = child.processHandle;
    child.processHandle = nullptr;
    return process;
}

bool WaitForChildExit(HANDLE process, DWORD timeoutMs) {
    return !process || WaitForSingleObject(process, timeoutMs) == WAIT_OBJECT_0;
}
