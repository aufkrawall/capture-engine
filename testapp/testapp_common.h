#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <avrt.h>

#include <string>

namespace testapp {

inline std::wstring NarrowToWide(const char* value) {
    if (!value || !*value) {
        return std::wstring();
    }

    const int requiredChars = MultiByteToWideChar(CP_ACP, 0, value, -1, nullptr, 0);
    if (requiredChars <= 1) {
        return std::wstring();
    }

    std::wstring wideValue(static_cast<size_t>(requiredChars - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, value, -1, wideValue.data(), requiredChars);
    return wideValue;
}

inline std::wstring QuoteCommandLineArg(const std::wstring& value) {
    if (value.empty()) {
        return L"\"\"";
    }

    bool needsQuotes = false;
    for (wchar_t ch : value) {
        if (ch == L' ' || ch == L'\t' || ch == L'\"') {
            needsQuotes = true;
            break;
        }
    }
    if (!needsQuotes) {
        return value;
    }

    std::wstring quoted;
    quoted.push_back(L'\"');
    size_t backslashCount = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashCount;
            continue;
        }
        if (ch == L'\"') {
            quoted.append(backslashCount * 2 + 1, L'\\');
            quoted.push_back(L'\"');
            backslashCount = 0;
            continue;
        }
        if (backslashCount != 0) {
            quoted.append(backslashCount, L'\\');
            backslashCount = 0;
        }
        quoted.push_back(ch);
    }
    if (backslashCount != 0) {
        quoted.append(backslashCount * 2, L'\\');
    }
    quoted.push_back(L'\"');
    return quoted;
}

inline bool LaunchX86SiblingProcess(int argc, char* argv[]) {
#if defined(_WIN64)
    wchar_t modulePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0) {
        return false;
    }

    std::wstring exePath = modulePath;
    const size_t slashPos = exePath.find_last_of(L"\\/");
    if (slashPos == std::wstring::npos) {
        return false;
    }

    const std::wstring directory = exePath.substr(0, slashPos);
    const std::wstring exeName = exePath.substr(slashPos + 1);
    const std::wstring siblingPath = directory + L"\\x86\\" + exeName;
    if (GetFileAttributesW(siblingPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    std::wstring commandLine = QuoteCommandLineArg(siblingPath);
    for (int i = 1; i < argc; ++i) {
        commandLine.push_back(L' ');
        commandLine += QuoteCommandLineArg(NarrowToWide(argv[i]));
    }

    STARTUPINFOW startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo = {};
    std::wstring mutableCommandLine = commandLine;
    if (!CreateProcessW(siblingPath.c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &startupInfo, &processInfo)) {
        return false;
    }

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return true;
#else
    (void)argc;
    (void)argv;
    return false;
#endif
}

inline void EnableGameDpiAwareness() {
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) {
        user32 = LoadLibraryA("user32.dll");
    }

    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT value);
    auto setProcessDpiAwarenessContext =
        reinterpret_cast<SetProcessDpiAwarenessContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (setProcessDpiAwarenessContext &&
        setProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        return;
    }

    HMODULE shcore = LoadLibraryA("shcore.dll");
    if (shcore) {
        enum ProcessDpiAwarenessValue {
            kProcessDpiUnaware = 0,
            kProcessSystemDpiAware = 1,
            kProcessPerMonitorDpiAware = 2,
        };

        using SetProcessDpiAwarenessFn = HRESULT(WINAPI*)(ProcessDpiAwarenessValue value);
        auto setProcessDpiAwareness =
            reinterpret_cast<SetProcessDpiAwarenessFn>(GetProcAddress(shcore, "SetProcessDpiAwareness"));
        if (setProcessDpiAwareness && SUCCEEDED(setProcessDpiAwareness(kProcessPerMonitorDpiAware))) {
            return;
        }
    }

    SetProcessDPIAware();
}

inline void ApplyGameScheduling() {
    PROCESS_POWER_THROTTLING_STATE pts = {};
    pts.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pts.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    pts.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pts, sizeof(pts));

    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    DWORD mmcssTaskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristics(TEXT("Games"), &mmcssTaskIndex);
    if (mmcssHandle) {
        AvSetMmThreadPriority(mmcssHandle, AVRT_PRIORITY_HIGH);
    }

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
}

inline RECT GetPrimaryMonitorRect() {
    POINT origin = {0, 0};
    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo)) {
        return monitorInfo.rcMonitor;
    }

    RECT rect = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    return rect;
}

inline UINT GetMonitorDpiForRect(const RECT& monitorRect) {
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) {
        user32 = LoadLibraryA("user32.dll");
    }

    using GetDpiForSystemFn = UINT(WINAPI*)();
    auto getDpiForSystem = reinterpret_cast<GetDpiForSystemFn>(GetProcAddress(user32, "GetDpiForSystem"));
    if (getDpiForSystem) {
        UINT dpi = getDpiForSystem();
        if (dpi > 0) {
            return dpi;
        }
    }

    POINT point = {monitorRect.left, monitorRect.top};
    HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    if (monitor) {
        HMODULE shcore = LoadLibraryA("shcore.dll");
        if (shcore) {
            using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR monitor, int dpiType, UINT* dpiX, UINT* dpiY);
            auto getDpiForMonitor =
                reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"));
            if (getDpiForMonitor) {
                UINT dpiX = 0;
                UINT dpiY = 0;
                if (SUCCEEDED(getDpiForMonitor(monitor, 0, &dpiX, &dpiY)) && dpiX > 0) {
                    return dpiX;
                }
            }
        }
    }

    return 96;
}

inline RECT AdjustWindowRectForClientSize(DWORD style, DWORD exStyle, int clientWidth, int clientHeight) {
    RECT rect = {0, 0, clientWidth, clientHeight};
    const RECT primaryMonitorRect = GetPrimaryMonitorRect();
    const UINT dpi = GetMonitorDpiForRect(primaryMonitorRect);

    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) {
        user32 = LoadLibraryA("user32.dll");
    }

    using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    auto adjustWindowRectExForDpi =
        reinterpret_cast<AdjustWindowRectExForDpiFn>(GetProcAddress(user32, "AdjustWindowRectExForDpi"));
    if (adjustWindowRectExForDpi && adjustWindowRectExForDpi(&rect, style, FALSE, exStyle, dpi)) {
        return rect;
    }

    AdjustWindowRectEx(&rect, style, FALSE, exStyle);
    return rect;
}

inline bool PrimeWindowForBenchmark(HWND hwnd, bool fullscreen, int clientWidth, int clientHeight,
                                    DWORD warmupMs = 750) {
    if (!hwnd) {
        return false;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    if (fullscreen) {
        const RECT monitorRect = GetPrimaryMonitorRect();
        SetWindowPos(hwnd, HWND_TOPMOST, monitorRect.left, monitorRect.top, clientWidth, clientHeight,
                     SWP_SHOWWINDOW);
    } else {
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }

    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);

    const uint64_t deadline = GetTickCount64() + warmupMs;
    MSG msg = {};
    while (GetTickCount64() < deadline) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                return false;
            }
        }
        Sleep(10);
    }

    return IsWindow(hwnd) != FALSE;
}

}  // namespace testapp