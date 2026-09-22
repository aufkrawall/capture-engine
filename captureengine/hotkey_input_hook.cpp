#include "hotkey_input_hook.h"

#include <atomic>

#include <thread>

#include "../common/crash_handler.h"

#include "../common/hotkey_matcher.h"

#include "../common/keyboard_hook_policy.h"

#include "../common/logging.h"

#include "main_internal.h"

namespace {

namespace policy = ce::keyboard_hook;

// Thread messages the hook thread handles itself.
constexpr UINT kMsgRearmHook = WM_APP + 1;
constexpr UINT kMsgReleaseHookForCrash = WM_APP + 2;

struct HotkeyHookState {
    SRWLOCK bindingsLock = SRWLOCK_INIT;
    HotkeyBinding bindings[kMaxHotkeyBindings] = {};
    size_t bindingCount = 0;

    // Touched only by the hook thread.
    HotkeyMatcher matcher;

    // The live hook. Whoever removes it exchanges it out first, so the hook
    // thread and the crash path can never both unhook the same handle.
    std::atomic<HHOOK> hook{nullptr};
    std::atomic<DWORD> timeoutMs{policy::kDefaultLowLevelHooksTimeoutMs};
    std::atomic<DWORD> installError{0};
    std::atomic<bool> timeCritical{false};
    std::atomic<bool> rearmPending{false};
    std::atomic<bool> crashReleased{false};
    HANDLE crashReleasedEvent = nullptr;

    std::atomic<DWORD> targetThreadId{0};
    std::atomic<DWORD> hookThreadId{0};
    std::atomic<bool> installed{false};
    std::thread thread;

    // Diagnostics. The hook thread only counts; the controller thread logs,
    // because a log write can block on the file system and the hook thread
    // must never block.
    std::atomic<uint64_t> delivered{0};
    std::atomic<uint64_t> deliveryFailures{0};
    std::atomic<DWORD> lastDeliveryError{0};
    std::atomic<uint64_t> lateCallbacks{0};
    std::atomic<uint64_t> pastTimeoutCallbacks{0};
    std::atomic<DWORD> worstCallbackAgeMs{0};
    std::atomic<uint64_t> rearms{0};
    std::atomic<uint64_t> rearmFailures{0};
    std::atomic<DWORD> lastRearmError{0};
    std::atomic<uint64_t> systemRemovals{0};
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

DWORD ReadLowLevelHooksTimeoutMs() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"LowLevelHooksTimeout", RRF_RT_REG_DWORD,
                     nullptr, &value, &size) == ERROR_SUCCESS) {
        return policy::ResolveLowLevelHooksTimeoutMs(true, value);
    }
    wchar_t text[16] = {};
    size = sizeof(text);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"LowLevelHooksTimeout", RRF_RT_REG_SZ, nullptr,
                     text, &size) == ERROR_SUCCESS) {
        wchar_t* end = nullptr;
        const unsigned long parsed = wcstoul(text, &end, 10);
        if (end != text)
            return policy::ResolveLowLevelHooksTimeoutMs(true, static_cast<DWORD>(parsed));
    }
    return policy::ResolveLowLevelHooksTimeoutMs(false, 0);
}

void RecordWorstCallbackAge(DWORD ageMs) {
    DWORD worst = g_HotkeyHook.worstCallbackAgeMs.load(std::memory_order_relaxed);
    while (ageMs > worst &&
           !g_HotkeyHook.worstCallbackAgeMs.compare_exchange_weak(worst, ageMs, std::memory_order_relaxed)) {
    }
}

bool HandleKeyEvent(WPARAM message, const KBDLLHOOKSTRUCT& event, bool pastTimeout) {
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
    // An event the system already passed on is only bookkept for the same
    // reason: the application has it, so it can neither be consumed nor acted on.
    if (!pastTimeout && TryAcquireSRWLockShared(&g_HotkeyHook.bindingsLock)) {
        count = g_HotkeyHook.bindingCount;
        for (size_t i = 0; i < count && i < kMaxHotkeyBindings; ++i)
            bindings[i] = g_HotkeyHook.bindings[i];
        ReleaseSRWLockShared(&g_HotkeyHook.bindingsLock);
    }

    const HotkeyMatchResult match = g_HotkeyHook.matcher.Observe(bindings, count, vkey, action, ReadModifierState());
    if (pastTimeout)
        return false;
    if (match.id == 0)
        return match.swallow;

    const DWORD targetThreadId = g_HotkeyHook.targetThreadId.load(std::memory_order_acquire);
    if (targetThreadId == 0 || !PostThreadMessage(targetThreadId, main_kMsgHotkeyFromInputHook,
                                                  static_cast<WPARAM>(match.id), static_cast<LPARAM>(vkey))) {
        // The press could not be acted on, so it must not be eaten either.
        g_HotkeyHook.matcher.ClearSwallow(vkey);
        g_HotkeyHook.lastDeliveryError.store(GetLastError(), std::memory_order_relaxed);
        g_HotkeyHook.deliveryFailures.fetch_add(1, std::memory_order_release);
        return false;
    }

    g_HotkeyHook.delivered.fetch_add(1, std::memory_order_relaxed);
    return true;
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && lParam != 0) {
        const auto& event = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        const DWORD timeoutMs = g_HotkeyHook.timeoutMs.load(std::memory_order_relaxed);
        const DWORD ageMs = policy::CallbackAgeMs(event.time, GetTickCount());
        const bool injected = (event.flags & LLKHF_INJECTED) != 0;
        const bool pastTimeout = !injected && policy::IsPastSystemTimeout(ageMs, timeoutMs);
        if (policy::ShouldRearmAfterCallback(ageMs, timeoutMs, injected)) {
            // This thread was held long enough for the system to count, or to
            // be about to count, a timeout against the hook. Re-arm once the
            // queued callbacks are drained; posted messages run after them.
            RecordWorstCallbackAge(ageMs);
            g_HotkeyHook.lateCallbacks.fetch_add(1, std::memory_order_relaxed);
            if (pastTimeout)
                g_HotkeyHook.pastTimeoutCallbacks.fetch_add(1, std::memory_order_relaxed);
            if (!g_HotkeyHook.rearmPending.exchange(true, std::memory_order_acq_rel))
                PostThreadMessage(GetCurrentThreadId(), kMsgRearmHook, 0, 0);
        }
        // Consuming the key is what makes the two delivery paths mutually
        // exclusive: a key the hook eats never reaches hotkey processing, so it
        // can never also arrive as WM_HOTKEY.
        if (HandleKeyEvent(wParam, event, pastTimeout))
            return 1;
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// Swaps in a fresh hook. The system counts timeouts per hook and removes one
// after repeated timeouts without telling its owner, so after any late answer
// the hook is replaced: the new one starts with a clean history, and failing to
// remove the old one proves the system had already taken it away. The new hook
// is installed before the old one is removed, so no keystroke goes unobserved.
void RearmHook() {
    g_HotkeyHook.rearmPending.store(false, std::memory_order_release);
    if (g_HotkeyHook.crashReleased.load(std::memory_order_acquire))
        return;
    const HHOOK fresh = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);
    if (!fresh) {
        g_HotkeyHook.lastRearmError.store(GetLastError(), std::memory_order_relaxed);
        g_HotkeyHook.rearmFailures.fetch_add(1, std::memory_order_release);
        return;
    }
    const HHOOK previous = g_HotkeyHook.hook.exchange(fresh, std::memory_order_acq_rel);
    if (previous && !UnhookWindowsHookEx(previous))
        g_HotkeyHook.systemRemovals.fetch_add(1, std::memory_order_relaxed);
    g_HotkeyHook.rearms.fetch_add(1, std::memory_order_release);
    // The crash path may have run between the check above and the exchange.
    if (g_HotkeyHook.crashReleased.load(std::memory_order_acquire)) {
        if (const HHOOK stray = g_HotkeyHook.hook.exchange(nullptr, std::memory_order_acq_rel))
            UnhookWindowsHookEx(stray);
    }
}

void HotkeyHookThreadMain(HANDLE readyEvent) {
    // Give the thread a message queue before anyone can post to it.
    MSG msg;
    PeekMessage(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    g_HotkeyHook.hookThreadId.store(GetCurrentThreadId(), std::memory_order_release);

    // Every keystroke on the desktop waits for this thread, and it does only a
    // few microseconds of work per event, so it runs at the highest priority
    // of its class: a game's busy render threads must not be able to queue it.
    g_HotkeyHook.timeCritical.store(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != FALSE,
                                    std::memory_order_relaxed);
    g_HotkeyHook.timeoutMs.store(ReadLowLevelHooksTimeoutMs(), std::memory_order_relaxed);

    g_HotkeyHook.matcher.Reset();
    const HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);
    g_HotkeyHook.installError.store(hook ? 0 : GetLastError(), std::memory_order_relaxed);
    g_HotkeyHook.hook.store(hook, std::memory_order_release);
    g_HotkeyHook.installed.store(hook != nullptr, std::memory_order_release);
    SetEvent(readyEvent);
    if (!hook)
        return;

    // No logging on this thread while the hook is installed: see HotkeyHookState.
    while (true) {
        const BOOL result = GetMessage(&msg, nullptr, 0, 0);
        if (result == 0 || result == -1)
            break;
        if (msg.hwnd == nullptr && msg.message == kMsgRearmHook) {
            RearmHook();
            continue;
        }
        if (msg.hwnd == nullptr && msg.message == kMsgReleaseHookForCrash) {
            if (const HHOOK current = g_HotkeyHook.hook.exchange(nullptr, std::memory_order_acq_rel))
                UnhookWindowsHookEx(current);
            if (g_HotkeyHook.crashReleasedEvent)
                SetEvent(g_HotkeyHook.crashReleasedEvent);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (const HHOOK current = g_HotkeyHook.hook.exchange(nullptr, std::memory_order_acq_rel))
        UnhookWindowsHookEx(current);
    g_HotkeyHook.installed.store(false, std::memory_order_release);
    LogInfo("[Hotkey] Keyboard hook removed (deliveries=%llu rearms=%llu systemRemovals=%llu)",
            (unsigned long long)g_HotkeyHook.delivered.load(std::memory_order_relaxed),
            (unsigned long long)g_HotkeyHook.rearms.load(std::memory_order_relaxed),
            (unsigned long long)g_HotkeyHook.systemRemovals.load(std::memory_order_relaxed));
}

// Crash-path release. Runs on the crashing thread before the dump suspends the
// process: removing the hook from here is immediate when the system allows it
// from this thread; otherwise the hook thread removes it, bounded by the system
// timeout because waiting longer than the system itself would is pointless.
void ReleaseHotkeyInputHookForCrash() {
    g_HotkeyHook.crashReleased.store(true, std::memory_order_release);
    const HHOOK hook = g_HotkeyHook.hook.exchange(nullptr, std::memory_order_acq_rel);
    if (!hook || UnhookWindowsHookEx(hook))
        return;
    // The handle is out of the slot; hand it back so the hook thread can remove it.
    g_HotkeyHook.hook.store(hook, std::memory_order_release);
    const DWORD hookThreadId = g_HotkeyHook.hookThreadId.load(std::memory_order_acquire);
    if (hookThreadId == 0 || hookThreadId == GetCurrentThreadId() || !g_HotkeyHook.crashReleasedEvent)
        return;
    if (PostThreadMessage(hookThreadId, kMsgReleaseHookForCrash, 0, 0))
        WaitForSingleObject(g_HotkeyHook.crashReleasedEvent, g_HotkeyHook.timeoutMs.load(std::memory_order_relaxed));
}

struct HotkeyHookReport {
    uint64_t deliveryFailures = 0;
    uint64_t lateCallbacks = 0;
    uint64_t pastTimeoutCallbacks = 0;
    uint64_t rearms = 0;
    uint64_t rearmFailures = 0;
    uint64_t systemRemovals = 0;
};

HotkeyHookReport g_LastHotkeyHookReport;  // controller thread only

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
    if (!g_HotkeyHook.crashReleasedEvent)
        g_HotkeyHook.crashReleasedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_HotkeyHook.crashReleased.store(false, std::memory_order_release);

    g_HotkeyHook.targetThreadId.store(targetThreadId, std::memory_order_release);
    g_HotkeyHook.thread = std::thread(HotkeyHookThreadMain, readyEvent);
    WaitForSingleObject(readyEvent, INFINITE);
    CloseHandle(readyEvent);

    const bool installed = g_HotkeyHook.installed.load(std::memory_order_acquire);
    if (!installed) {
        LogError(
            "[Hotkey] Keyboard hook could not be installed (error %lu); hotkeys depend on RegisterHotKey alone and "
            "stay unavailable in applications that register RIDEV_NOHOTKEYS",
            g_HotkeyHook.installError.load(std::memory_order_relaxed));
        StopHotkeyInputHook();
        return false;
    }
    RegisterCrashPreDumpCallback(ReleaseHotkeyInputHookForCrash);
    LogInfo(
        "[Hotkey] Keyboard hook installed; hotkeys also reach applications that suppress them "
        "(systemTimeout=%lu ms, timeCritical=%d)",
        g_HotkeyHook.timeoutMs.load(std::memory_order_relaxed),
        g_HotkeyHook.timeCritical.load(std::memory_order_relaxed) ? 1 : 0);
    return true;
}

void StopHotkeyInputHook() {
    if (!g_HotkeyHook.thread.joinable())
        return;
    RegisterCrashPreDumpCallback(nullptr);
    const DWORD hookThreadId = g_HotkeyHook.hookThreadId.load(std::memory_order_acquire);
    if (hookThreadId != 0)
        PostThreadMessage(hookThreadId, WM_QUIT, 0, 0);
    g_HotkeyHook.thread.join();
    g_HotkeyHook.hookThreadId.store(0, std::memory_order_release);
    g_HotkeyHook.targetThreadId.store(0, std::memory_order_release);
    ReportHotkeyInputHookDiagnostics();
}

bool IsHotkeyInputHookActive() {
    return g_HotkeyHook.installed.load(std::memory_order_acquire);
}

uint64_t GetHotkeyInputHookDeliveredCount() {
    return g_HotkeyHook.delivered.load(std::memory_order_relaxed);
}

void ReportHotkeyInputHookDiagnostics() {
    HotkeyHookReport now;
    now.deliveryFailures = g_HotkeyHook.deliveryFailures.load(std::memory_order_acquire);
    now.lateCallbacks = g_HotkeyHook.lateCallbacks.load(std::memory_order_relaxed);
    now.pastTimeoutCallbacks = g_HotkeyHook.pastTimeoutCallbacks.load(std::memory_order_relaxed);
    now.rearms = g_HotkeyHook.rearms.load(std::memory_order_acquire);
    now.rearmFailures = g_HotkeyHook.rearmFailures.load(std::memory_order_acquire);
    now.systemRemovals = g_HotkeyHook.systemRemovals.load(std::memory_order_relaxed);
    HotkeyHookReport& last = g_LastHotkeyHookReport;

    if (now.deliveryFailures != last.deliveryFailures) {
        LogError("[Hotkey] %llu keyboard-hook hotkey press(es) could not be delivered (error %lu); those keys passed "
                 "through (total=%llu)",
                 (unsigned long long)(now.deliveryFailures - last.deliveryFailures),
                 g_HotkeyHook.lastDeliveryError.load(std::memory_order_relaxed),
                 (unsigned long long)now.deliveryFailures);
    }
    if (now.lateCallbacks != last.lateCallbacks) {
        // Every late answer held keyboard input for the whole desktop.
        LogWarn("[Hotkey] Keyboard hook answered late %llu time(s), %llu past the system timeout (worst %lu ms, "
                "systemTimeout=%lu ms); desktop input waited on CaptureEngine",
                (unsigned long long)(now.lateCallbacks - last.lateCallbacks),
                (unsigned long long)(now.pastTimeoutCallbacks - last.pastTimeoutCallbacks),
                g_HotkeyHook.worstCallbackAgeMs.exchange(0, std::memory_order_relaxed),
                g_HotkeyHook.timeoutMs.load(std::memory_order_relaxed));
    }
    if (now.rearms != last.rearms || now.systemRemovals != last.systemRemovals) {
        LogInfo("[Hotkey] Keyboard hook re-armed %llu time(s) (total=%llu)",
                (unsigned long long)(now.rearms - last.rearms), (unsigned long long)now.rearms);
        if (now.systemRemovals != last.systemRemovals) {
            LogError("[Hotkey] Windows had already removed the keyboard hook after repeated timeouts; the re-arm "
                     "restored it (systemRemovals=%llu)",
                     (unsigned long long)now.systemRemovals);
        }
    }
    if (now.rearmFailures != last.rearmFailures) {
        LogError("[Hotkey] Keyboard hook could not be re-armed (error %lu, failures=%llu); hotkeys may depend on "
                 "RegisterHotKey alone",
                 g_HotkeyHook.lastRearmError.load(std::memory_order_relaxed), (unsigned long long)now.rearmFailures);
    }
    last = now;
}
