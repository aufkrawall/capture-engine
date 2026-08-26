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
#include "source_fragment_reader.h"

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
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
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
    LocalFree(reinterpret_cast<HLOCAL>(arguments));
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

TEST(ProcessIPCTest, RecordingIdentityIsStrictAndProducesImmutableMediaLogNames) {
    char valid[] = "--mode=media --recording-id=r0042";
    char traversal[] = "--recording-id=..\\escape";
    char empty[] = "--recording-id=";
    EXPECT_EQ(ParseRecordingId(valid), "r0042");
    EXPECT_TRUE(ParseRecordingId(traversal).empty());
    EXPECT_TRUE(ParseRecordingId(empty).empty());
    EXPECT_EQ(GetProcessLogFileName(ProcessMode::Media, "r0042", 1234), "media_r0042_1234.log");
    EXPECT_EQ(GetProcessLogFileName(ProcessMode::Media, "", 1234), "media_unscoped_1234.log");
    EXPECT_EQ(GetProcessLogFileName(ProcessMode::Inject, "r0042", 1234), "inject.log");
}

TEST(ProcessIPCTest, ProductionUsesInheritedEndpointsWithoutFixedPipeConstants) {
    const std::string header = ReadSource("common/process_ipc.h");
    // The endpoint implementation is split across the server and client halves,
    // so both are scanned: the positive expectations below name code from each,
    // and the negative ones must hold across the whole implementation.
    const std::string serverSource = ReadSource("common/process_ipc.cpp");
    const std::string clientSource = ReadSource("common/process_ipc_client.cpp");
    const std::string source = serverSource + clientSource;
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(serverSource.empty());
    ASSERT_FALSE(clientSource.empty());
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
    EXPECT_NE(source.find("startup.StartupInfo.dwFlags = STARTF_FORCEOFFFEEDBACK"), std::string::npos);
}

TEST(ProcessIPCTest, ControllerClearsLaunchFeedbackBeforeSlowStartupAndChildrenNeverSetTheCursor) {
    const std::string source = ReadSource("captureengine/main.cpp");
    ASSERT_FALSE(source.empty());

    const size_t modeParsing = source.find("ProcessMode mode = ParseProcessMode(lpCmdLine)");
    const size_t controllerOnlyCursor = source.find("if (mode == ProcessMode::Controller)", modeParsing);
    const size_t earlyCursor = source.find("PrimeStartupCursor();", controllerOnlyCursor);
    const size_t pathResolution = source.find("// Get paths", earlyCursor);
    const size_t processDispatch = source.find("switch (mode)", pathResolution);
    ASSERT_NE(modeParsing, std::string::npos);
    ASSERT_NE(controllerOnlyCursor, std::string::npos);
    ASSERT_NE(earlyCursor, std::string::npos);
    ASSERT_NE(pathResolution, std::string::npos);
    ASSERT_NE(processDispatch, std::string::npos);
    EXPECT_LT(modeParsing, controllerOnlyCursor);
    EXPECT_LT(controllerOnlyCursor, earlyCursor);
    EXPECT_LT(earlyCursor, pathResolution);
    EXPECT_EQ(source.substr(pathResolution, processDispatch - pathResolution).find("PrimeStartupCursor();"),
              std::string::npos);

    const size_t controllerMain = source.find("int ControllerMain(HINSTANCE hInstance)");
    const size_t trayCreation = source.find("auto tray = std::make_unique<TrayIcon>", controllerMain);
    const size_t vulkanRegistration = source.find("VulkanLayerResidency vulkanReg", controllerMain);
    ASSERT_NE(controllerMain, std::string::npos);
    ASSERT_NE(trayCreation, std::string::npos);
    ASSERT_NE(vulkanRegistration, std::string::npos);
    EXPECT_LT(trayCreation, vulkanRegistration);
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

TEST(ProcessIPCTest, NormalRecordingStopIsAcceptedBeforeMediaFinalizationAndEndpointRelease) {
    const std::string controllerSource = ReadSource("captureengine/main.cpp");
    const std::string mediaSource = ReadSource("captureengine/media_main.cpp");
    ASSERT_FALSE(controllerSource.empty());
    ASSERT_FALSE(mediaSource.empty());

    const size_t helperBegin = controllerSource.find("RequestRecordingStopAndReleaseMedia(");
    const size_t helperEnd = controllerSource.find("void CheckRecordingFailureState()", helperBegin);
    ASSERT_NE(helperBegin, std::string::npos);
    ASSERT_NE(helperEnd, std::string::npos);
    const std::string helper = controllerSource.substr(helperBegin, helperEnd - helperBegin);
    const size_t mediaRequest = helper.find("RequestChildRecordingStop(main_g_MediaClient.get()");
    const size_t injectFallback = helper.find("RequestChildRecordingStop(main_g_InjectClient.get()");
    const size_t endpointRelease = helper.find("main_g_MediaClient->Disconnect()");
    ASSERT_NE(mediaRequest, std::string::npos);
    ASSERT_NE(injectFallback, std::string::npos);
    ASSERT_NE(endpointRelease, std::string::npos);
    EXPECT_LT(mediaRequest, injectFallback);
    EXPECT_LT(injectFallback, endpointRelease);
    EXPECT_NE(helper.find("mediaAccepted || RequestChildRecordingStop"), std::string::npos);

    EXPECT_NE(controllerSource.find("RequestRecordingStopAndReleaseMedia(\"record hotkey\", 5000)"),
              std::string::npos);
    EXPECT_NE(controllerSource.find("RequestRecordingStopAndReleaseMedia(\"audio-only hotkey\", 5000)"),
              std::string::npos);
    EXPECT_EQ(controllerSource.find("Stop failed - retrying once"), std::string::npos);

    const size_t stopCase = mediaSource.find("case ProcessCommand::StopRecording:");
    const size_t nextCase = mediaSource.find("case ProcessCommand::Ping:", stopCase);
    ASSERT_NE(stopCase, std::string::npos);
    ASSERT_NE(nextCase, std::string::npos);
    const std::string stopBlock = mediaSource.substr(stopCase, nextCase - stopCase);
    const size_t sharedStateClear = stopBlock.find("SetCaptureRequestedState(false)");
    const size_t accepted = stopBlock.find("ipc.SendResponse(ProcessResponse::Ack)");
    const size_t finalization = stopBlock.find("StopRecording();");
    ASSERT_NE(sharedStateClear, std::string::npos);
    ASSERT_NE(accepted, std::string::npos);
    ASSERT_NE(finalization, std::string::npos);
    EXPECT_LT(sharedStateClear, accepted);
    EXPECT_LT(accepted, finalization);
    EXPECT_EQ(stopBlock.find("ProcessResponse::RecordingStopped"), std::string::npos);
    EXPECT_NE(stopBlock.find("g_Running = false"), std::string::npos);
}

TEST(ProcessIPCTest, OverlayToggleHotkeyIsWiredEndToEnd) {
    const std::string controllerSource = ReadSource("captureengine/main.cpp");
    const std::string injectSource = ReadSource("captureengine/inject_main.cpp");
    const std::string publicationSource =
        ReadSource("captureengine/inject_config_publication.cpp");
    const std::string protocolHeader = ReadSource("common/process_ipc.h");
    const std::string templateSource = ReadSource("captureengine/config.ini.template");
    ASSERT_FALSE(controllerSource.empty());
    ASSERT_FALSE(injectSource.empty());
    ASSERT_FALSE(publicationSource.empty());
    ASSERT_FALSE(protocolHeader.empty());
    ASSERT_FALSE(templateSource.empty());

    EXPECT_NE(protocolHeader.find("ToggleOverlay = 6"), std::string::npos);

    // The controller registers the hotkey, dispatches it in the message loop and
    // forwards the intent to the inject client over the authenticated channel.
    // Both delivery paths reach the same dispatch: RegisterHotKey posts
    // WM_HOTKEY, and the low-level keyboard hook posts its own message for the
    // applications that suppress hotkey processing outright.
    EXPECT_NE(controllerSource.find("HOTKEY_ID_TOGGLE_OVERLAY"), std::string::npos);
    const size_t loopDispatch =
        controllerSource.find("msg.message == WM_HOTKEY || msg.message == main_kMsgHotkeyFromInputHook");
    ASSERT_NE(loopDispatch, std::string::npos);
    EXPECT_NE(controllerSource.find("DispatchHotkey(static_cast<int>(msg.wParam))", loopDispatch), std::string::npos);
    const size_t dispatch = controllerSource.find("hotkeyId == HOTKEY_ID_TOGGLE_OVERLAY");
    ASSERT_NE(dispatch, std::string::npos);
    EXPECT_NE(controllerSource.find("ToggleOverlay();", dispatch), std::string::npos);
    EXPECT_NE(controllerSource.find("SendCommand(ProcessCommand::ToggleOverlay, nullptr, &response)"),
              std::string::npos);

    // The inject process arms a runtime visibility override, republishes the
    // active target's resolved config and acknowledges. Flipping the loaded base
    // config and publishing that instead stripped the running target's profile,
    // so a single overlay toggle dropped its graphics, DLSS and UE5 overrides.
    const size_t toggleCase = injectSource.find("case ProcessCommand::ToggleOverlay:");
    ASSERT_NE(toggleCase, std::string::npos);
    const size_t toggleEnd = injectSource.find("case ProcessCommand::ReloadConfig:", toggleCase);
    ASSERT_NE(toggleEnd, std::string::npos);
    const std::string toggleBlock = injectSource.substr(toggleCase, toggleEnd - toggleCase);
    EXPECT_NE(toggleBlock.find("TogglePublishedOverlayVisibility(pSharedMem)"), std::string::npos);
    EXPECT_EQ(toggleBlock.find("currentConfig"), std::string::npos)
        << "publishing the base config here drops the active target's profile overrides";
    EXPECT_NE(toggleBlock.find("ProcessResponse::Ack"), std::string::npos);
    EXPECT_NE(publicationSource.find("ResolveActiveConfigLocked(sharedMemory, targetProcess)"),
              std::string::npos);
    EXPECT_NE(publicationSource.find("ToggleOverlayVisibility(publication.overlayVisibility"),
              std::string::npos);
    EXPECT_NE(publicationSource.find("ApplyOverlayVisibility(publication.overlayVisibility, resolved)"),
              std::string::npos);
    EXPECT_NE(publicationSource.find("PublishConfigLocked(sharedMemory, resolved"), std::string::npos);

    // One publication path for the whole process. A second direct call is
    // exactly how an unresolved base config reached shared memory.
    size_t publications = 0;
    for (const std::string* source : {&injectSource, &publicationSource}) {
        for (size_t at = source->find("UpdateSharedMemoryFromConfig("); at != std::string::npos;
             at = source->find("UpdateSharedMemoryFromConfig(", at + 1)) {
            ++publications;
        }
    }
    EXPECT_EQ(publications, 1u) << "every shared-memory config publication must resolve the active target first";

    // The config template exposes the renamed hotkey and no longer carries the
    // dead toggle_fps key.
    EXPECT_NE(templateSource.find("toggle_overlay="), std::string::npos);
    EXPECT_EQ(templateSource.find("toggle_fps"), std::string::npos);
}

TEST(RestrictedChildLauncherProbe, InheritsOnlyAllowlistedMapping) {
    const auto allowedValue = CommandLineHandle(L"--probe-allowed=");
    const auto blockedValue = CommandLineHandle(L"--probe-blocked=");
    if (!allowedValue || !blockedValue) {
        SUCCEED();
        return;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    GetStartupInfoW(&startup);
    EXPECT_NE(startup.dwFlags & STARTF_FORCEOFFFEEDBACK, 0u);

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
