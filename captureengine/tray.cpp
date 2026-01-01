#include "tray.h"
#include <shellapi.h>

TrayIcon::TrayIcon(HINSTANCE hInstance, std::function<void()> onQuit,
                   std::function<void()> onOpenConfig)
    : hInstance(hInstance), onQuit(onQuit), onOpenConfig(onOpenConfig) {
  InitWindow();
  InitIcon();
}

TrayIcon::~TrayIcon() {
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
  hWnd = CreateWindowExA(0, "CaptureEngineTray", "CaptureEngine", 0, 0, 0, 0, 0,
                         HWND_MESSAGE, NULL, hInstance, NULL);

  // Store 'this' pointer
  SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)this);
}

void TrayIcon::InitIcon() {
  hIconIdle = (HICON)LoadImageA(hInstance, MAKEINTRESOURCEA(IDI_ICON_IDLE), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
  hIconRecording = (HICON)LoadImageA(hInstance, MAKEINTRESOURCEA(IDI_ICON_RECORDING), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);

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
  if (!nid.hIcon) nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
  
  strcpy_s(nid.szTip, recording ? "Capture Engine (Recording...)" : "Capture Engine");
  Shell_NotifyIconA(NIM_MODIFY, &nid);
}

LRESULT CALLBACK TrayIcon::WndProc(HWND hWnd, UINT message, WPARAM wParam,
                                   LPARAM lParam) {
  TrayIcon *pThis = (TrayIcon *)GetWindowLongPtr(hWnd, GWLP_USERDATA);

  if (message == WM_TRAYICON) {
    if (lParam == WM_LBUTTONUP) {
      if (pThis && pThis->onOpenConfig)
        pThis->onOpenConfig();
    } else if (lParam == WM_RBUTTONUP) {
      // Right-click closes the program immediately
      if (pThis && pThis->onQuit)
        pThis->onQuit();
    }
  }

  return DefWindowProc(hWnd, message, wParam, lParam);
}
