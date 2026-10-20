#pragma once

#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include "captureengine/resource.h"
#include "frontend/app/setup_model.h"

#define WM_TRAYICON (WM_USER + 1)
#define WM_SHUTDOWN_TIMER (WM_USER + 2)

class TrayIcon {
public:
    struct Callbacks {
        std::function<void()> onQuit;
        std::function<void()> onOpenConfig;
        // The setup items: the state the menu draws, and the item the user picked (SetupRequests decides what it means).
        std::function<ce::frontend::TraySetupState()> setupState;
        std::function<void(ce::frontend::TrayCommand)> onSetupCommand;
        // The tray's log lines: a CE_LOG_* level and UTF-8 text. The frontend routes them to the runtime's log.
        std::function<void(int32_t level, const char* message)> log;
    };

    TrayIcon(HINSTANCE hInstance, Callbacks callbacks);
    TrayIcon(HINSTANCE hInstance, std::function<void()> onQuit, std::function<void()> onOpenConfig);
    ~TrayIcon();

    void Update();  // Run message loop step or loop
    void Run();     // Blocking message loop

    void SetRecordingState(bool recording);

    // Start shutdown animation (blinking icon with "Shutting down..." tooltip)
    // Icon will blink until Remove() is called
    void StartShutdownAnimation();

    // Remove the icon completely (call after shutdown is complete)
    void Remove();

    bool IsShuttingDown() const {
        return shuttingDown;
    }

private:
    HINSTANCE hInstance;
    HWND hWnd = nullptr;
    NOTIFYICONDATAA nid{};
    UINT taskbarCreatedMessage = 0;
    Callbacks callbacks;

    HICON hIconIdle = nullptr;
    HICON hIconRecording = nullptr;
    HICON hIconShutdown = nullptr;

    bool shuttingDown = false;
    bool blinkState = false;
    bool iconInitialized = false;
    bool iconRemovalRequested = false;
    UINT_PTR blinkTimerId = 0;

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
    void InitWindow();
    void InitIcon();
    void RestoreAfterTaskbarCreated();
    void Log(int32_t level, const char* format, ...) const;
    void UpdateBlinkState();
    void ShowContextMenu();
};
