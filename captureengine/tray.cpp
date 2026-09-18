#include "tray.h"
#include "../common/logging.h"
#include "../common/shared_defs.h"
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

    runtimeOverrideRefusedMessage = RegisterWindowMessageA(RuntimeOverrideRefusedMessageName());
    if (runtimeOverrideRefusedMessage == 0)
        LogWarn("[Tray] Failed to register the runtime-override notification message (error=%lu)", GetLastError());

    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "CaptureEngineTray";

    RegisterClassExA(&wc);

    // Use a hidden top-level tool window instead of a message-only window so
    // Explorer can observe a real UI window during launch and clear the startup
    // wait cursor promptly. Mark WS_EX_TOPMOST so popup menus owned by this window
    // inherit topmost status over the taskbar and fullscreen games.
    hWnd = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, "CaptureEngineTray", "CaptureEngine",
                           WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInstance, this);
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

    // Detect the taskbar rectangle and monitor bounds to prevent the context menu from
    // overlapping behind or under the taskbar.
    RECT rcExclude = {0};
    bool hasExcludeRect = false;

    HWND hPointWnd = WindowFromPoint(pt);
    if (hPointWnd) {
        HWND hRoot = GetAncestor(hPointWnd, GA_ROOT);
        if (hRoot) {
            wchar_t cls[64] = {0};
            if (GetClassNameW(hRoot, cls, static_cast<int>(sizeof(cls) / sizeof(cls[0]))) &&
                (wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0)) {
                if (GetWindowRect(hRoot, &rcExclude)) {
                    hasExcludeRect = true;
                }
            }
        }
    }

    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    const bool hasMonInfo = (hMon && GetMonitorInfoW(hMon, &mi));

    if (!hasExcludeRect && hasMonInfo) {
        if (mi.rcWork.bottom < mi.rcMonitor.bottom && pt.y >= mi.rcWork.bottom) {
            rcExclude = {mi.rcMonitor.left, mi.rcWork.bottom, mi.rcMonitor.right, mi.rcMonitor.bottom};
            hasExcludeRect = true;
        } else if (mi.rcWork.top > mi.rcMonitor.top && pt.y <= mi.rcWork.top) {
            rcExclude = {mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcWork.top};
            hasExcludeRect = true;
        } else if (mi.rcWork.left > mi.rcMonitor.left && pt.x <= mi.rcWork.left) {
            rcExclude = {mi.rcMonitor.left, mi.rcMonitor.top, mi.rcWork.left, mi.rcMonitor.bottom};
            hasExcludeRect = true;
        } else if (mi.rcWork.right < mi.rcMonitor.right && pt.x >= mi.rcWork.right) {
            rcExclude = {mi.rcWork.right, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom};
            hasExcludeRect = true;
        }
    }

    if (!hasExcludeRect) {
        rcExclude = {pt.x - 16, pt.y - 16, pt.x + 16, pt.y + 16};
        hasExcludeRect = true;
    }

    // Temporarily clear WS_EX_NOACTIVATE and ensure WS_EX_TOPMOST while showing the context menu
    // so Windows allows our window to take foreground focus and forces owned popups above the taskbar.
    const LONG_PTR originalExStyle = GetWindowLongPtr(hWnd, GWL_EXSTYLE);
    SetWindowLongPtr(hWnd, GWL_EXSTYLE, (originalExStyle & ~WS_EX_NOACTIVATE) | WS_EX_TOPMOST);

    // Bring hWnd to the foreground and ensure it is topmost. If a fullscreen borderless game is running,
    // attach thread input so SetForegroundWindow succeeds reliably.
    HWND hForeground = GetForegroundWindow();
    DWORD foregroundThreadId = hForeground ? GetWindowThreadProcessId(hForeground, nullptr) : 0;
    DWORD currentThreadId = GetCurrentThreadId();
    if (foregroundThreadId != 0 && foregroundThreadId != currentThreadId) {
        AttachThreadInput(currentThreadId, foregroundThreadId, TRUE);
        SetForegroundWindow(hWnd);
        SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        AttachThreadInput(currentThreadId, foregroundThreadId, FALSE);
    } else {
        SetForegroundWindow(hWnd);
        SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) {
        ShowWindow(hWnd, SW_HIDE);
        SetWindowLongPtr(hWnd, GWL_EXSTYLE, originalExStyle);
        return;
    }

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

    // Determine alignment: if the taskbar is at the bottom (or cursor in lower half),
    // open the menu upwards above the taskbar so items like "Close" are fully visible and clickable.
    UINT uFlags = TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY;
    const bool isBottomTaskbar =
        (hasExcludeRect && rcExclude.top > (hasMonInfo ? mi.rcMonitor.top : 0) &&
         rcExclude.bottom >= (hasMonInfo ? mi.rcMonitor.bottom : rcExclude.top));
    const bool isBottomHalf = hasMonInfo ? (pt.y >= (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2) : true;

    int menuY = pt.y;
    if (isBottomTaskbar || isBottomHalf) {
        uFlags |= TPM_BOTTOMALIGN | TPM_VERTICAL;
        if (hasExcludeRect && rcExclude.top > 0)
            menuY = rcExclude.top;
    } else {
        uFlags |= TPM_TOPALIGN | TPM_VERTICAL;
        if (hasExcludeRect && rcExclude.bottom > rcExclude.top)
            menuY = rcExclude.bottom;
    }

    const bool isRightHalf = hasMonInfo ? (pt.x > (mi.rcMonitor.left + mi.rcMonitor.right) / 2) : true;
    if (isRightHalf) {
        uFlags |= TPM_RIGHTALIGN;
    } else {
        uFlags |= TPM_LEFTALIGN;
    }

    TPMPARAMS tpm = {sizeof(TPMPARAMS), rcExclude};

    const UINT cmd = TrackPopupMenuEx(hMenu, uFlags, pt.x, menuY, hWnd, &tpm);

    DestroyMenu(hMenu);
    ShowWindow(hWnd, SW_HIDE);
    SetWindowLongPtr(hWnd, GWL_EXSTYLE, originalExStyle);
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
    } else if (pThis && pThis->runtimeOverrideRefusedMessage != 0 &&
               message == pThis->runtimeOverrideRefusedMessage) {
        // Posted by the inject process when the hook reported that a configured
        // DLSS/Streamline override did not take effect. The reason text is
        // resolved here rather than sent, so the two processes cannot disagree
        // about the wording and no string crosses a window message.
        const uint32_t reason = static_cast<uint32_t>(wParam);
        const char* detail = RuntimeOverrideRefusalText(reason);
        if (detail && detail[0] && !pThis->shuttingDown) {
            pThis->ShowNotification("CaptureEngine: DLSS/Streamline override not applied", detail);
        }
        return 0;
    } else if (message == WM_INITMENUPOPUP) {
        HWND hMenuWnd = FindWindowW(L"#32768", nullptr);
        if (hMenuWnd) {
            SetWindowPos(hMenuWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;
    } else if (message == WM_TRAYICON) {
        if (pThis && pThis->shuttingDown) {
            // Ignore all clicks during shutdown
            return 0;
        }
        if (lParam == WM_LBUTTONUP) {
            if (pThis && pThis->callbacks.onOpenConfig)
                pThis->callbacks.onOpenConfig();
        } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
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

void TrayIcon::ShowNotification(const std::string& title, const std::string& text) {
    if (!iconInitialized || iconRemovalRequested || shuttingDown) {
        return;
    }

    // NIM_MODIFY with NIF_INFO shows the balloon and leaves the icon itself
    // alone. The info fields are only read for this one call, so they are
    // cleared afterwards to keep a later NIM_MODIFY (recording state, blink)
    // from re-raising the same balloon.
    nid.uFlags |= NIF_INFO;
    strncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    strncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    nid.dwInfoFlags = NIIF_WARNING | NIIF_NOSOUND;
    const BOOL shown = Shell_NotifyIconA(NIM_MODIFY, &nid);

    nid.uFlags &= ~NIF_INFO;
    nid.szInfoTitle[0] = '\0';
    nid.szInfo[0] = '\0';
    nid.dwInfoFlags = 0;

    if (!shown) {
        LogWarn("[Tray] Notification balloon rejected (error=%lu): %s", GetLastError(), text.c_str());
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
