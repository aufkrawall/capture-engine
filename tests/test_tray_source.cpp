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

TEST(TrayIconSourceTest, EnsuresContextMenuOpensOnTopOfTaskbar) {
    const std::string source = ReadTraySource("captureengine/tray.cpp");
    ASSERT_FALSE(source.empty());

    // Window creation must include WS_EX_TOPMOST so owned popups inherit topmost Z-order
    EXPECT_NE(source.find("WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST"), std::string::npos);

    // Context menu display must ensure foreground focus and topmost positioning
    const size_t showMenu = source.find("void TrayIcon::ShowContextMenu()");
    ASSERT_NE(showMenu, std::string::npos);
    const size_t wndProc = source.find("LRESULT CALLBACK TrayIcon::WndProc", showMenu);
    ASSERT_NE(wndProc, std::string::npos);
    const std::string showMenuBody = source.substr(showMenu, wndProc - showMenu);

    // Must remove WS_EX_NOACTIVATE and assert HWND_TOPMOST before tracking menu
    EXPECT_NE(showMenuBody.find("SetWindowLongPtr(hWnd, GWL_EXSTYLE, (originalExStyle & ~WS_EX_NOACTIVATE) | WS_EX_TOPMOST);"), std::string::npos);
    EXPECT_NE(showMenuBody.find("SetWindowPos(hWnd, HWND_TOPMOST"), std::string::npos);
    EXPECT_NE(showMenuBody.find("AttachThreadInput(currentThreadId, foregroundThreadId, TRUE);"), std::string::npos);
    EXPECT_NE(showMenuBody.find("SetForegroundWindow(hWnd);"), std::string::npos);

    // Must use TrackPopupMenuEx with exclusion rect to avoid taskbar occlusion
    EXPECT_NE(showMenuBody.find("TrackPopupMenuEx(hMenu, uFlags, pt.x, menuY, hWnd, &tpm)"), std::string::npos);
    EXPECT_NE(showMenuBody.find("TPMPARAMS tpm = {sizeof(TPMPARAMS), rcExclude};"), std::string::npos);
    EXPECT_NE(showMenuBody.find("uFlags |= TPM_BOTTOMALIGN | TPM_VERTICAL;"), std::string::npos);

    // Must NOT install CBT hook because modifying #32768 creation parameters aborts TrackPopupMenuEx in User32
    EXPECT_EQ(source.find("WH_CBT"), std::string::npos);
    EXPECT_EQ(source.find("TrayMenuCbtProc"), std::string::npos);

    // WndProc must handle WM_INITMENUPOPUP to enforce topmost on the popup menu
    const std::string wndProcBody = source.substr(wndProc);
    EXPECT_NE(wndProcBody.find("message == WM_INITMENUPOPUP"), std::string::npos);
    EXPECT_NE(wndProcBody.find("FindWindowW(L\"#32768\", nullptr)"), std::string::npos);
    EXPECT_NE(wndProcBody.find("WM_RBUTTONUP || lParam == WM_CONTEXTMENU"), std::string::npos);
}
