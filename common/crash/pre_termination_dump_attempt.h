#pragma once

#include <windows.h>

#include <limits>

#include "common/platform/raii_helpers.h"

namespace ce::crash_dump_policy {

// Serialize dump ownership with the final native self-termination. A second
// caller must not reach the kernel while the owner is still publishing a dump.
class PreTerminationDumpAttempt {
public:
    bool Initialize() {
        const HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
        if (!kernelBase)
            return false;
        wait_ = reinterpret_cast<decltype(wait_)>(GetProcAddress(kernelBase, "WaitOnAddress"));
        wake_ = reinterpret_cast<decltype(wake_)>(GetProcAddress(kernelBase, "WakeByAddressAll"));
        return wait_ && wake_;
    }

    bool Started() {
        const LONG64 state = InterlockedCompareExchange64(&state_, 0, 0);
        return state > 0 || state == kComplete;
    }

    bool TryStart() {
        return InterlockedCompareExchange64(&state_, GetCurrentThreadId(), 0) == 0;
    }

    void Complete() { Publish(kComplete); }

    template <typename Forward, typename OnTimeout>
    auto ForwardExit(DWORD timeoutMs, Forward forward, OnTimeout onTimeout) {
        const auto thread = static_cast<LONG64>(GetCurrentThreadId());
        const ULONGLONG start = GetTickCount64();
        for (;;) {
            LONG64 observed = InterlockedCompareExchange64(&state_, -thread, 0);
            if (observed == 0) {
                // Native termination can return, including its null-handle
                // phase. Never hold a lock across a call that can kill peers.
                const auto release = ce::make_scope_guard([&] { Publish(0); });
                return forward();
            }
            if (observed == kComplete || observed == thread || observed == -thread)
                return forward();
            const ULONGLONG elapsed = GetTickCount64() - start;
            if (!wait_ || !wake_ || elapsed >= timeoutMs ||
                !wait_(&state_, &observed, sizeof(observed), timeoutMs - static_cast<DWORD>(elapsed))) {
                onTimeout();
                return forward();
            }
        }
    }

private:
    void Publish(LONG64 state) {
        InterlockedExchange64(&state_, state);
        if (wake_)
            wake_(const_cast<LONG64*>(&state_));
    }

    static constexpr LONG64 kComplete = (std::numeric_limits<LONG64>::min)();
    // Positive thread IDs own the dump. Negative IDs reserve native forwarding.
    alignas(8) volatile LONG64 state_ = 0;
    BOOL (WINAPI* wait_)(volatile VOID*, PVOID, SIZE_T, DWORD) = nullptr;
    VOID (WINAPI* wake_)(PVOID) = nullptr;
};

}  // namespace ce::crash_dump_policy
