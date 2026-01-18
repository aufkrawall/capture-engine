#pragma once

#include <windows.h>
#include <mutex>
#include <unordered_map>

class InputManager {
public:
    static InputManager& Get();

    // Hook the window Proc for this HWND
    void HookWindow(HWND hwnd);

    // Unhook the window Proc (restore original)
    void UnhookWindow(HWND hwnd);

    // Unhook all windows (shutdown)
    void Shutdown();

    // The hooked WndProc
    static LRESULT CALLBACK HookWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    InputManager() = default;
    ~InputManager();

    struct WindowHookInfo {
        WNDPROC originalProc = nullptr;
        bool hooked = false;
    };

    std::mutex m_Mutex;
    std::unordered_map<HWND, WindowHookInfo> m_Hooks;
};
