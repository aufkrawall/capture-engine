#pragma once

#include <Windows.h>
#include <functional>
#include <string>
#include "resource.h"

#define WM_TRAYICON (WM_USER + 1)

class TrayIcon {
public:
  TrayIcon(HINSTANCE hInstance, std::function<void()> onQuit,
           std::function<void()> onOpenConfig);
  ~TrayIcon();

  void Update(); // Run message loop step or loop
  void Run();    // Blocking message loop
  
  void SetRecordingState(bool recording);

private:
  HINSTANCE hInstance;
  HWND hWnd;
  NOTIFYICONDATAA nid;
  std::function<void()> onQuit;
  std::function<void()> onOpenConfig;
  
  HICON hIconIdle = nullptr;
  HICON hIconRecording = nullptr;

  static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam,
                                  LPARAM lParam);
  void InitWindow();
  void InitIcon();
};
