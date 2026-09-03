#include "tray.h"
#include "../common/logging.h"
#include <shellapi.h>

static constexpr UINT_PTR BLINK_TIMER_ID = 1001;
static constexpr UINT BLINK_INTERVAL_MS = 500;

TrayIcon::TrayIcon(HINSTANCE hInstance, Callbacks callbacks)
    : hInstance(hInstance),
      callbacks(std::move(callbacks)) {
    InitWindow();
    InitIcon();
}

TrayIcon::TrayIcon(HINSTANCE hInstance, std::function<void()> onQuit, std::function<void()> onOpenConfig)
    : hInstance(hInstance) {
    callbacks.onQuit = std::move(onQuit);
    callbacks.onOpenConfig = std::move(onOpenConfig);
    InitWindow();
    InitIcon();
}

TrayIcon::~TrayIcon() {
    Remove();
    if (hWnd)
        DestroyWindow(hWnd);
}

void TrayIcon::InitWindow() {
    taskbarCreatedMessage = RegisterWindowMessageA("TaskbarCreated");
    if (taskbarCreatedMessage == 0)
        LogWarn("[Tray] Failed to register Explorer taskbar recreation message (error=%lu)", GetLastError());

    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "CaptureEngineTray";

    RegisterClassExA(&wc);

    // Use a hidden top-level tool window instead of a message-only window so
    // Explorer can observe a real UI window during launch and clear the startup
    // wait cursor promptly.
    hWnd = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "CaptureEngineTray", "CaptureEngine", WS_POPUP, 0, 0, 0,
                           0, NULL, NULL, hInstance, this);
    if (!hWnd) {
        LogError("[Tray] Failed to create notification window (error=%lu)", GetLastError());
        return;
    }
    ShowWindow(hWnd, SW_HIDE);
    UpdateWindow(hWnd);
}

void TrayIcon::InitIcon() {
    hIconIdle =
        (HICON)LoadImageA(hInstance, MAKEINTRESOURCEA(IDI_ICON_IDLE), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    hIconRecording = (HICON)LoadImageA(hInstance, MAKEINTRESOURCEA(IDI_ICON_RECORDING), IMAGE_ICON, 0, 0,
                                       LR_DEFAULTSIZE | LR_SHARED);
    hIconShutdown =
        (HICON)LoadImageA(hInstance, MAKEINTRESOURCEA(IDI_ICON_SHUTDOWN), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);

    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(NOTIFYICONDATAA);
    nid.hWnd = hWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = hIconIdle ? hIconIdle : LoadIcon(NULL, IDI_APPLICATION);
    strcpy_s(nid.szTip, "Capture Engine");
    iconInitialized = true;

    SetLastError(ERROR_SUCCESS);
    if (!Shell_NotifyIconA(NIM_ADD, &nid))
        LogError("[Tray] Failed to add tray icon (error=%lu)", GetLastError());
    else
        LogInfo("[Tray] Tray icon added");
}

void TrayIcon::RestoreAfterTaskbarCreated() {
    if (iconRemovalRequested || !iconInitialized)
        return;

    LogInfo("[Tray] Explorer taskbar was recreated; restoring tray icon");
    SetLastError(ERROR_SUCCESS);
    if (!Shell_NotifyIconA(NIM_ADD, &nid)) {
        LogError("[Tray] Failed to restore tray icon after taskbar recreation (error=%lu)", GetLastError());
        return;
    }
    LogInfo("[Tray] Tray icon restored after taskbar recreation");
}

void TrayIcon::SetRecordingState(bool recording) {
    nid.hIcon = (recording && hIconRecording) ? hIconRecording : hIconIdle;
    if (!nid.hIcon)
        nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);

    strcpy_s(nid.szTip, recording ? "Capture Engine (Recording...)" : "Capture Engine");
    Shell_NotifyIconA(NIM_MODIFY, &nid);
}

void TrayIcon::ShowContextMenu() {
    if (shuttingDown || !hWnd)
        return;

    POINT pt;
    if (!GetCursorPos(&pt))
        return;

    SetForegroundWindow(hWnd);

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu)
        return;

    constexpr UINT kIdOpenConfig = 1001;
    constexpr UINT kIdInstallPawnIo = 1002;
    constexpr UINT kIdUninstallPawnIo = 1003;
    constexpr UINT kIdClose = 1004;

    AppendMenuW(hMenu, MF_STRING, kIdOpenConfig, L"Open config");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    const bool installed = callbacks.isPawnIoInstalled ? callbacks.isPawnIoInstalled() : false;
    if (installed) {
        AppendMenuW(hMenu, MF_STRING, kIdUninstallPawnIo, L"Uninstall PawnIO");
    } else {
        AppendMenuW(hMenu, MF_STRING, kIdInstallPawnIo, L"Install PawnIO");
    }

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, kIdClose, L"Close");

    const UINT cmd =
        TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hWnd, nullptr);
    DestroyMenu(hMenu);
    PostMessage(hWnd, WM_NULL, 0, 0);

    if (cmd == kIdOpenConfig) {
        if (callbacks.onOpenConfig)
            callbacks.onOpenConfig();
    } else if (cmd == kIdInstallPawnIo) {
        if (callbacks.onInstallPawnIo)
            callbacks.onInstallPawnIo();
    } else if (cmd == kIdUninstallPawnIo) {
        if (callbacks.onUninstallPawnIo)
            callbacks.onUninstallPawnIo();
    } else if (cmd == kIdClose) {
        if (callbacks.onQuit) {
            StartShutdownAnimation();
            callbacks.onQuit();
        }
    }
}

LRESULT CALLBACK TrayIcon::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    TrayIcon* pThis = (TrayIcon*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTA*>(lParam);
        pThis = static_cast<TrayIcon*>(create->lpCreateParams);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
    }

    if (pThis && pThis->taskbarCreatedMessage != 0 && message == pThis->taskbarCreatedMessage) {
        pThis->RestoreAfterTaskbarCreated();
        return 0;
    } else if (message == WM_TRAYICON) {
        if (pThis && pThis->shuttingDown) {
            // Ignore all clicks during shutdown
            return 0;
        }
        if (lParam == WM_LBUTTONUP) {
            if (pThis && pThis->callbacks.onOpenConfig)
                pThis->callbacks.onOpenConfig();
        } else if (lParam == WM_RBUTTONUP) {
            if (pThis)
                pThis->ShowContextMenu();
        }
    } else if (message == WM_TIMER && wParam == BLINK_TIMER_ID) {
        if (pThis) {
            pThis->UpdateBlinkState();
        }
    } else if (message == WM_CLOSE) {
        if (pThis && pThis->callbacks.onQuit && !pThis->shuttingDown) {
            pThis->StartShutdownAnimation();
            pThis->callbacks.onQuit();
        }
        return 0;
    } else if (message == WM_QUERYENDSESSION) {
        // Windows is asking if we can shut down — always say yes
        return TRUE;
    } else if (message == WM_ENDSESSION) {
        if (wParam) {
            // Session is ending (shutdown/logoff) — trigger graceful exit
            if (pThis && pThis->callbacks.onQuit && !pThis->shuttingDown) {
                pThis->StartShutdownAnimation();
                pThis->callbacks.onQuit();
            }
        }
        return 0;
    }

    return DefWindowProc(hWnd, message, wParam, lParam);
}

void TrayIcon::StartShutdownAnimation() {
    if (shuttingDown)
        return;

    shuttingDown = true;
    blinkState = false;

    // Set initial shutdown icon and tooltip
    if (hIconShutdown) {
        nid.hIcon = hIconShutdown;
        strcpy_s(nid.szTip, "Capture Engine (Shutting down...)");
        Shell_NotifyIconA(NIM_MODIFY, &nid);
    }

    // Start timer for blinking (every 500ms)
    if (hWnd) {
        blinkTimerId = SetTimer(hWnd, BLINK_TIMER_ID, BLINK_INTERVAL_MS, NULL);
    }
}

void TrayIcon::UpdateBlinkState() {
    if (!shuttingDown || !iconInitialized || iconRemovalRequested)
        return;

    blinkState = !blinkState;

    if (blinkState) {
        // Icon visible (red)
        nid.hIcon = hIconShutdown ? hIconShutdown : hIconIdle;
    } else {
        // Icon "hidden" (transparent/blank)
        nid.hIcon = NULL;
    }

    strcpy_s(nid.szTip, "Capture Engine (Shutting down...)");
    Shell_NotifyIconA(NIM_MODIFY, &nid);
}

void TrayIcon::Remove() {
    iconRemovalRequested = true;

    if (blinkTimerId && hWnd) {
        KillTimer(hWnd, blinkTimerId);
        blinkTimerId = 0;
    }

    if (iconInitialized) {
        Shell_NotifyIconA(NIM_DELETE, &nid);
        iconInitialized = false;
    }
}

void TrayIcon::Update() {
    MSG msg;
    while (PeekMessageA(&msg, hWnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

void TrayIcon::Run() {
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}
