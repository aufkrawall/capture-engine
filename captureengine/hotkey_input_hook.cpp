#include "hotkey_input_hook.h"

#include <atomic>

#include <thread>

#include "../common/hotkey_matcher.h"

#include "../common/logging.h"

#include "main_internal.h"

namespace {

struct HotkeyHookState {
    SRWLOCK bindingsLock = SRWLOCK_INIT;
    HotkeyBinding bindings[kMaxHotkeyBindings] = {};
    size_t bindingCount = 0;

    // Touched only by the hook thread.
    HotkeyMatcher matcher;

    std::atomic<DWORD> targetThreadId{0};
    std::atomic<DWORD> hookThreadId{0};
    std::atomic<bool> installed{false};
    std::atomic<uint64_t> delivered{0};
    std::thread thread;
};

HotkeyHookState g_HotkeyHook;

// Physical modifier state. The async table is maintained by the raw input
// thread, so it stays correct even for applications that take their keyboard
// through raw input with RIDEV_NOLEGACY and never see a WM_KEYDOWN.
HotkeyModifierState ReadModifierState() {
    HotkeyModifierState modifiers;
    modifiers.ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    modifiers.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    modifiers.alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    modifiers.win = ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) || ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0);
    return modifiers;
}

bool HandleKeyEvent(WPARAM message, const KBDLLHOOKSTRUCT& event) {
    HotkeyKeyAction action;
    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
        action = HotkeyKeyAction::KeyDown;
    } else if (message == WM_KEYUP || message == WM_SYSKEYUP) {
        action = HotkeyKeyAction::KeyUp;
    } else {
        return false;
    }

    const int vkey = static_cast<int>(event.vkCode);

    HotkeyBinding bindings[kMaxHotkeyBindings] = {};
    size_t count = 0;
    // Never wait here. A low-level keyboard hook that blocks stalls the input of
    // every process on the desktop, and the system drops hooks that answer too
    // slowly. A press that lands exactly while the table is being republished is
    // simply not matched here and stays with the RegisterHotKey path; the key
    // bookkeeping below still runs, so no later press is misread as a repeat.
    if (TryAcquireSRWLockShared(&g_HotkeyHook.bindingsLock)) {
        count = g_HotkeyHook.bindingCount;
        for (size_t i = 0; i < count && i < kMaxHotkeyBindings; ++i)
            bindings[i] = g_HotkeyHook.bindings[i];
        ReleaseSRWLockShared(&g_HotkeyHook.bindingsLock);
    }

    const HotkeyMatchResult match = g_HotkeyHook.matcher.Observe(bindings, count, vkey, action, ReadModifierState());
    if (match.id == 0)
        return match.swallow;

    const DWORD targetThreadId = g_HotkeyHook.targetThreadId.load(std::memory_order_acquire);
    if (targetThreadId == 0 ||
        !PostThreadMessage(targetThreadId, main_kMsgHotkeyFromInputHook, static_cast<WPARAM>(match.id), 0)) {
        // The press could not be acted on, so it must not be eaten either.
        g_HotkeyHook.matcher.ClearSwallow(vkey);
        LogError("[Hotkey] Failed to deliver hotkey id=%d from the keyboard hook (error %lu); key passed through",
                 match.id, GetLastError());
        return false;
    }

    const uint64_t delivered = g_HotkeyHook.delivered.fetch_add(1, std::memory_order_relaxed) + 1;
    LogDebug("[Hotkey] Keyboard-hook delivery id=%d vk=0x%02X total=%llu", match.id, vkey,
             (unsigned long long)delivered);
    return true;
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && lParam != 0) {
        // Consuming the key is what makes the two delivery paths mutually
        // exclusive: a key the hook eats never reaches hotkey processing, so it
        // can never also arrive as WM_HOTKEY.
        if (HandleKeyEvent(wParam, *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam)))
            return 1;
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void HotkeyHookThreadMain(HANDLE readyEvent) {
    // Give the thread a message queue before anyone can post to it.
    MSG msg;
    PeekMessage(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    g_HotkeyHook.hookThreadId.store(GetCurrentThreadId(), std::memory_order_release);

    // This thread only pumps messages, so the hook can answer immediately even
    // while the controller thread is busy starting a recording or encoding a
    // screenshot.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    g_HotkeyHook.matcher.Reset();
    const HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);
    if (hook) {
        g_HotkeyHook.installed.store(true, std::memory_order_release);
        LogInfo("[Hotkey] Keyboard hook installed; hotkeys also reach applications that suppress them");
    } else {
        LogError(
            "[Hotkey] Keyboard hook could not be installed (error %lu); hotkeys depend on RegisterHotKey alone and "
            "stay unavailable in applications that register RIDEV_NOHOTKEYS",
            GetLastError());
    }
    SetEvent(readyEvent);
    if (!hook)
        return;

    while (true) {
        const BOOL result = GetMessage(&msg, nullptr, 0, 0);
        if (result == 0 || result == -1)
            break;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(hook);
    g_HotkeyHook.installed.store(false, std::memory_order_release);
    LogInfo("[Hotkey] Keyboard hook removed (deliveries=%llu)",
            (unsigned long long)g_HotkeyHook.delivered.load(std::memory_order_relaxed));
}

}  // namespace

bool RegisterConfiguredHotkey(int hotkeyId, const AppConfig::HotkeyConfig& hotkey, const char* name) {
    if (hotkey.vkey == 0)
        return false;
    if (RegisterHotKey(NULL, hotkeyId, hotkey.GetModifiers(), hotkey.vkey))
        return true;
    LogError(
        "[Hotkey] Failed to register the %s hotkey vk=0x%02X (error %lu); another application owns this combination, "
        "so CaptureEngine leaves it to that application",
        name, hotkey.vkey, GetLastError());
    return false;
}

void PublishHotkeyBindings(const AppConfig& config, const HotkeyOwnership& ownership) {
    HotkeyBinding bindings[kMaxHotkeyBindings] = {};
    const size_t count = BuildHotkeyBindings(config, ownership, bindings, kMaxHotkeyBindings);

    AcquireSRWLockExclusive(&g_HotkeyHook.bindingsLock);
    for (size_t i = 0; i < kMaxHotkeyBindings; ++i)
        g_HotkeyHook.bindings[i] = bindings[i];
    g_HotkeyHook.bindingCount = count;
    ReleaseSRWLockExclusive(&g_HotkeyHook.bindingsLock);

    for (size_t i = 0; i < count; ++i) {
        LogDebug("[Hotkey] Binding id=%d vk=0x%02X ctrl=%d shift=%d alt=%d win=%d", bindings[i].id, bindings[i].vkey,
                 bindings[i].ctrl ? 1 : 0, bindings[i].shift ? 1 : 0, bindings[i].alt ? 1 : 0,
                 bindings[i].win ? 1 : 0);
    }
}

bool StartHotkeyInputHook(DWORD targetThreadId) {
    if (g_HotkeyHook.thread.joinable())
        return g_HotkeyHook.installed.load(std::memory_order_acquire);
    if (targetThreadId == 0) {
        LogError("[Hotkey] Refusing to start the keyboard hook without a delivery thread");
        return false;
    }

    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!readyEvent) {
        LogError("[Hotkey] Failed to create the keyboard hook startup event (error %lu)", GetLastError());
        return false;
    }

    g_HotkeyHook.targetThreadId.store(targetThreadId, std::memory_order_release);
    g_HotkeyHook.thread = std::thread(HotkeyHookThreadMain, readyEvent);
    WaitForSingleObject(readyEvent, INFINITE);
    CloseHandle(readyEvent);

    const bool installed = g_HotkeyHook.installed.load(std::memory_order_acquire);
    if (!installed)
        StopHotkeyInputHook();
    return installed;
}

void StopHotkeyInputHook() {
    if (!g_HotkeyHook.thread.joinable())
        return;
    const DWORD hookThreadId = g_HotkeyHook.hookThreadId.load(std::memory_order_acquire);
    if (hookThreadId != 0)
        PostThreadMessage(hookThreadId, WM_QUIT, 0, 0);
    g_HotkeyHook.thread.join();
    g_HotkeyHook.hookThreadId.store(0, std::memory_order_release);
    g_HotkeyHook.targetThreadId.store(0, std::memory_order_release);
}

bool IsHotkeyInputHookActive() {
    return g_HotkeyHook.installed.load(std::memory_order_acquire);
}
