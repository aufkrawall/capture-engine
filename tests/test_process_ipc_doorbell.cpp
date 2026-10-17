#include <gtest/gtest.h>

#include <windows.h>
#include <shellapi.h>

#include <cwchar>
#include <filesystem>
#include <string>

#include "common/ipc/process_ipc.h"
#include "common/platform/restricted_child_process.h"
#include "source_fragment_reader.h"

// The IPC doorbell (common/ipc/process_ipc.h): the controller rings a child's inherited event after
// writing a command, and a child waiting for it serves the command at once. Until 2026-10-18 the inject
// child looked at its pipe every 250 ms (100 ms with a game) while the controller's main thread waited
// for the ack, so every config reload froze tray and hotkeys for 250-313 ms (20261018_005433 and three
// earlier sessions).
namespace {

bool HasArgument(const wchar_t* prefix) {
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments)
        return false;
    bool found = false;
    for (int index = 1; index < argumentCount && !found; ++index)
        found = wcsncmp(arguments[index], prefix, wcslen(prefix)) == 0;
    LocalFree(reinterpret_cast<HLOCAL>(arguments));
    return found;
}

}  // namespace

// Runs only as the child ACommandWakesTheChildAtOnce launches, with the controller's --ipc-* arguments.
TEST(ProcessIPCDoorbellProbe, ServesACommandTheMomentItRings) {
    if (!HasArgument(L"--ipc-wake=")) {
        SUCCEED();
        return;
    }
    ProcessIPCServer server(ProcessMode::Media);
    ASSERT_TRUE(server.Init());
    // Without the ring this sleeps the whole 30 s and the parent's 10 s command window expires first.
    ASSERT_TRUE(server.WaitForCommand(30000));
    ProcessCommand command = ProcessCommand::None;
    ASSERT_TRUE(server.PollCommand(command));
    EXPECT_EQ(command, ProcessCommand::Ping);
    EXPECT_TRUE(server.SendResponse(ProcessResponse::Pong));
    EXPECT_FALSE(server.WaitForCommand(0)) << "one ring per command";
}

TEST(ProcessIPCDoorbellTest, ACommandWakesTheChildAtOnce) {
    std::wstring executable(32768, L'\0');
    const DWORD executableLength = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    ASSERT_GT(executableLength, 0u);
    ASSERT_LT(executableLength, executable.size());
    executable.resize(executableLength);

    ProcessIPCClient client(ProcessMode::Media);
    HANDLE endpoint = INVALID_HANDLE_VALUE;
    std::wstring arguments;
    ASSERT_TRUE(client.PrepareChildEndpoint(endpoint, arguments));
    HANDLE wake = client.ChildWakeHandle();
    ASSERT_NE(wake, nullptr);
    EXPECT_NE(arguments.find(L"--ipc-wake=0x"), std::wstring::npos);

    const std::wstring commandLine =
        L"\"" + executable + L"\" --gtest_filter=ProcessIPCDoorbellProbe.ServesACommandTheMomentItRings " + arguments;
    ce::process::RestrictedChildProcess child;
    DWORD error = ERROR_SUCCESS;
    const bool launched = ce::process::LaunchRestrictedChildProcess(
        executable, commandLine, std::filesystem::current_path().wstring(), {endpoint, wake}, CREATE_NO_WINDOW, child,
        error);
    SetHandleInformation(endpoint, HANDLE_FLAG_INHERIT, 0);
    CloseHandle(endpoint);
    ASSERT_TRUE(launched) << error;

    const bool authenticated = client.CompleteChildSpawn(child.processId, 15000);
    ProcessResponse response = ProcessResponse::None;
    const bool answered = authenticated && client.SendCommand(ProcessCommand::Ping, nullptr, &response, 10000);
    client.Disconnect();
    if (WaitForSingleObject(child.processHandle, 15000) != WAIT_OBJECT_0)
        TerminateProcess(child.processHandle, ERROR_PROCESS_ABORTED);
    DWORD exitCode = ERROR_PROCESS_ABORTED;
    GetExitCodeProcess(child.processHandle, &exitCode);
    EXPECT_TRUE(authenticated);
    EXPECT_TRUE(answered) << "the child must answer within the controller's window, not at its next poll";
    EXPECT_EQ(response, ProcessResponse::Pong);
    EXPECT_EQ(exitCode, 0u);
}

// The inject child's loop waits on the doorbell and reads the pipe as soon as it rings.
TEST(ProcessIPCDoorbellSourceTest, InjectLoopWaitsOnTheDoorbell) {
    const std::string source = ce::test_source::ReadLogicalSource(std::filesystem::current_path() /
                                                                  "captureengine/injection/inject_main.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("if (commandRang || hasGame || (ipcNow - lastIpcPoll) >= 250) {"), std::string::npos);
    EXPECT_NE(source.find("commandRang = ipc.WaitForCommand("), std::string::npos);
    EXPECT_EQ(source.find("Sleep(250);"), std::string::npos);
}
