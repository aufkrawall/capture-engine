#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>

// The threads that currently hold an armed Steam null-callback recovery guard,
// readable WITHOUT thread-local storage.
//
// SteamOverlayInitVehHandler is registered once for the whole process and runs
// first for every exception on every thread. Asking "is THIS thread armed?"
// through a thread_local read is unsafe there: the loader's worker threads
// (LdrpProcessWork) and other threads that skip thread attach have no
// thread-local block for the module, so the read dereferences a null slot, the
// handler faults inside itself, and the nested fault re-enters the handler until
// the stack is gone. The sanitizer pass found it as a silent 0xC0000005 in the
// first window-creating test after the handler was registered (the parallel
// loader mapped a DLL and AddressSanitizer took a shadow-commit fault on that
// worker thread). A game process can raise the same exception on such a thread,
// and there it would take the game down.
//
// Thread ids are what identifies an armed thread; GetCurrentThreadId reads the
// TEB directly and needs no thread-local block. The handler consults this set
// first and touches the thread_local recovery context only for a thread that
// armed a guard, which is by definition an ordinary thread.

namespace ce::steam_recovery {

enum class ArmResult {
    kArmed,         // this call registered the thread; the caller must Disarm(slot)
    kAlreadyArmed,  // an outer guard on the thread owns the registration
    kFull,          // no free slot: the thread is not armed
};

template <size_t Capacity>
class ArmedThreadSet {
public:
    static_assert(Capacity > 0, "the set needs at least one slot");
    static constexpr size_t kNoSlot = Capacity;

    constexpr ArmedThreadSet() = default;

    ArmResult Arm(DWORD threadId, size_t* slot) {
        *slot = kNoSlot;
        if (threadId == 0) {
            return ArmResult::kFull;
        }
        if (Contains(threadId)) {
            return ArmResult::kAlreadyArmed;
        }
        for (size_t i = 0; i < Capacity; ++i) {
            DWORD expected = 0;
            if (m_threads[i].compare_exchange_strong(expected, threadId, std::memory_order_acq_rel)) {
                *slot = i;
                return ArmResult::kArmed;
            }
        }
        return ArmResult::kFull;
    }

    void Disarm(size_t slot) {
        if (slot < Capacity) {
            m_threads[slot].store(0, std::memory_order_release);
        }
    }

    // Called from the process-wide vectored handler: no thread-local storage, no
    // calls, and not instrumented, so it cannot fault where the handler must not.
    __attribute__((no_sanitize("address"))) bool Contains(DWORD threadId) const {
        for (size_t i = 0; i < Capacity; ++i) {
            if (m_threads[i].load(std::memory_order_acquire) == threadId) {
                return threadId != 0;
            }
        }
        return false;
    }

private:
    std::atomic<DWORD> m_threads[Capacity] = {};
};

// A foreign Present runs on the presenting threads of one process; a handful is
// already generous.
inline constexpr size_t kArmedThreadCapacity = 16;

// Constant-initialized, so reading it needs no guard variable in the handler.
inline ArmedThreadSet<kArmedThreadCapacity> g_armedThreads;

}  // namespace ce::steam_recovery
