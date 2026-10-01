// The setup / uninstall window: construction, navigation, the worker thread and
// the off-screen preview used to check the look without installing anything.

#include "wizard.h"

#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>

namespace ce::setup {

Wizard g_wizard;

namespace {

constexpr wchar_t kWindowClass[] = L"CaptureEngineSetupWindow";

struct OptionRow {
    int id;
    uint32_t bit;
    HWND Wizard::*control;
    const wchar_t* text;
};

const OptionRow kOptionRows[] = {
    {kIdOptDesktop, kOptDesktopShortcut, &Wizard::optDesktop, L"Create a desktop shortcut"},
    {kIdOptStartMenu, kOptStartMenuShortcut, &Wizard::optStartMenu, L"Create a Start menu shortcut"},
    {kIdOptAutostart, kOptAutostart, &Wizard::optAutostart,
     L"Start Capture Engine when I sign in\nKeeps hotkeys and the overlay ready before a game starts."},
    {kIdOptService, kOptService, &Wizard::optService,
     L"Install the elevation service\nReads CPU sensors and display timing without running as administrator."},
    {kIdOptAdmin, kOptElevated, &Wizard::optAdmin,
     L"Always run Capture Engine as administrator\nAsks for permission at every launch. Only some games and tools need it."},
    {kIdOptPawnIo, kOptPawnIo, &Wizard::optPawnIo,
     L"Install the PawnIO driver\nSigned driver for CPU temperature and power readings (optional)."},
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

std::wstring EditText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(std::max(length, 0)) + 1, L'\0');
    const int copied = GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
    text.resize(static_cast<size_t>(std::max(copied, 0)));
    return text;
}

}  // namespace

bool Checked(HWND control) {
    return control && SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void SetChecked(HWND control, bool checked) {
    if (control)
        SendMessageW(control, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

namespace {

std::wstring FormatSize(uint64_t bytes) {
    wchar_t text[48];
    if (bytes >= (1ull << 30))
        swprintf(text, 48, L"%.1f GB", static_cast<double>(bytes) / static_cast<double>(1ull << 30));
    else
        swprintf(text, 48, L"%llu MB", static_cast<unsigned long long>((bytes + (1u << 20) - 1) >> 20));
    return text;
}

}  // namespace

HWND CreateButton(Wizard& wizard, const wchar_t* text, int id, DWORD kind, COLORREF background) {
    HWND button = CreateWindowExW(0, ui::kButtonClass, text, WS_CHILD | WS_TABSTOP | kind, 0, 0, 10, 10, wizard.window,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), wizard.instance, nullptr);
    SendMessageW(button, ui::CEM_SETBACKGROUND, 0, static_cast<LPARAM>(background));
    return button;
}

namespace {

void ApplyFonts(Wizard& wizard) {
    for (HWND control : {wizard.license, wizard.path}) {
        if (control)
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(control == wizard.license ? ui::g_fonts.mono
                                                                                                  : ui::g_fonts.body),
                         TRUE);
    }
}

bool IsOptionId(int id) {
    for (const OptionRow& row : kOptionRows) {
        if (row.id == id)
            return true;
    }
    return false;
}

// Reflects `wizard.options` in the option checkboxes (the driver row stays as it is
// when the driver is already installed).
void ApplyOptionChecks(Wizard& wizard) {
    for (const OptionRow& row : kOptionRows) {
        HWND control = wizard.*row.control;
        if (control && !(row.bit == kOptPawnIo && wizard.pawnIoPresent))
            SetChecked(control, (wizard.options & row.bit) != 0);
    }
}

void LoadIcon(Wizard& wizard) {
    if (wizard.icon)
        DestroyIcon(wizard.icon);
    const int size = ui::Scale(wizard.dpi, 40);
    wizard.icon = static_cast<HICON>(LoadImageW(wizard.instance, MAKEINTRESOURCEW(kIconResource), IMAGE_ICON, size, size,
                                                LR_DEFAULTCOLOR));
}

// ---------------------------------------------------------------------------
// Install folder page
// ---------------------------------------------------------------------------

}  // namespace

void RefreshDirectoryNote(Wizard& wizard) {
    if (!wizard.path)
        return;
    const std::wstring directory = NormalizeDirectory(EditText(wizard.path));
    const DirectoryIssue issue = ValidateInstallDirectory(directory, WindowsDirectory());
    wizard.directoryValid = issue == DirectoryIssue::Ok;
    if (!wizard.directoryValid) {
        wizard.directoryNote = Widen(DirectoryIssueText(issue));
    } else {
        std::wstring note;
#if !defined(CE_UNINSTALLER)
        const uint64_t needed = wizard.payload ? wizard.payload->TotalSize() : 0;
#else
        const uint64_t needed = 0;
#endif
        if (needed)
            note += L"Space required: " + FormatSize(needed) + L"\n";
        ULARGE_INTEGER freeBytes{};
        const std::wstring anchor = NearestExistingDirectory(directory);
        if (!anchor.empty() && GetDiskFreeSpaceExW(anchor.c_str(), &freeBytes, nullptr, nullptr)) {
            note += L"Free space on " + anchor.substr(0, 2) + L" " + FormatSize(freeBytes.QuadPart) + L"\n";
            if (needed && freeBytes.QuadPart < needed + (64ull << 20)) {
                wizard.directoryValid = false;
                note += L"\nThere is not enough free space on that drive.";
            }
        }
        if (wizard.directoryValid) {
            if (PathExists(JoinPath(directory, kAppExe)))
                note += L"\nCapture Engine is already installed in this folder. It will be updated and your "
                        L"config.ini stays untouched.";
            else if (wizard.existing.registered &&
                     !EqualsNoCase(NormalizeDirectory(wizard.existing.directory), directory))
                note += L"\nCapture Engine is currently installed in " + wizard.existing.directory +
                        L". Setup will move it to this folder.";
        }
        wizard.directoryNote = note;
    }
    if (wizard.next && wizard.page == Page::Location)
        EnableWindow(wizard.next, wizard.directoryValid);
    InvalidateRect(wizard.window, nullptr, FALSE);
}

namespace {

void BrowseForFolder(Wizard& wizard) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return;
    DWORD flags = 0;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Choose the Capture Engine folder");
    if (SUCCEEDED(dialog->Show(wizard.window))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                std::wstring chosen = NormalizeDirectory(path);
                // A picked folder that is not ours becomes the parent of a "Capture Engine"
                // folder, so the files never land loose in Documents or on a drive's top level.
                if (!PathExists(JoinPath(chosen, kAppExe)) && !EqualsNoCase(LeafOf(chosen), kInstallFolderName))
                    chosen = JoinPath(chosen, kInstallFolderName);
                SetWindowTextW(wizard.path, chosen.c_str());
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

void ReportProgress(Wizard& wizard, int percent, const std::wstring& status) {
    EnterCriticalSection(&wizard.lock);
    if (percent >= 0)
        wizard.percent = std::max(wizard.percent, percent);
    wizard.status = status;
    LeaveCriticalSection(&wizard.lock);
    PostMessageW(wizard.window, kMsgProgress, 0, 0);
}

DWORD WINAPI WorkerThread(LPVOID parameter) {
    Wizard& wizard = *static_cast<Wizard*>(parameter);
    const ProgressFn progress = [&wizard](int percent, const std::wstring& status) {
        ReportProgress(wizard, percent, status);
    };
    if (wizard.uninstall) {
        UninstallRequest request;
        request.directory = wizard.directory;
        request.removeUserData = (wizard.options & kOptRemoveData) != 0;
        request.filesOnly = wizard.filesOnly;
        request.closeTimeoutSeconds = wizard.closeTimeoutSeconds;
        wizard.uninstallResult = RunUninstall(request, progress);
    }
#if !defined(CE_UNINSTALLER)
    else if (wizard.payload) {
        InstallRequest request;
        request.directory = wizard.directory;
        request.options = wizard.options;
        request.filesOnly = wizard.filesOnly;
        request.closeTimeoutSeconds = wizard.closeTimeoutSeconds;
        wizard.installResult = RunInstall(*wizard.payload, request, progress);
    }
#endif
    PostMessageW(wizard.window, kMsgWorkDone, 0, 0);
    return 0;
}

void StartWork(Wizard& wizard) {
    wizard.page = Page::Progress;
    wizard.percent = 0;
    wizard.status = L"Starting...";
    UpdatePageControls(wizard);
    wizard.worker = CreateThread(nullptr, 0, WorkerThread, &wizard, 0, nullptr);
    if (!wizard.worker) {
        wizard.installResult.error = L"Setup could not start its worker thread: " + ErrorText(GetLastError());
        wizard.uninstallResult.error = wizard.installResult.error;
        if (wizard.uninstall)
            BuildUninstallResult(wizard);
        else
            BuildInstallResult(wizard);
        wizard.page = Page::Finish;
        UpdatePageControls(wizard);
    }
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

void GoBack(Wizard& wizard) {
    if (wizard.page == Page::Options)
        wizard.page = Page::Location;
    else if (wizard.page == Page::Location)
        wizard.page = Page::License;
    UpdatePageControls(wizard);
}

void GoNext(Wizard& wizard) {
    switch (wizard.page) {
    case Page::License:
        if (!Checked(wizard.accept))
            return;
        wizard.page = Page::Location;
        RefreshDirectoryNote(wizard);
        break;
    case Page::Location:
        RefreshDirectoryNote(wizard);
        if (!wizard.directoryValid)
            return;
        wizard.directory = NormalizeDirectory(EditText(wizard.path));
        if (!EqualsNoCase(wizard.directory, wizard.defaultsDirectory)) {
            // A different folder may hold (or not hold) an installation: what is on offer
            // follows that folder, unless the user already made their own choices.
            wizard.existing = ReadExistingInstall(wizard.directory);
            wizard.pawnIoPresent = wizard.existing.state.pawnIoInstalled;
            if (!wizard.optionsTouched) {
                wizard.options = (DefaultOptions(wizard.existing.state) | wizard.overrideOn) & ~wizard.overrideOff;
                ApplyOptionChecks(wizard);
            }
            wizard.defaultsDirectory = wizard.directory;
        }
        wizard.page = Page::Options;
        break;
    case Page::Options: {
        uint32_t options = wizard.options & (kOptLaunch | kOptRemoveData);
        for (const OptionRow& row : kOptionRows) {
            if (Checked(wizard.*row.control))
                options |= row.bit;
        }
        wizard.options = options;
        StartWork(wizard);
        return;
    }
    case Page::ConfirmRemove:
        wizard.options = Checked(wizard.removeData) ? kOptRemoveData : 0;
        StartWork(wizard);
        return;
    case Page::Progress:
        return;
    case Page::Finish:
        if (!wizard.filesOnly && !wizard.uninstall && wizard.workSucceeded && Checked(wizard.launch)) {
            if (LaunchAsInteractiveUser(JoinPath(wizard.directory, kAppExe), L"", wizard.directory))
                Log("launch: started Capture Engine");
            else
                Log("launch: could not start Capture Engine");
        }
        wizard.exitCode = wizard.workSucceeded ? kExitOk : kExitFailed;
        DestroyWindow(wizard.window);
        return;
    }
    UpdatePageControls(wizard);
}

void CloseWindow(Wizard& wizard) {
    if (wizard.page == Page::Progress)
        return;
    if (wizard.page == Page::Finish)
        wizard.exitCode = wizard.workSucceeded ? kExitOk : kExitFailed;
    else
        wizard.exitCode = kExitCancelled;
    DestroyWindow(wizard.window);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    Wizard& wizard = g_wizard;
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        HDC buffer = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ previous = SelectObject(buffer, bitmap);
        PaintWizard(wizard, buffer, client);
        BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, previous);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_PRINTCLIENT: {
        RECT client{};
        GetClientRect(window, &client);
        PaintWizard(wizard, reinterpret_cast<HDC>(wParam), client);
        return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, ui::kText);
        SetBkColor(dc, ui::kSurface);
        return reinterpret_cast<LRESULT>(wizard.surfaceBrush);
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);
        if (id == kIdPath && code == EN_CHANGE) {
            RefreshDirectoryNote(wizard);
            return 0;
        }
        if (code != BN_CLICKED)
            break;
        if (IsOptionId(id))
            wizard.optionsTouched = true;
        switch (id) {
        case kIdAccept:
            UpdatePageControls(wizard);
            break;
        case kIdBrowse:
            BrowseForFolder(wizard);
            break;
        case kIdBack:
            GoBack(wizard);
            break;
        case kIdNext:
            GoNext(wizard);
            break;
        case kIdCancel:
            CloseWindow(wizard);
            break;
        default:
            break;
        }
        return 0;
    }
    case kMsgProgress:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case kMsgWorkDone:
        if (wizard.worker) {
            WaitForSingleObject(wizard.worker, INFINITE);
            CloseHandle(wizard.worker);
            wizard.worker = nullptr;
        }
        if (wizard.uninstall)
            BuildUninstallResult(wizard);
        else
            BuildInstallResult(wizard);
        wizard.page = Page::Finish;
        SetChecked(wizard.launch, (wizard.options & kOptLaunch) != 0);
        UpdatePageControls(wizard);
        return 0;
    case WM_DPICHANGED: {
        wizard.dpi = HIWORD(wParam);
        ui::CreateFonts(wizard.dpi);
        ApplyFonts(wizard);
        LoadIcon(wizard);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested)
            SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        UpdatePageControls(wizard);
        return 0;
    }
    case WM_CLOSE:
        CloseWindow(wizard);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

}  // namespace

bool CreateWizardWindow(Wizard& wizard, HINSTANCE instance, const std::wstring& title, bool offscreen) {
    wizard.instance = instance;
    InitializeCriticalSection(&wizard.lock);
    wizard.surfaceBrush = CreateSolidBrush(ui::kSurface);
    wizard.dpi = ui::WindowDpi(nullptr);
    ui::CreateFonts(wizard.dpi);
    if (!ui::RegisterButtonClass(instance))
        return false;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(kIconResource));
    windowClass.hIconSm = windowClass.hIcon;
    if (!RegisterClassExW(&windowClass)) {
        Log("ui: RegisterClassEx failed (error %lu)", GetLastError());
        return false;
    }
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT desired = {0, 0, ui::Scale(wizard.dpi, kClientWidth), ui::Scale(wizard.dpi, kClientHeight)};
    AdjustWindowRectEx(&desired, style, FALSE, offscreen ? WS_EX_TOOLWINDOW : 0);
    const int width = desired.right - desired.left;
    const int height = desired.bottom - desired.top;
    const int left = offscreen ? -32000 : (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int top = offscreen ? -32000 : (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    wizard.window = CreateWindowExW(offscreen ? WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE : 0, kWindowClass, title.c_str(),
                                    style, left, top, width, height, nullptr, nullptr, instance, nullptr);
    if (!wizard.window) {
        Log("ui: CreateWindowEx failed (error %lu)", GetLastError());
        return false;
    }
    // The monitor the window landed on may differ from the primary one.
    const UINT actual = ui::WindowDpi(wizard.window);
    if (actual != wizard.dpi) {
        wizard.dpi = actual;
        ui::CreateFonts(actual);
    }
    LoadIcon(wizard);
    ui::ApplyDarkTitleBar(wizard.window);

    wizard.back = CreateButton(wizard, L"Back", kIdBack, BS_PUSHBUTTON, ui::kFooter);
    wizard.next = CreateButton(wizard, L"Next", kIdNext, BS_DEFPUSHBUTTON, ui::kFooter);
    wizard.cancel = CreateButton(wizard, L"Cancel", kIdCancel, BS_PUSHBUTTON, ui::kFooter);
    return true;
}

namespace {

}  // namespace

#if !defined(CE_UNINSTALLER)
void CreateSetupControls(Wizard& wizard, const std::wstring& licenseText) {
    wizard.license = CreateWindowExW(0, L"EDIT", licenseText.c_str(),
                                     WS_CHILD | WS_VSCROLL | WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                     0, 0, 10, 10, wizard.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdLicense)),
                                     wizard.instance, nullptr);
    ui::EnableDarkScrollbars(wizard.license);
    wizard.accept = CreateButton(wizard, L"I accept the license terms", kIdAccept, BS_AUTOCHECKBOX);
    wizard.path = CreateWindowExW(0, L"EDIT", wizard.directory.c_str(), WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10,
                                  10, wizard.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdPath)),
                                  wizard.instance, nullptr);
    SendMessageW(wizard.path, EM_LIMITTEXT, 200, 0);
    wizard.browse = CreateButton(wizard, L"Browse...", kIdBrowse, BS_PUSHBUTTON);
    for (const OptionRow& row : kOptionRows) {
        HWND control = CreateButton(wizard, row.text, row.id, BS_AUTOCHECKBOX);
        wizard.*row.control = control;
        SetChecked(control, (wizard.options & row.bit) != 0);
    }
    if (wizard.pawnIoPresent) {
        // Already installed: shown ticked and locked rather than hidden.
        SetWindowTextW(wizard.optPawnIo,
                       L"The PawnIO driver is already installed\nCPU temperature and power readings are available.");
        SetChecked(wizard.optPawnIo, true);
        EnableWindow(wizard.optPawnIo, FALSE);
    }
    wizard.launch = CreateButton(wizard, L"Start Capture Engine now", kIdLaunch, BS_AUTOCHECKBOX);
    ApplyFonts(wizard);
}
#endif

namespace {

int RunLoop(Wizard& wizard) {
    // The testing mode keeps the window off-screen and never takes focus.
    ShowWindow(wizard.window, wizard.filesOnly ? SW_SHOWNOACTIVATE : SW_SHOW);
    UpdateWindow(wizard.window);
    if (!wizard.filesOnly)
        SetForegroundWindow(wizard.window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        const bool ours = message.hwnd == wizard.window || IsChild(wizard.window, message.hwnd);
        if (ours && message.message == WM_KEYDOWN && wizard.page != Page::Progress) {
            wchar_t className[32] = {};
            GetClassNameW(message.hwnd, className, 32);
            const bool multiline = GetWindowLongW(message.hwnd, GWL_STYLE) & ES_MULTILINE;
            if (message.wParam == VK_ESCAPE) {
                SendMessageW(wizard.window, WM_COMMAND, MAKEWPARAM(kIdCancel, BN_CLICKED), 0);
                continue;
            }
            if (message.wParam == VK_RETURN && !(wcscmp(className, L"Edit") == 0 && multiline) &&
                !(wcscmp(className, ui::kButtonClass) == 0 && message.hwnd != wizard.next &&
                  (GetWindowLongW(message.hwnd, GWL_STYLE) & BS_TYPEMASK) != BS_AUTOCHECKBOX)) {
                if (IsWindowEnabled(wizard.next) && IsWindowVisible(wizard.next))
                    SendMessageW(wizard.window, WM_COMMAND, MAKEWPARAM(kIdNext, BN_CLICKED), 0);
                continue;
            }
        }
        if (!ours || !IsDialogMessageW(wizard.window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return wizard.exitCode;
}

}  // namespace

void DestroyWizard(Wizard& wizard) {
    ui::DestroyFonts();
    if (wizard.surfaceBrush)
        DeleteObject(wizard.surfaceBrush);
    if (wizard.icon)
        DestroyIcon(wizard.icon);
    DeleteCriticalSection(&wizard.lock);
}

#if !defined(CE_UNINSTALLER)
std::wstring LicenseText(PayloadReader* payload) {
    std::string contents;
    DWORD error = 0;
    if (payload) {
        const FileEntry* entry = payload->Find("licenses/MIT_CaptureEngine.txt");
        if (!entry)
            entry = payload->Find("LICENSE");
        if (entry && payload->ReadContents(*entry, &contents, &error) != PayloadStatus::Ok)
            contents.clear();
    }
    if (contents.empty())
        contents = "MIT License\n\nCopyright (c) 2026 aufkrawall";
    // The edit control wants CR LF.
    std::wstring wide = Widen(contents);
    std::wstring converted;
    converted.reserve(wide.size() + wide.size() / 40);
    for (size_t index = 0; index < wide.size(); ++index) {
        if (wide[index] == L'\n' && (index == 0 || wide[index - 1] != L'\r'))
            converted.push_back(L'\r');
        converted.push_back(wide[index]);
    }
    return converted;
}
#endif

void ShowMessage(HWND owner, const std::wstring& text, const std::wstring& caption, bool error) {
    MessageBoxW(owner, text.c_str(), caption.c_str(),
                MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION) | MB_SETFOREGROUND);
}

#if !defined(CE_UNINSTALLER)
int RunSetupWizard(HINSTANCE instance, PayloadReader* payload, const CommandLine& command,
                   const std::wstring& defaultDirectory) {
    Wizard& wizard = g_wizard;
    wizard.payload = payload;
    // Where to start: the command line, else the folder of our own earlier installation, else the default.
    const ExistingInstall probe = ReadExistingInstall(defaultDirectory);
    wizard.directory = !command.directory.empty() ? NormalizeDirectory(command.directory)
                       : probe.registered && DirectoryExists(probe.directory) ? NormalizeDirectory(probe.directory)
                                                                              : defaultDirectory;
    wizard.existing = ReadExistingInstall(wizard.directory);
    wizard.defaultsDirectory = wizard.directory;
    wizard.overrideOn = command.optionsOn;
    wizard.overrideOff = command.optionsOff;
    wizard.pawnIoPresent = wizard.existing.state.pawnIoInstalled;
    wizard.options = ApplyOverrides(DefaultOptions(wizard.existing.state), command);
    wizard.filesOnly = command.filesOnly;
    wizard.closeTimeoutSeconds = command.closeTimeoutSeconds;
    if (!CreateWizardWindow(wizard, instance, std::wstring(kProductName) + L" Setup", command.filesOnly))
        return kExitFailed;
    CreateSetupControls(wizard, LicenseText(payload));
    wizard.page = Page::License;
    RefreshDirectoryNote(wizard);
    UpdatePageControls(wizard);
    const int code = RunLoop(wizard);
    DestroyWizard(wizard);
    return code;
}

#endif  // !CE_UNINSTALLER

int RunUninstallWizard(HINSTANCE instance, const std::wstring& directory, bool removeDataDefault, bool filesOnly,
                       unsigned closeTimeoutSeconds) {
    Wizard& wizard = g_wizard;
    wizard.closeTimeoutSeconds = closeTimeoutSeconds;
    wizard.uninstall = true;
    wizard.filesOnly = filesOnly;
    wizard.directory = directory;
    if (!CreateWizardWindow(wizard, instance, L"Uninstall Capture Engine", filesOnly))
        return kExitFailed;
    wizard.removeData = CreateButton(wizard,
                                     L"Also delete my settings and logs\nRemoves config.ini and the logs folder. "
                                     L"Recordings are never deleted.",
                                     kIdRemoveData, BS_AUTOCHECKBOX);
    SetChecked(wizard.removeData, removeDataDefault);
    wizard.page = Page::ConfirmRemove;
    UpdatePageControls(wizard);
    const int code = RunLoop(wizard);
    DestroyWizard(wizard);
    return code;
}


}  // namespace ce::setup
