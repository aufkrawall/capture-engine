#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string ReadTraySource(const std::filesystem::path& relativePath) {
    std::ifstream stream(std::filesystem::current_path() / relativePath, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST(TrayIconSourceTest, RestoresCurrentIconStateWhenExplorerRecreatesTaskbar) {
    const std::string header = ReadTraySource("captureengine/tray.h");
    const std::string source = ReadTraySource("captureengine/tray.cpp");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());

    EXPECT_NE(header.find("UINT taskbarCreatedMessage = 0"), std::string::npos);
    EXPECT_NE(source.find("RegisterWindowMessageA(\"TaskbarCreated\")"), std::string::npos);
    EXPECT_NE(source.find("CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE"), std::string::npos);
    EXPECT_NE(source.find("static_cast<TrayIcon*>(create->lpCreateParams)"), std::string::npos);

    const size_t handler = source.find("message == pThis->taskbarCreatedMessage");
    ASSERT_NE(handler, std::string::npos);
    EXPECT_NE(source.find("pThis->RestoreAfterTaskbarCreated();", handler), std::string::npos);

    const size_t recovery = source.find("void TrayIcon::RestoreAfterTaskbarCreated()");
    const size_t recordingState = source.find("void TrayIcon::SetRecordingState", recovery);
    ASSERT_NE(recovery, std::string::npos);
    ASSERT_NE(recordingState, std::string::npos);
    const std::string recoveryBody = source.substr(recovery, recordingState - recovery);
    EXPECT_NE(recoveryBody.find("iconRemovalRequested || !iconInitialized"), std::string::npos);
    EXPECT_NE(recoveryBody.find("Shell_NotifyIconA(NIM_ADD, &nid)"), std::string::npos);
    EXPECT_EQ(recoveryBody.find("nid.hIcon ="), std::string::npos);
    EXPECT_EQ(recoveryBody.find("strcpy_s(nid.szTip"), std::string::npos);
}
