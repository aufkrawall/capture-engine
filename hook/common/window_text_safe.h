#pragma once

#include <windows.h>

#include <cstring>

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

// Appends `text` to the NUL-terminated `buffer`, separating entries with " | "
// and flattening line breaks, so a multi-line message stays one log line.
// Truncates at the buffer's end. Pure, so it is testable without a window.
inline void AppendDialogTextFragment(char* buffer, int bufferChars, const char* text) {
    if (!buffer || bufferChars <= 0 || !text || text[0] == '\0') {
        return;
    }
    int length = 0;
    while (length < bufferChars - 1 && buffer[length] != '\0') {
        ++length;
    }
    if (length != 0) {
        for (const char* separator = " | "; *separator != '\0' && length < bufferChars - 1; ++separator) {
            buffer[length++] = *separator;
        }
    }
    bool previousWasBreak = false;
    for (const char* cursor = text; *cursor != '\0' && length < bufferChars - 1; ++cursor) {
        const bool lineBreak = *cursor == '\r' || *cursor == '\n';
        if (lineBreak && previousWasBreak) {
            continue;
        }
        buffer[length++] = lineBreak ? ' ' : *cursor;
        previousWasBreak = lineBreak;
    }
    buffer[length] = '\0';
}

struct DialogBodyTextContext {
    char* buffer = nullptr;
    int bufferChars = 0;
    UINT timeoutMs = kDefaultWindowTextTimeoutMs;
};

inline BOOL CALLBACK CollectDialogBodyTextProc(HWND child, LPARAM lParam) {
    auto* context = reinterpret_cast<DialogBodyTextContext*>(lParam);
    char className[32] = {};
    if (!context || !GetClassNameA(child, className, static_cast<int>(sizeof(className)))) {
        return TRUE;
    }
    // A message box keeps its text in a Static, a custom error dialog often in
    // a read-only Edit. Buttons only say "OK".
    if (_stricmp(className, "Static") != 0 && _stricmp(className, "Edit") != 0) {
        return TRUE;
    }
    // A crash report box lists a whole call stack; the useful frames come last.
    char text[2048] = {};
    ReadWindowTitleBounded(child, text, static_cast<int>(sizeof(text)), context->timeoutMs);
    AppendDialogTextFragment(context->buffer, context->bufferChars, text);
    return TRUE;
}

// The message a dialog shows, read from its Static and Edit children with the
// same bounded send as the title. Empty when the dialog has no readable text.
inline void ReadDialogBodyTextBounded(HWND dialog, char* buffer, int bufferChars,
                                      UINT timeoutMs = kDefaultWindowTextTimeoutMs) {
    if (!buffer || bufferChars <= 0) {
        return;
    }
    buffer[0] = '\0';
    if (!dialog || !IsWindow(dialog)) {
        return;
    }
    DialogBodyTextContext context{buffer, bufferChars, timeoutMs};
    EnumChildWindows(dialog, CollectDialogBodyTextProc, reinterpret_cast<LPARAM>(&context));
}

}  // namespace ce::window_text
