#pragma once

#include <Windows.h>
#include <functional>
#include <string>
#include "resource.h"

#define WM_TRAYICON (WM_USER + 1)
#define WM_SHUTDOWN_TIMER (WM_USER + 2)

class TrayIcon {
public:
  TrayIcon(HINSTANCE hInstance, std::function<void()> onQuit,
           std::function<void()> onOpenConfig);
  ~TrayIcon();

  void Update(); // Run message loop step or loop
  void Run();    // Blocking message loop
  
  void SetRecordingState(bool recording);
  
  // Start shutdown animation (blinking icon with "Shutting down..." tooltip)
  // Icon will blink until Remove() is called
  void StartShutdownAnimation();
  
  // Remove the icon completely (call after shutdown is complete)
  void Remove();
  
  bool IsShuttingDown() const { return shuttingDown; }

private:
  HINSTANCE hInstance;
  HWND hWnd;
  NOTIFYICONDATAA nid;
  std::function<void()> onQuit;
  std::function<void()> onOpenConfig;
  
  HICON hIconIdle = nullptr;
  HICON hIconRecording = nullptr;
  
  bool shuttingDown = false;
  bool blinkState = false;
  UINT_PTR blinkTimerId = 0;

  static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam,
                                  LPARAM lParam);
  void InitWindow();
  void InitIcon();
  void UpdateBlinkState();
};
