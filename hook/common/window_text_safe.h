#pragma once

#include <windows.h>

// Reading a window title from inside the window's own process.
//
// GetWindowText() is only cheap for windows owned by ANOTHER process, where
// USER32 returns cached text. For a window owned by the CURRENT process it is
// documented to send WM_GETTEXT to the owning thread and block until that
// thread dispatches it - with no timeout. CE reads titles from two places that
// both run inside the game:
//
//   * the DirectDraw overlay route, on the game's render thread, whenever the
//     presentation window is not cached yet and the game is not foreground -
//     which is exactly the startup window, when the game's UI thread is busy
//     loading and may not pump for seconds, and
//   * the freeze watchdog, twice a second for the whole session, which can
//     wedge the one thread whose job is to notice that something is wedged.
//
// Neither caller needs the title badly enough to block on it: they use it for
// diagnostics and for telling one dialog from another. Bound the send and
// treat a timeout as an empty title.
namespace ce::window_text {

inline constexpr UINT kDefaultWindowTextTimeoutMs = 50;

// Fills `buffer` with the window's title, or leaves it empty when the owning
// thread does not answer within `timeoutMs`. Always NUL-terminates.
inline void ReadWindowTitleBounded(HWND hwnd, char* buffer, int bufferChars,
                                   UINT timeoutMs = kDefaultWindowTextTimeoutMs) {
    if (!buffer || bufferChars <= 0) {
        return;
    }
    buffer[0] = '\0';
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }

    // Same thread: GetWindowText dispatches inline, so there is nothing to wait
    // for and nothing to bound.
    if (GetWindowThreadProcessId(hwnd, nullptr) == GetCurrentThreadId()) {
        if (GetWindowTextA(hwnd, buffer, bufferChars) <= 0) {
            buffer[0] = '\0';
        }
        buffer[bufferChars - 1] = '\0';
        return;
    }

    DWORD_PTR sendResult = 0;
    if (!SendMessageTimeoutA(hwnd, WM_GETTEXT, static_cast<WPARAM>(bufferChars),
                             reinterpret_cast<LPARAM>(buffer), SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, timeoutMs,
                             &sendResult)) {
        buffer[0] = '\0';
        return;
    }
    buffer[bufferChars - 1] = '\0';
}

}  // namespace ce::window_text
