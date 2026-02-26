#include "tray.h"
#include <shellapi.h>

static constexpr UINT_PTR BLINK_TIMER_ID = 1001;
static constexpr UINT BLINK_INTERVAL_MS = 500;

TrayIcon::TrayIcon(HINSTANCE hInstance, std::function<void()> onQuit, std::function<void()> onOpenConfig)
    : hInstance(hInstance),
      onQuit(onQuit),
      onOpenConfig(onOpenConfig) {
    InitWindow();
    InitIcon();
}

TrayIcon::~TrayIcon() {
    if (blinkTimerId) {
        KillTimer(hWnd, blinkTimerId);
        blinkTimerId = 0;
    }
    Shell_NotifyIconA(NIM_DELETE, &nid);
    DestroyWindow(hWnd);
}

void TrayIcon::InitWindow() {
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "CaptureEngineTray";

    RegisterClassExA(&wc);

    // Create hidden window
    hWnd = CreateWindowExA(0, "CaptureEngineTray", "CaptureEngine", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);

    // Store 'this' pointer
    SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)this);
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

    Shell_NotifyIconA(NIM_ADD, &nid);
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

    if (message == WM_TRAYICON) {
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

    // LogInfo("Tray blinking: state=%d", blinkState);
    Shell_NotifyIconA(NIM_MODIFY, &nid);
}

void TrayIcon::Remove() {
    if (blinkTimerId) {
        KillTimer(hWnd, blinkTimerId);
        blinkTimerId = 0;
    }
    Shell_NotifyIconA(NIM_DELETE, &nid);
}
