// IPC message validation, command-line parsing and the ProcessIPCServer
// endpoint. ProcessIPCClient and child-process spawning live in
// process_ipc_client.cpp; helpers shared by both are declared in
// process_ipc_internal.h.

#include "process_ipc.h"

#include "logging.h"
#include "restricted_child_process.h"

#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include "process_ipc_internal.h"

std::string g_SessionDirName;
std::string g_RecordingId;

static std::atomic<uint32_t> g_invalidMessageLogCount{0};
std::atomic<uint64_t> g_pipeNameSequence{1};

static void LogInvalidMessage(const char* format, ...) {
    if (g_invalidMessageLogCount.fetch_add(1, std::memory_order_relaxed) >= 32)
        return;
    char message[512]{};
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    LogWarn("[IPC] Rejected message: %s", message);
}

static bool IsValidMode(ProcessMode mode) {
    return mode >= ProcessMode::Controller && mode <= ProcessMode::Sensors;
}

bool IsValidCommand(uint16_t opcode) {
    return opcode >= static_cast<uint16_t>(ProcessCommand::Shutdown) &&
           opcode <= static_cast<uint16_t>(ProcessCommand::ToggleOverlay);
}

static bool IsValidResponse(uint16_t opcode) {
    return opcode >= static_cast<uint16_t>(ProcessResponse::Ack) &&
           opcode <= static_cast<uint16_t>(ProcessResponse::RecordingStopped);
}

static bool ValidatePayload(const ProcessMessage& message) {
    if (message.payloadSize > PROCESS_MAX_PAYLOAD)
        return false;
    if (message.payloadSize == 0)
        return true;
    if (message.payload[message.payloadSize - 1] != '\0')
        return false;
    return strnlen(message.payload, message.payloadSize) == message.payloadSize - 1;
}

bool ValidateOpcodePayload(const ProcessMessage& message) {
    if (message.kind == ProcessMessageKind::Startup)
        return message.payloadSize == 0;
    if (message.kind == ProcessMessageKind::Command) {
        if (message.opcode == static_cast<uint16_t>(ProcessCommand::StartRecording)) {
            return message.payloadSize == 0 ||
                   (message.payloadSize == sizeof("audio_only") && strcmp(message.payload, "audio_only") == 0);
        }
        return message.payloadSize == 0;
    }
    if (message.kind == ProcessMessageKind::Response) {
        return message.opcode == static_cast<uint16_t>(ProcessResponse::Error) || message.payloadSize == 0;
    }
    return false;
}

bool IsResponseAllowed(ProcessCommand command, ProcessResponse response) {
    if (response == ProcessResponse::Error)
        return true;
    switch (command) {
        case ProcessCommand::Shutdown:
        case ProcessCommand::ReloadConfig:
        case ProcessCommand::ToggleOverlay:
            return response == ProcessResponse::Ack;
        case ProcessCommand::StartRecording:
            return response == ProcessResponse::Ack || response == ProcessResponse::RecordingStarted;
        case ProcessCommand::StopRecording:
            return response == ProcessResponse::Ack || response == ProcessResponse::RecordingStopped;
        case ProcessCommand::Ping:
            return response == ProcessResponse::Pong;
        default:
            return false;
    }
}

bool IsIpcMode(ProcessMode mode) {
    return mode == ProcessMode::Inject || mode == ProcessMode::Media || mode == ProcessMode::Limiter;
}

const wchar_t* ModeNameWide(ProcessMode mode) {
    switch (mode) {
        case ProcessMode::Inject:
            return L"inject";
        case ProcessMode::Media:
            return L"media";
        case ProcessMode::Limiter:
            return L"limiter";
        case ProcessMode::Logger:
            return L"logger";
        case ProcessMode::Sensors:
            return L"sensors";
        default:
            return L"controller";
    }
}

const char* ModeName(ProcessMode mode) {
    switch (mode) {
        case ProcessMode::Inject:
            return "inject";
        case ProcessMode::Media:
            return "media";
        case ProcessMode::Limiter:
            return "limiter";
        case ProcessMode::Logger:
            return "logger";
        case ProcessMode::Sensors:
            return "sensors";
        default:
            return "controller";
    }
}

std::wstring Utf8ToWide(const char* text) {
    if (!text || !*text)
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (length <= 1)
        return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, result.data(), length) == 0)
        return {};
    result.resize(static_cast<size_t>(length - 1));
    return result;
}

std::wstring QuoteCommandLineArgument(const std::wstring& argument) {
    if (argument.empty())
        return L"\"\"";
    if (argument.find_first_of(L" \t\"") == std::wstring::npos)
        return argument;
    std::wstring quoted(1, L'\"');
    size_t backslashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(character);
        }
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

static bool ParseUnsigned(const wchar_t* text, int base, uint64_t maximum, uint64_t& value) {
    if (!text || !*text || *text == L'-')
        return false;
    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long long parsed = wcstoull(text, &end, base);
    if (errno == ERANGE || end == text || !end || *end != L'\0' || parsed > maximum)
        return false;
    value = static_cast<uint64_t>(parsed);
    return true;
}

static int HexValue(wchar_t character) {
    if (character >= L'0' && character <= L'9')
        return character - L'0';
    if (character >= L'a' && character <= L'f')
        return character - L'a' + 10;
    if (character >= L'A' && character <= L'F')
        return character - L'A' + 10;
    return -1;
}

static bool ParseNonce(const wchar_t* text, ProcessChannelNonce& nonce) {
    if (!text || wcslen(text) != nonce.size() * 2)
        return false;
    for (size_t index = 0; index < nonce.size(); ++index) {
        const int high = HexValue(text[index * 2]);
        const int low = HexValue(text[index * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        nonce[index] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

std::wstring NonceToHex(const ProcessChannelNonce& nonce) {
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(nonce.size() * 2);
    for (uint8_t value : nonce) {
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 0xF]);
    }
    return result;
}

bool FillNonce(ProcessChannelNonce& nonce) {
    return BCryptGenRandom(nullptr, nonce.data(), static_cast<ULONG>(nonce.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) ==
           0;
}

bool BuildPipeSecurity(PipeSecurity& security) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<uint8_t> storage(bytes);
    const bool queried = bytes != 0 && GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes) != FALSE;
    CloseHandle(token);
    if (!queried)
        return false;

    auto* tokenUser = reinterpret_cast<TOKEN_USER*>(storage.data());
    wchar_t* sid = nullptr;
    if (!ConvertSidToStringSidW(tokenUser->User.Sid, &sid))
        return false;
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &security.descriptor,
                                                              nullptr)) {
        return false;
    }
    security.attributes.nLength = sizeof(security.attributes);
    security.attributes.lpSecurityDescriptor = security.descriptor;
    security.attributes.bInheritHandle = FALSE;
    return true;
}

void CancelOverlapped(HANDLE pipe, OVERLAPPED& overlapped) {
    CancelIoEx(pipe, &overlapped);
    DWORD ignored = 0;
    GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
}

ProcessMessage BuildMessage(ProcessMessageKind kind, uint16_t opcode, ProcessMode senderMode, uint64_t sequence,
                            uint32_t senderPid, const ProcessChannelNonce& nonce, const char* payload) {
    ProcessMessage message{};
    message.kind = kind;
    message.opcode = opcode;
    message.senderMode = senderMode;
    message.sequence = sequence;
    message.senderPid = senderPid;
    message.nonce = nonce;
    if (payload && *payload) {
        const size_t length = strnlen(payload, PROCESS_MAX_PAYLOAD);
        if (length < PROCESS_MAX_PAYLOAD) {
            memcpy(message.payload, payload, length);
            message.payloadSize = static_cast<uint32_t>(length + 1);
        }
    }
    message.totalSize = message.headerSize + message.payloadSize;
    return message;
}

static bool HasModeTokenBoundary(const char* token, size_t length) {
    const char next = token[length];
    return next == '\0' || next == ' ' || next == '\t';
}

bool ValidateProcessMessage(const ProcessMessage& message, size_t bytesRead, ProcessMessageKind expectedKind,
                            ProcessMode expectedSenderMode, uint32_t expectedSenderPid,
                            const ProcessChannelNonce& expectedNonce, uint64_t expectedSequence,
                            bool requireExactSequence) {
    if (bytesRead < offsetof(ProcessMessage, payload) || bytesRead > sizeof(ProcessMessage) ||
        message.magic != PROCESS_MSG_MAGIC || message.version != PROCESS_PROTOCOL_VERSION ||
        message.headerSize != offsetof(ProcessMessage, payload) || message.totalSize != bytesRead ||
        message.totalSize != message.headerSize + message.payloadSize || message.kind != expectedKind ||
        message.senderMode != expectedSenderMode || !IsValidMode(message.senderMode) ||
        message.senderPid != expectedSenderPid || message.nonce != expectedNonce || !ValidatePayload(message) ||
        !ValidateOpcodePayload(message)) {
        return false;
    }
    if ((requireExactSequence && message.sequence != expectedSequence) ||
        (!requireExactSequence && message.sequence <= expectedSequence)) {
        return false;
    }
    switch (message.kind) {
        case ProcessMessageKind::Startup:
            return message.opcode == 0 && message.sequence == 0 && message.payloadSize == 0;
        case ProcessMessageKind::Command:
            return IsValidCommand(message.opcode);
        case ProcessMessageKind::Response:
            return IsValidResponse(message.opcode);
        default:
            return false;
    }
}

ProcessMode ParseProcessMode(int argc, char* argv[]) {
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--mode=inject") == 0)
            return ProcessMode::Inject;
        if (strcmp(argv[index], "--mode=media") == 0)
            return ProcessMode::Media;
        if (strcmp(argv[index], "--mode=limiter") == 0)
            return ProcessMode::Limiter;
        if (strcmp(argv[index], "--mode=logger") == 0)
            return ProcessMode::Logger;
        if (strcmp(argv[index], "--mode=sensors") == 0)
            return ProcessMode::Sensors;
    }
    return ProcessMode::Controller;
}

ProcessMode ParseProcessMode(LPSTR commandLine) {
    if (!commandLine)
        return ProcessMode::Controller;
    const char* mode = strstr(commandLine, "--mode=");
    if (!mode)
        return ProcessMode::Controller;
    mode += 7;
    if (strncmp(mode, "inject", 6) == 0 && HasModeTokenBoundary(mode, 6))
        return ProcessMode::Inject;
    if (strncmp(mode, "media", 5) == 0 && HasModeTokenBoundary(mode, 5))
        return ProcessMode::Media;
    if (strncmp(mode, "limiter", 7) == 0 && HasModeTokenBoundary(mode, 7))
        return ProcessMode::Limiter;
    if (strncmp(mode, "logger", 6) == 0 && HasModeTokenBoundary(mode, 6))
        return ProcessMode::Logger;
    if (strncmp(mode, "sensors", 7) == 0 && HasModeTokenBoundary(mode, 7))
        return ProcessMode::Sensors;
    return ProcessMode::Controller;
}

std::string ParseSessionDir(LPSTR commandLine) {
    if (!commandLine)
        return {};
    const char* value = strstr(commandLine, "--session-dir=");
    if (!value)
        return {};
    value += 14;
    const char* end = value;
    while (*end && *end != ' ' && *end != '\t')
        ++end;
    return std::string(value, end);
}

std::string ParseRecordingId(LPSTR commandLine) {
    if (!commandLine)
        return {};
    const char* value = strstr(commandLine, "--recording-id=");
    if (!value)
        return {};
    value += 15;
    const char* end = value;
    while (*end && *end != ' ' && *end != '\t') {
        const unsigned char character = static_cast<unsigned char>(*end);
        if (!(std::isalnum(character) || character == '_' || character == '-'))
            return {};
        ++end;
    }
    const size_t length = static_cast<size_t>(end - value);
    if (length == 0 || length > 64)
        return {};
    return std::string(value, end);
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
        default:
            return "captureengine.log";
    }
}

std::string GetProcessLogFileName(ProcessMode mode, const std::string& recordingId, uint32_t processId) {
    if (mode != ProcessMode::Media)
        return GetLogFileName(mode);

    char processSuffix[24]{};
    snprintf(processSuffix, sizeof(processSuffix), "_%lu.log", static_cast<unsigned long>(processId));
    return std::string("media_") + (recordingId.empty() ? "unscoped" : recordingId) + processSuffix;
}

ProcessIPCServer::ProcessIPCServer(ProcessMode mode) : mode_(mode) {}

ProcessIPCServer::~ProcessIPCServer() {
    Shutdown();
}

bool ProcessIPCServer::ReadStartupArguments() {
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments)
        return false;
    uint64_t handleValue = 0;
    uint64_t controllerPid = 0;
    bool haveHandle = false;
    bool haveControllerPid = false;
    bool haveNonce = false;
    bool invalid = false;
    for (int index = 1; index < argumentCount; ++index) {
        constexpr wchar_t handlePrefix[] = L"--ipc-handle=";
        constexpr wchar_t pidPrefix[] = L"--ipc-controller-pid=";
        constexpr wchar_t noncePrefix[] = L"--ipc-nonce=";
        if (wcsncmp(arguments[index], handlePrefix, std::size(handlePrefix) - 1) == 0) {
            if (haveHandle || !ParseUnsigned(arguments[index] + std::size(handlePrefix) - 1, 0,
                                             std::numeric_limits<uintptr_t>::max(), handleValue)) {
                invalid = true;
            } else {
                haveHandle = true;
            }
        } else if (wcsncmp(arguments[index], pidPrefix, std::size(pidPrefix) - 1) == 0) {
            if (haveControllerPid ||
                !ParseUnsigned(arguments[index] + std::size(pidPrefix) - 1, 10, std::numeric_limits<uint32_t>::max(),
                               controllerPid) ||
                controllerPid == 0) {
                invalid = true;
            } else {
                haveControllerPid = true;
            }
        } else if (wcsncmp(arguments[index], noncePrefix, std::size(noncePrefix) - 1) == 0) {
            if (haveNonce || !ParseNonce(arguments[index] + std::size(noncePrefix) - 1, nonce_)) {
                invalid = true;
            } else {
                haveNonce = true;
            }
        }
    }
    LocalFree(reinterpret_cast<HLOCAL>(arguments));
    if (invalid || !haveHandle || !haveControllerPid || !haveNonce || handleValue == 0 ||
        handleValue == reinterpret_cast<uintptr_t>(INVALID_HANDLE_VALUE)) {
        return false;
    }

    pipe_ = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handleValue));
    controllerPid_ = static_cast<uint32_t>(controllerPid);
    DWORD handleFlags = 0;
    DWORD endpointFlags = 0;
    DWORD pipeState = 0;
    DWORD serverPid = 0;
    if (!GetHandleInformation(pipe_, &handleFlags) || GetFileType(pipe_) != FILE_TYPE_PIPE ||
        !GetNamedPipeInfo(pipe_, &endpointFlags, nullptr, nullptr, nullptr) ||
        !GetNamedPipeHandleStateW(pipe_, &pipeState, nullptr, nullptr, nullptr, nullptr, 0) ||
        (handleFlags & HANDLE_FLAG_INHERIT) == 0 || (endpointFlags & PIPE_SERVER_END) != 0 ||
        (pipeState & PIPE_READMODE_MESSAGE) == 0 || !GetNamedPipeServerProcessId(pipe_, &serverPid) ||
        serverPid != controllerPid_) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
        return false;
    }
    return SetHandleInformation(pipe_, HANDLE_FLAG_INHERIT, 0) != FALSE;
}

bool ProcessIPCServer::SendStartupHandshake() {
    const ProcessMessage handshake =
        BuildMessage(ProcessMessageKind::Startup, 0, mode_, 0, GetCurrentProcessId(), nonce_, nullptr);
    DWORD bytesWritten = 0;
    return WriteFile(pipe_, &handshake, handshake.totalSize, &bytesWritten, nullptr) &&
           bytesWritten == handshake.totalSize;
}

bool ProcessIPCServer::Init() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!IsIpcMode(mode_) || pipe_ != INVALID_HANDLE_VALUE || !ReadStartupArguments() || !SendStartupHandshake()) {
        if (pipe_ != INVALID_HANDLE_VALUE)
            CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
        return false;
    }
    connected_.store(true, std::memory_order_release);
    fatalDisconnect_.store(false, std::memory_order_release);
    LogInfo("[IPC] Accepted inherited %s endpoint from controller PID %lu", ModeName(mode_),
            static_cast<unsigned long>(controllerPid_));
    return true;
}

void ProcessIPCServer::MarkDisconnected(DWORD error, const char* operation) {
    if (!fatalDisconnect_.exchange(true, std::memory_order_acq_rel))
        LogWarn("[IPC] %s channel failed during %s (error=%lu); child will exit", ModeName(mode_), operation,
                static_cast<unsigned long>(error));
    connected_.store(false, std::memory_order_release);
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
}

void ProcessIPCServer::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_.store(false, std::memory_order_release);
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    lastSequence_ = 0;
}

bool ProcessIPCServer::PollCommand(ProcessCommand& command, char* payload, size_t payloadSize) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_.load(std::memory_order_acquire) || pipe_ == INVALID_HANDLE_VALUE)
        return false;
    DWORD available = 0;
    DWORD messageBytes = 0;
    if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, &messageBytes)) {
        MarkDisconnected(GetLastError(), "peek");
        return false;
    }
    if (available == 0)
        return false;
    if (messageBytes < offsetof(ProcessMessage, payload) || messageBytes > sizeof(ProcessMessage)) {
        LogInvalidMessage("invalid byte length %lu", static_cast<unsigned long>(messageBytes));
        MarkDisconnected(ERROR_INVALID_DATA, "framing");
        return false;
    }

    ProcessMessage message{};
    DWORD bytesRead = 0;
    if (!ReadFile(pipe_, &message, sizeof(message), &bytesRead, nullptr)) {
        MarkDisconnected(GetLastError(), "read");
        return false;
    }
    if (!ValidateProcessMessage(message, bytesRead, ProcessMessageKind::Command, ProcessMode::Controller,
                                controllerPid_, nonce_, lastSequence_, false)) {
        LogInvalidMessage("command identity/header/sequence mismatch");
        MarkDisconnected(ERROR_INVALID_DATA, "validation");
        return false;
    }
    lastSequence_ = message.sequence;
    command = static_cast<ProcessCommand>(message.opcode);
    if (payload && payloadSize != 0) {
        payload[0] = '\0';
        if (message.payloadSize != 0) {
            const size_t copyLength = std::min(payloadSize - 1, static_cast<size_t>(message.payloadSize - 1));
            memcpy(payload, message.payload, copyLength);
            payload[copyLength] = '\0';
        }
    }
    return true;
}

bool ProcessIPCServer::SendResponse(ProcessResponse response, const char* payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_.load(std::memory_order_acquire) || pipe_ == INVALID_HANDLE_VALUE ||
        !IsValidResponse(static_cast<uint16_t>(response)) ||
        (payload && strnlen(payload, PROCESS_MAX_PAYLOAD) == PROCESS_MAX_PAYLOAD)) {
        return false;
    }
    const ProcessMessage message = BuildMessage(ProcessMessageKind::Response, static_cast<uint16_t>(response), mode_,
                                                lastSequence_, GetCurrentProcessId(), nonce_, payload);
    if (!ValidateOpcodePayload(message))
        return false;
    DWORD bytesWritten = 0;
    if (!WriteFile(pipe_, &message, message.totalSize, &bytesWritten, nullptr) || bytesWritten != message.totalSize) {
        MarkDisconnected(GetLastError(), "write");
        return false;
    }
    return true;
}
