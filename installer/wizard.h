#pragma once

// State and entry points of the setup / uninstall window (ui_wizard.cpp,
// ui_pages.cpp). One window per process.

#include "ui.h"

namespace ce::setup {

enum class Page { License, Location, Options, ConfirmRemove, Progress, Finish };

enum ControlId {
    kIdAccept = 1001,
    kIdPath,
    kIdBrowse,
    kIdOptDesktop,
    kIdOptStartMenu,
    kIdOptAutostart,
    kIdOptService,
    kIdOptAdmin,
    kIdOptPawnIo,
    kIdLaunch,
    kIdRemoveData,
    kIdBack,
    kIdNext,
    kIdCancel,
    kIdLicense,
};

inline constexpr UINT kMsgProgress = WM_APP + 1;
inline constexpr UINT kMsgWorkDone = WM_APP + 2;

// Logical (96 DPI) metrics.
inline constexpr int kClientWidth = 640;
inline constexpr int kClientHeight = 520;
inline constexpr int kMargin = 32;
inline constexpr int kHeaderHeight = 112;
inline constexpr int kFooterHeight = 72;

struct ResultLine {
    COLORREF color;
    std::wstring text;
};

struct Wizard {
    HWND window = nullptr;
    HINSTANCE instance = nullptr;
    UINT dpi = 96;
    bool uninstall = false;
    // Testing: off-screen window, file operations only (see --files-only).
    bool filesOnly = false;
    unsigned closeTimeoutSeconds = 30;
    std::vector<DWORD> waitingInstallers;
    Page page = Page::License;

    HWND license = nullptr;
    HWND accept = nullptr;
    HWND path = nullptr;
    HWND browse = nullptr;
    HWND optDesktop = nullptr;
    HWND optStartMenu = nullptr;
    HWND optAutostart = nullptr;
    HWND optService = nullptr;
    HWND optAdmin = nullptr;
    HWND optPawnIo = nullptr;
    HWND launch = nullptr;
    HWND removeData = nullptr;
    HWND back = nullptr;
    HWND next = nullptr;
    HWND cancel = nullptr;
    HBRUSH surfaceBrush = nullptr;
    HICON icon = nullptr;

    PayloadReader* payload = nullptr;
    ExistingInstall existing;
    std::wstring directory;
    std::wstring directoryNote;
    bool directoryValid = true;
    bool pawnIoPresent = false;
    uint32_t options = 0;
    // Option defaults follow the folder the user picks; they are recomputed on the
    // folder page until the user has touched an option themselves.
    std::wstring defaultsDirectory;
    bool optionsTouched = false;
    uint32_t overrideOn = 0;
    uint32_t overrideOff = 0;

    HANDLE worker = nullptr;
    CRITICAL_SECTION lock;
    int percent = 0;
    std::wstring status;
    bool workSucceeded = false;
    InstallResult installResult;
    UninstallResult uninstallResult;
    std::vector<ResultLine> resultLines;
    std::wstring resultTitle;
    COLORREF resultColor = ui::kText;
    int exitCode = kExitCancelled;
};

extern Wizard g_wizard;

// ui_pages.cpp
void LayoutControls(Wizard& wizard);
void UpdatePageControls(Wizard& wizard);
void InvalidateProgress(Wizard& wizard);
void PaintWizard(Wizard& wizard, HDC dc, const RECT& client);

// ui_results.cpp
void AddResultLine(Wizard& wizard, COLORREF color, std::wstring text);
void BuildInstallResult(Wizard& wizard);
void BuildUninstallResult(Wizard& wizard);

// ui_wizard.cpp, shared with the preview (ui_preview.cpp)
void RefreshDirectoryNote(Wizard& wizard);
bool Checked(HWND control);
void SetChecked(HWND control, bool checked);
HWND CreateButton(Wizard& wizard, const wchar_t* text, int id, DWORD kind, COLORREF background = ui::kBackground);
bool CreateWizardWindow(Wizard& wizard, HINSTANCE instance, const std::wstring& title, bool offscreen);
void CreateSetupControls(Wizard& wizard, const std::wstring& licenseText);
std::wstring LicenseText(PayloadReader* payload);
void DestroyWizard(Wizard& wizard);

// ui_wizard.cpp
int RunSetupWizard(HINSTANCE instance, PayloadReader* payload, const CommandLine& command,
                   const std::wstring& defaultDirectory);
int RunUninstallWizard(HINSTANCE instance, const std::wstring& directory, bool removeDataDefault,
                       bool filesOnly = false, unsigned closeTimeoutSeconds = 30,
                       const std::vector<DWORD>& waitingInstallers = {});
// Renders every page to <directory>\page-N.bmp without showing a window.
int RunPreview(HINSTANCE instance, const std::wstring& directory);
void ShowMessage(HWND owner, const std::wstring& text, const std::wstring& caption, bool error);

}  // namespace ce::setup
