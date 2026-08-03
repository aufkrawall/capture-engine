#include "tray.h"
#include "../common/logging.h"
#include <shellapi.h>

static constexpr UINT_PTR BLINK_TIMER_ID = 1001;
static constexpr UINT BLINK_INTERVAL_MS = 500;

TrayIcon::TrayIcon(HINSTANCE hInstance, std::function<void()> onQuit, std::function<void()> onOpenConfig)
    : hInstance(hInstance),
      onQuit(std::move(onQuit)),
      onOpenConfig(std::move(onOpenConfig)) {
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
            if (pThis && pThis->onOpenConfig)
                pThis->onOpenConfig();
        } else if (lParam == WM_RBUTTONUP) {
            // Right-click starts shutdown (don't hide icon yet)
            if (pThis && pThis->onQuit) {
                pThis->StartShutdownAnimation();
                pThis->onQuit();
            }
        }
    } else if (message == WM_TIMER && wParam == BLINK_TIMER_ID) {
        if (pThis) {
            pThis->UpdateBlinkState();
        }
    } else if (message == WM_QUERYENDSESSION) {
        // Windows is asking if we can shut down — always say yes
        return TRUE;
    } else if (message == WM_ENDSESSION) {
        if (wParam) {
            // Session is ending (shutdown/logoff) — trigger graceful exit
            if (pThis && pThis->onQuit && !pThis->shuttingDown) {
                pThis->StartShutdownAnimation();
                pThis->onQuit();
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

    // Update tooltip to show shutting down
    strcpy_s(nid.szTip, "Capture Engine (Shutting down...)");
    Shell_NotifyIconA(NIM_MODIFY, &nid);

    // Start blink timer
    blinkTimerId = SetTimer(hWnd, BLINK_TIMER_ID, BLINK_INTERVAL_MS, NULL);
}

void TrayIcon::UpdateBlinkState() {
    blinkState = !blinkState;

    if (blinkState) {
        // Show icon (normal)
        nid.hIcon = hIconIdle ? hIconIdle : LoadIcon(NULL, IDI_APPLICATION);
    } else {
        // Show shutdown icon (orange placeholder)
        nid.hIcon = hIconShutdown ? hIconShutdown : LoadIcon(NULL, IDI_WINLOGO);
    }

    Shell_NotifyIconA(NIM_MODIFY, &nid);
}

void TrayIcon::Remove() {
    if (iconRemovalRequested)
        return;
    iconRemovalRequested = true;

    if (blinkTimerId) {
        KillTimer(hWnd, blinkTimerId);
        blinkTimerId = 0;
    }
    if (iconInitialized)
        Shell_NotifyIconA(NIM_DELETE, &nid);
}
