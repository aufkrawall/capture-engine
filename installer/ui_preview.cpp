// Renders every wizard page to a bitmap without showing a window, so the look can
// be checked (and compared across changes) without installing anything.

#include "wizard.h"

#if !defined(CE_UNINSTALLER)

namespace ce::setup {

// ---------------------------------------------------------------------------
// Preview
// ---------------------------------------------------------------------------

namespace {

bool SaveWindowBitmap(HWND window, const std::wstring& path) {
    RECT client{};
    GetClientRect(window, &client);
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, client.right, client.bottom);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    // The page itself, then every visible control at its client position. WM_PRINT with
    // PRF_CHILDREN offsets children by the caption height when the frame is not printed.
    SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT);
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (!(GetWindowLongW(child, GWL_STYLE) & WS_VISIBLE))
            continue;
        RECT bounds{};
        GetWindowRect(child, &bounds);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&bounds), 2);
        SaveDC(memory);
        SetViewportOrgEx(memory, bounds.left, bounds.top, nullptr);
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT | PRF_ERASEBKGND);
        RestoreDC(memory, -1);
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = client.right;
    info.bmiHeader.biHeight = client.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 24;
    info.bmiHeader.biCompression = BI_RGB;
    const int stride = ((client.right * 3) + 3) & ~3;
    std::vector<uint8_t> pixels(static_cast<size_t>(stride) * static_cast<size_t>(client.bottom));
    GetDIBits(memory, bitmap, 0, static_cast<UINT>(client.bottom), pixels.data(), &info, DIB_RGB_COLORS);
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);

    BITMAPFILEHEADER header{};
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
    header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
    std::string file(reinterpret_cast<const char*>(&header), sizeof(header));
    file.append(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    file.append(reinterpret_cast<const char*>(pixels.data()), pixels.size());
    DWORD error = 0;
    return WriteFileAtomically(path, file.data(), file.size(), true, &error);
}

}  // namespace

int RunPreview(HINSTANCE instance, const std::wstring& directory) {
    Wizard& wizard = g_wizard;
    wizard.existing.registered = true;
    wizard.existing.directory = L"C:\\Program Files\\Capture Engine";
    wizard.existing.version = L"0.1.6800";
    wizard.existing.state.installed = true;
    wizard.directory = wizard.existing.directory;
    wizard.options = kFreshDefaults;
    if (!CreateWizardWindow(wizard, instance, L"Capture Engine Setup", true))
        return kExitFailed;
    CreateSetupControls(wizard, LicenseText(nullptr));
    wizard.removeData = CreateButton(wizard,
                                     L"Also delete my settings and logs\nRemoves config.ini and the logs folder. "
                                     L"Recordings are never deleted.",
                                     kIdRemoveData, BS_AUTOCHECKBOX);
    ShowWindow(wizard.window, SW_SHOWNOACTIVATE);

    struct Shot {
        const wchar_t* name;
        Page page;
    };
    const Shot shots[] = {{L"1-license", Page::License},      {L"2-location", Page::Location},
                          {L"3-options", Page::Options},      {L"4-remove", Page::ConfirmRemove},
                          {L"5-progress", Page::Progress},    {L"6-finish-ok", Page::Finish},
                          {L"7-finish-failed", Page::Finish}};
    int index = 0;
    for (const Shot& shot : shots) {
        wizard.page = shot.page;
        wizard.uninstall = shot.page == Page::ConfirmRemove;
        if (shot.page == Page::License)
            SetChecked(wizard.accept, true);
        if (shot.page == Page::Progress) {
            wizard.percent = 45;
            wizard.status = L"Copying capture_hook_x64.dll";
        }
        if (shot.page == Page::Finish) {
            wizard.resultLines.clear();
            wizard.workSucceeded = index == 5;
            if (wizard.workSucceeded) {
                wizard.resultTitle = L"Capture Engine was updated";
                wizard.resultColor = ui::kCaution;
                AddResultLine(wizard, ui::kText, std::wstring(L"Version ") + kVersion + L" is installed in " + wizard.directory + L".");
                AddResultLine(wizard, ui::kTextSecondary,
                        L"Your config.ini was kept as it was. The defaults of this version are in config.ini.new in "
                        L"the same folder, so you can compare them and copy new options over.");
                AddResultLine(wizard, ui::kCaution,
                        L"The PawnIO driver could not be installed (code 1). CPU temperature and power readings stay "
                        L"unavailable until it is installed.");
                SetChecked(wizard.launch, true);
            } else {
                wizard.resultTitle = L"Setup did not finish";
                wizard.resultColor = ui::kCritical;
                AddResultLine(wizard, ui::kText, L"capture_hook_x64.dll is in use and cannot be replaced: Access is denied. (error 5)");
                AddResultLine(wizard, ui::kTextSecondary, L"Your previous installation, if any, was left as it was.");
            }
        }
        if (shot.page == Page::Location && index == 1)
            SetWindowTextW(wizard.path, L"C:\\Program Files\\Capture Engine");
        UpdatePageControls(wizard);
        RefreshDirectoryNote(wizard);
        UpdateWindow(wizard.window);
        SaveWindowBitmap(wizard.window, JoinPath(directory, std::wstring(L"page-") + shot.name + L".bmp"));
        ++index;
    }
    DestroyWindow(wizard.window);
    DestroyWizard(wizard);
    return kExitOk;
}

}  // namespace ce::setup
#endif  // !CE_UNINSTALLER
