#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include "../common/process_ipc.h"
#include "../common/restricted_child_process.h"

namespace {

ProcessChannelNonce TestNonce() {
    ProcessChannelNonce nonce{};
    for (size_t index = 0; index < nonce.size(); ++index)
        nonce[index] = static_cast<uint8_t>(index * 7 + 3);
    return nonce;
}

ProcessMessage ValidCommand() {
    ProcessMessage message{};
    message.kind = ProcessMessageKind::Command;
    message.opcode = static_cast<uint16_t>(ProcessCommand::Ping);
    message.senderMode = ProcessMode::Controller;
    message.sequence = 9;
    message.senderPid = 1234;
    message.nonce = TestNonce();
    return message;
}

std::string ReadSource(const std::filesystem::path& relativePath) {
    std::ifstream stream(std::filesystem::current_path() / relativePath, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::optional<uintptr_t> CommandLineHandle(const wchar_t* prefix) {
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments)
        return std::nullopt;
    std::optional<uintptr_t> result;
    const size_t prefixLength = wcslen(prefix);
    for (int index = 1; index < argumentCount; ++index) {
        if (wcsncmp(arguments[index], prefix, prefixLength) != 0)
            continue;
        errno = 0;
        wchar_t* end = nullptr;
        const unsigned long long value = wcstoull(arguments[index] + prefixLength, &end, 0);
        if (errno == 0 && end != arguments[index] + prefixLength && end && *end == L'\0' && value != 0 &&
            value <= std::numeric_limits<uintptr_t>::max()) {
            result = static_cast<uintptr_t>(value);
        }
        break;
    }
    LocalFree(arguments);
    return result;
}

}  // namespace

TEST(ProcessIPCTest, AcceptsExactAuthenticatedCommand) {
    const ProcessMessage message = ValidCommand();
    EXPECT_TRUE(ValidateProcessMessage(message, message.totalSize, ProcessMessageKind::Command, ProcessMode::Controller,
                                       1234, TestNonce(), 8, false));
}

TEST(ProcessIPCTest, RejectsShortOversizedAndInconsistentFraming) {
    ProcessMessage message = ValidCommand();
    EXPECT_FALSE(ValidateProcessMessage(message, message.headerSize - 1, ProcessMessageKind::Command,
                                        ProcessMode::Controller, 1234, TestNonce(), 8, false));
    EXPECT_FALSE(ValidateProcessMessage(message, sizeof(ProcessMessage) + 1, ProcessMessageKind::Command,
                                        ProcessMode::Controller, 1234, TestNonce(), 8, false));
    ++message.totalSize;
    EXPECT_FALSE(ValidateProcessMessage(message, message.totalSize - 1, ProcessMessageKind::Command,
                                        ProcessMode::Controller, 1234, TestNonce(), 8, false));
}

TEST(ProcessIPCTest, RejectsIdentityNonceModeAndSequenceMismatch) {
    ProcessMessage message = ValidCommand();
    EXPECT_FALSE(ValidateProcessMessage(message, message.totalSize, ProcessMessageKind::Command,
                                        ProcessMode::Controller, 9999, TestNonce(), 8, false));
    ProcessChannelNonce wrongNonce = TestNonce();
    wrongNonce[4] ^= 0x80;
    EXPECT_FALSE(ValidateProcessMessage(message, message.totalSize, ProcessMessageKind::Command,
                                        ProcessMode::Controller, 1234, wrongNonce, 8, false));
    EXPECT_FALSE(ValidateProcessMessage(message, message.totalSize, ProcessMessageKind::Response,
                                        ProcessMode::Controller, 1234, TestNonce(), 8, false));
    EXPECT_FALSE(ValidateProcessMessage(message, message.totalSize, ProcessMessageKind::Command,
                                        ProcessMode::Controller, 1234, TestNonce(), 9, false));
}

TEST(ProcessIPCTest, RejectsUnknownOpcodeAndMalformedPayload) {
    ProcessMessage message = ValidCommand();
    message.opcode = 99;
    EXPECT_FALSE(ValidateProcessMessage(message, message.totalSize, ProcessMessageKind::Command,
                                        ProcessMode::Controller, 1234, TestNonce(), 8, false));
    message = ValidCommand();
    memcpy(message.payload, "payload", 8);
    message.payloadSize = 8;
    message.totalSize = message.headerSize + message.payloadSize;
    message.payload[message.payloadSize - 1] = 'x';
    EXPECT_FALSE(ValidateProcessMessage(message, message.totalSize, ProcessMessageKind::Command,
                                        ProcessMode::Controller, 1234, TestNonce(), 8, false));
}

TEST(ProcessIPCTest, EnforcesOpcodeSpecificPayloads) {
    ProcessMessage message = ValidCommand();
    memcpy(message.payload, "payload", 8);
    message.payloadSize = 8;
    message.totalSize = message.headerSize + message.payloadSize;
    EXPECT_FALSE(ValidateProcessMessage(message, message.totalSize, ProcessMessageKind::Command,
                                        ProcessMode::Controller, 1234, TestNonce(), 8, false));

    message = ValidCommand();
    message.opcode = static_cast<uint16_t>(ProcessCommand::StartRecording);
    memcpy(message.payload, "audio_only", sizeof("audio_only"));
    message.payloadSize = sizeof("audio_only");
    message.totalSize = message.headerSize + message.payloadSize;
    EXPECT_TRUE(ValidateProcessMessage(message, message.totalSize, ProcessMessageKind::Command, ProcessMode::Controller,
                                       1234, TestNonce(), 8, false));
}

TEST(ProcessIPCTest, StartupHandshakeRequiresExactPidNonceAndZeroSequence) {
    ProcessMessage startup{};
    startup.kind = ProcessMessageKind::Startup;
    startup.senderMode = ProcessMode::Media;
    startup.senderPid = 4321;
    startup.nonce = TestNonce();
    EXPECT_TRUE(ValidateProcessMessage(startup, startup.totalSize, ProcessMessageKind::Startup, ProcessMode::Media,
                                       4321, TestNonce(), 0, true));
    startup.sequence = 1;
    EXPECT_FALSE(ValidateProcessMessage(startup, startup.totalSize, ProcessMessageKind::Startup, ProcessMode::Media,
                                        4321, TestNonce(), 0, true));
}

TEST(ProcessIPCTest, ProductionUsesInheritedEndpointsWithoutFixedPipeConstants) {
    const std::string header = ReadSource("common/process_ipc.h");
    const std::string source = ReadSource("common/process_ipc.cpp");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());
    EXPECT_EQ(header.find("PIPE_NAME_INJECT"), std::string::npos);
    EXPECT_EQ(header.find("CaptureEngine_Inject"), std::string::npos);
    EXPECT_EQ(source.find("WaitNamedPipe"), std::string::npos);
    EXPECT_NE(source.find("PrepareChildEndpoint"), std::string::npos);
    EXPECT_NE(source.find("GetNamedPipeServerProcessId"), std::string::npos);
    EXPECT_NE(source.find("SetNamedPipeHandleState(childEndpoint"), std::string::npos);
    EXPECT_NE(source.find("GetNamedPipeHandleStateW(pipe_"), std::string::npos);
    EXPECT_EQ(source.find("GetNamedPipeClientProcessId"), std::string::npos);
    EXPECT_NE(source.find("ValidateProcessMessage(handshake"), std::string::npos);
    EXPECT_NE(source.find("--ipc-nonce="), std::string::npos);
}

TEST(ProcessIPCTest, RestrictedLauncherUsesAnExplicitHandleList) {
    const std::string source = ReadSource("common/restricted_child_process.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("PROC_THREAD_ATTRIBUTE_HANDLE_LIST"), std::string::npos);
    EXPECT_NE(source.find("EXTENDED_STARTUPINFO_PRESENT"), std::string::npos);
    EXPECT_NE(source.find("inheritedHandles.empty() ? FALSE : TRUE"), std::string::npos);
}

TEST(ProcessIPCTest, ProcessLoopbackWorkerDispatchesBeforeEveryNormalStartupPath) {
    const std::string mainSource = ReadSource("captureengine/main.cpp");
    const std::string workerHost = ReadSource("captureengine/process_loopback_worker_host.cpp");
    const std::string processHeader = ReadSource("common/process_ipc.h");
    ASSERT_FALSE(mainSource.empty());
    ASSERT_FALSE(workerHost.empty());
    ASSERT_FALSE(processHeader.empty());

    const size_t workerDispatch = mainSource.find("TryRunProcessLoopbackWorkerHost()");
    const size_t dumpDispatch = mainSource.find("IsDumpHelperCommandLine(lpCmdLine)");
    const size_t modeParsing = mainSource.find("ParseProcessMode(lpCmdLine)");
    ASSERT_NE(workerDispatch, std::string::npos);
    ASSERT_NE(dumpDispatch, std::string::npos);
    ASSERT_NE(modeParsing, std::string::npos);
    EXPECT_LT(workerDispatch, dumpDispatch);
    EXPECT_LT(workerDispatch, modeParsing);

    EXPECT_NE(workerHost.find("CommandLineToArgvW(GetCommandLineW()"), std::string::npos);
    EXPECT_NE(workerHost.find("wcscmp(arguments[1], kProcessLoopbackWorkerCommand)"), std::string::npos);
    EXPECT_NE(workerHost.find("argumentCount != 11"), std::string::npos);
    EXPECT_NE(workerHost.find("return ERROR_BAD_ARGUMENTS"), std::string::npos);
    EXPECT_EQ(processHeader.find("ProcessLoopbackWorker"), std::string::npos);
}

TEST(ProcessIPCTest, ControllerRecoversChildrenOnlyThroughFreshAuthenticatedSpawns) {
    const std::string source = ReadSource("captureengine/main.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("recoverProcess(ProcessMode::Inject"), std::string::npos);
    EXPECT_NE(source.find("recoverProcess(ProcessMode::Media"), std::string::npos);
    EXPECT_NE(source.find("recoverProcess(ProcessMode::Limiter"), std::string::npos);
    EXPECT_NE(source.find("EnsureChildProcessConnected(mode, process, client"), std::string::npos);
}

TEST(RestrictedChildLauncherProbe, InheritsOnlyAllowlistedMapping) {
    const auto allowedValue = CommandLineHandle(L"--probe-allowed=");
    const auto blockedValue = CommandLineHandle(L"--probe-blocked=");
    if (!allowedValue || !blockedValue) {
        SUCCEED();
        return;
    }

    constexpr uint32_t kAllowedMagic = 0xA110CA7Eu;
    constexpr uint32_t kBlockedMagic = 0xB10CCEDu;
    HANDLE allowed = reinterpret_cast<HANDLE>(*allowedValue);
    HANDLE blocked = reinterpret_cast<HANDLE>(*blockedValue);
    auto* allowedView = static_cast<const uint32_t*>(MapViewOfFile(allowed, FILE_MAP_READ, 0, 0, sizeof(uint32_t)));
    ASSERT_NE(allowedView, nullptr);
    EXPECT_EQ(*allowedView, kAllowedMagic);
    UnmapViewOfFile(allowedView);

    auto* blockedView = static_cast<const uint32_t*>(MapViewOfFile(blocked, FILE_MAP_READ, 0, 0, sizeof(uint32_t)));
    if (blockedView) {
        EXPECT_NE(*blockedView, kBlockedMagic);
        UnmapViewOfFile(blockedView);
    }
}

TEST(ProcessIPCTest, RestrictedLauncherActuallyExcludesUnlistedInheritableHandles) {
    constexpr uint32_t kAllowedMagic = 0xA110CA7Eu;
    constexpr uint32_t kBlockedMagic = 0xB10CCEDu;
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    HANDLE allowed = CreateFileMappingW(INVALID_HANDLE_VALUE, &inheritable, PAGE_READWRITE, 0, 4096, nullptr);
    HANDLE blocked = CreateFileMappingW(INVALID_HANDLE_VALUE, &inheritable, PAGE_READWRITE, 0, 4096, nullptr);
    ASSERT_NE(allowed, nullptr);
    ASSERT_NE(blocked, nullptr);
    auto* allowedView = static_cast<uint32_t*>(MapViewOfFile(allowed, FILE_MAP_WRITE, 0, 0, sizeof(uint32_t)));
    auto* blockedView = static_cast<uint32_t*>(MapViewOfFile(blocked, FILE_MAP_WRITE, 0, 0, sizeof(uint32_t)));
    ASSERT_NE(allowedView, nullptr);
    ASSERT_NE(blockedView, nullptr);
    *allowedView = kAllowedMagic;
    *blockedView = kBlockedMagic;
    UnmapViewOfFile(allowedView);
    UnmapViewOfFile(blockedView);

    std::wstring executable(32768, L'\0');
    const DWORD executableLength =
        GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    ASSERT_GT(executableLength, 0u);
    ASSERT_LT(executableLength, executable.size());
    executable.resize(executableLength);
    const std::wstring commandLine =
        L"\"" + executable +
        L"\" --gtest_filter=RestrictedChildLauncherProbe.InheritsOnlyAllowlistedMapping --probe-allowed=" +
        std::to_wstring(reinterpret_cast<uintptr_t>(allowed)) + L" --probe-blocked=" +
        std::to_wstring(reinterpret_cast<uintptr_t>(blocked));

    ce::process::RestrictedChildProcess child;
    DWORD error = ERROR_SUCCESS;
    ASSERT_TRUE(ce::process::LaunchRestrictedChildProcess(
        executable, commandLine, std::filesystem::current_path().wstring(), {allowed}, CREATE_NO_WINDOW, child, error))
        << error;
    ASSERT_EQ(WaitForSingleObject(child.processHandle, 15000), WAIT_OBJECT_0);
    DWORD exitCode = ERROR_PROCESS_ABORTED;
    ASSERT_TRUE(GetExitCodeProcess(child.processHandle, &exitCode));
    EXPECT_EQ(exitCode, 0u);
    CloseHandle(blocked);
    CloseHandle(allowed);
}
