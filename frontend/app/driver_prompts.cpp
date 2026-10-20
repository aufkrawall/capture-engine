// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "frontend/app/driver_prompts.h"

#include <commctrl.h>
#include <shellapi.h>

#include <cstddef>
#include <string>

namespace ce::frontend {
namespace {

constexpr int kInstallButtonId = 101;
constexpr int kNotNowButtonId = 102;
constexpr int kNeverButtonId = 103;
constexpr int kRestartButtonId = 201;
constexpr int kLaterButtonId = 202;

using TaskDialogIndirectFunction = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);

// comctl32's TaskDialogIndirect, loaded on demand so the frontend does not link the common controls version.
TaskDialogIndirectFunction LoadTaskDialog() {
    HMODULE controls = LoadLibraryExW(L"comctl32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return controls ? reinterpret_cast<TaskDialogIndirectFunction>(GetProcAddress(controls, "TaskDialogIndirect"))
                    : nullptr;
}

bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation = {};
    DWORD returned = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned) != FALSE &&
                          elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

// Relaunches this executable elevated with the restart argument, then closes this instance (the tray's WM_CLOSE when the
// tray window exists, otherwise a plain exit).
void RestartAsAdministrator() {
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size())
        return;
    executable.resize(length);
    const std::wstring directory = executable.substr(0, executable.find_last_of(L"\\/"));
    const std::wstring params = L"--restart-from-pid=" + std::to_wstring(GetCurrentProcessId());
    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.lpParameters = params.c_str();
    info.lpDirectory = directory.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info))
        return;
    HWND trayWindow = FindWindowA("CaptureEngineTray", nullptr);
    if (trayWindow)
        PostMessageA(trayWindow, WM_CLOSE, 0, 0);
    else
        ExitProcess(0);
}

// The startup offer's task dialog. 0: the task dialog is unavailable.
int AskWithTaskDialog() {
    TaskDialogIndirectFunction taskDialog = LoadTaskDialog();
    if (!taskDialog)
        return 0;
    const TASKDIALOG_BUTTON buttons[] = {
        {kInstallButtonId, L"Install PawnIO\nRuns the official Microsoft-signed installer bundled with CaptureEngine."},
        {kNotNowButtonId, L"Not now\nAsk again the next time CaptureEngine starts."},
        {kNeverButtonId, L"Don't ask again\nKeep CPU sensors unavailable and stop asking."},
    };
    TASKDIALOGCONFIG config = {};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.pszWindowTitle = L"CaptureEngine";
    config.pszMainIcon = TD_INFORMATION_ICON;
    config.pszMainInstruction = L"Install PawnIO for CPU temperature, power and clock sensors?";
    config.pszContent =
        L"CaptureEngine can show CPU temperature, package power and core clocks in the overlay, but "
        L"LibreHardwareMonitor reads those through PawnIO - a separate, Microsoft-signed kernel driver that is not "
        L"installed on this PC.\n\n"
        L"CaptureEngine bundles the official Microsoft-signed PawnIO installer. Choosing to install runs it "
        L"locally and asks for administrator approval once. GPU sensors work either way.";
    config.cButtons = static_cast<UINT>(std::size(buttons));
    config.pButtons = buttons;
    config.nDefaultButton = kNotNowButtonId;
    int pressed = 0;
    if (FAILED(taskDialog(&config, &pressed, nullptr, nullptr)))
        return 0;
    return pressed;
}

// The startup offer's message box, used when the task dialog is unavailable. 0: no answer.
int AskWithMessageBox() {
    const int answer = MessageBoxW(
        nullptr,
        L"CaptureEngine can show CPU temperature, package power and core clocks, but LibreHardwareMonitor reads "
        L"those through PawnIO - a separate, Microsoft-signed kernel driver that is not installed on this PC.\n\n"
        L"CaptureEngine bundles the official Microsoft-signed installer for optional offline setup.\n\n"
        L"Yes\tInstall it now (asks for administrator approval once).\n"
        L"No\tNot now; ask again next time.\n"
        L"Cancel\tDon't ask again.\n\n"
        L"GPU sensors work either way.",
        L"CaptureEngine", MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_SETFOREGROUND);
    if (answer == IDYES)
        return kInstallButtonId;
    if (answer == IDNO)
        return kNotNowButtonId;
    if (answer == IDCANCEL)
        return kNeverButtonId;
    return 0;
}

}  // namespace

OfferAnswer AskDriverOffer() {
    int choice = AskWithTaskDialog();
    if (choice == 0)
        choice = AskWithMessageBox();
    if (choice == kInstallButtonId)
        return OfferAnswer::Install;
    if (choice == kNeverButtonId)
        return OfferAnswer::DontAskAgain;
    return OfferAnswer::NotNow;
}

void OfferRestartAsAdministrator(bool serviceEnabled) {
    if (serviceEnabled) {
        MessageBoxW(nullptr,
                    L"PawnIO was installed successfully.\n\n"
                    L"The elevation service will reconnect the hardware sensors automatically.",
                    L"CaptureEngine", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return;
    }
    if (IsProcessElevated()) {
        MessageBoxW(nullptr,
                    L"PawnIO was installed successfully.\n\n"
                    L"CPU temperature, package power and core clocks are now available.",
                    L"CaptureEngine", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return;
    }
    int choice = 0;
    if (TaskDialogIndirectFunction taskDialog = LoadTaskDialog()) {
        const TASKDIALOG_BUTTON buttons[] = {
            {kRestartButtonId,
             L"Restart as Administrator now\nCloses CaptureEngine and relaunches it elevated so CPU sensors work immediately."},
            {kLaterButtonId,
             L"Later\nKeep running without elevation. CPU sensors will remain unavailable until restarted as admin."},
        };
        TASKDIALOGCONFIG config = {};
        config.cbSize = sizeof(config);
        config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        config.pszWindowTitle = L"CaptureEngine";
        config.pszMainIcon = TD_INFORMATION_ICON;
        config.pszMainInstruction = L"PawnIO was installed successfully";
        config.pszContent =
            L"LibreHardwareMonitor requires administrator privileges to read CPU temperature, package power and core clocks.\n\n"
            L"Would you like to restart CaptureEngine as administrator now?";
        config.cButtons = static_cast<UINT>(std::size(buttons));
        config.pButtons = buttons;
        config.nDefaultButton = kRestartButtonId;
        taskDialog(&config, &choice, nullptr, nullptr);
    } else {
        const int res = MessageBoxW(
            nullptr,
            L"PawnIO was installed successfully.\n\n"
            L"LibreHardwareMonitor requires administrator privileges to read CPU temperature, package power and core clocks.\n\n"
            L"Restart CaptureEngine as administrator now?",
            L"CaptureEngine", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1 | MB_SETFOREGROUND);
        choice = (res == IDYES) ? kRestartButtonId : kLaterButtonId;
    }
    if (choice == kRestartButtonId)
        RestartAsAdministrator();
}

void ShowDriverAlreadyInstalled() {
    MessageBoxW(nullptr, L"The PawnIO driver is already installed on this machine.", L"CaptureEngine",
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

void ShowDriverInstallNotCompleted() {
    MessageBoxW(nullptr, L"PawnIO driver installation was cancelled or did not complete.", L"CaptureEngine",
                MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
}

void ShowDriverNotInstalled() {
    MessageBoxW(nullptr, L"The PawnIO driver is not installed on this machine.", L"CaptureEngine",
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

bool ConfirmDriverUninstall() {
    return MessageBoxW(nullptr,
                       L"Uninstall the PawnIO kernel driver?\n\n"
                       L"PawnIO is a shared system component. Other hardware monitoring tools on your PC "
                       L"(e.g. FanControl, LibreHardwareMonitor) may also rely on it.\n\n"
                       L"Are you sure you want to uninstall PawnIO?",
                       L"CaptureEngine", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND) == IDYES;
}

void ShowDriverRemoved() {
    MessageBoxW(nullptr, L"PawnIO driver was successfully uninstalled.", L"CaptureEngine",
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

void ShowDriverUninstallNotCompleted() {
    MessageBoxW(nullptr, L"PawnIO driver uninstallation was cancelled or did not complete.", L"CaptureEngine",
                MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
}

void ShowSettingFailed(const wchar_t* text) {
    MessageBoxW(nullptr, text, L"CaptureEngine", MB_OK | MB_ICONERROR);
}

}  // namespace ce::frontend
