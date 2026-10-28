#include "present_stall_stack.h"

#include "hook/runtime/hook_common.h"

#include <cstdio>
#include <cstring>

namespace ce::present_stall {

bool StackSampleBudget::Allows(uint64_t nowMs) const {
    if (taken_.load(std::memory_order_relaxed) < kBurst) {
        return true;
    }
    return nowMs - lastMs_.load(std::memory_order_relaxed) >= kIntervalMs;
}

void StackSampleBudget::Taken(uint64_t nowMs) {
    taken_.fetch_add(1, std::memory_order_relaxed);
    lastMs_.store(nowMs, std::memory_order_relaxed);
}

StackCopy::StackCopy() {
    data_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, kBytes + kSlack, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}

StackCopy::~StackCopy() {
    if (data_ != nullptr) {
        VirtualFree(data_, 0, MEM_RELEASE);
    }
}

bool StackCopy::CopySuspended(HANDLE thread, bool (*stillWanted)(void*), void* user) {
    bytes_ = 0;
#if defined(_M_X64)
    if (thread == nullptr || data_ == nullptr || SuspendThread(thread) == static_cast<DWORD>(-1)) {
        return false;
    }
    // Held suspended from here to ResumeThread: system calls and copies only.
    context_ = CONTEXT{};
    context_.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(thread, &context_) && (stillWanted == nullptr || stillWanted(user))) {
        const uintptr_t stackPointer = static_cast<uintptr_t>(context_.Rsp);
        MEMORY_BASIC_INFORMATION region{};
        if (stackPointer != 0 &&
            VirtualQuery(reinterpret_cast<const void*>(stackPointer), &region, sizeof(region)) == sizeof(region) &&
            region.State == MEM_COMMIT) {
            // The committed region holding the stack pointer ends at the stack's base.
            const uintptr_t end = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
            const size_t available = end > stackPointer ? end - stackPointer : 0;
            bytes_ = available < kBytes ? available : kBytes;
            std::memcpy(data_, reinterpret_cast<const void*>(stackPointer), bytes_);
            low_ = stackPointer;
        }
    }
    ResumeThread(thread);
#else
    (void)thread;
    (void)stillWanted;
    (void)user;
#endif
    return bytes_ != 0;
}

size_t UnwindStackCopy(const StackCopy& copy, uint64_t* frames, size_t maxFrames) {
#if defined(_M_X64)
    if (frames == nullptr || maxFrames == 0 || copy.bytes() == 0) {
        return 0;
    }
    CONTEXT context = copy.context();
    const uint64_t low = copy.low();
    const uint64_t high = low + copy.bytes();
    const uint64_t base = reinterpret_cast<uint64_t>(copy.data());
    const auto rebase = [&](DWORD64& reg) {
        if (reg >= low && reg < high) {
            reg = reg - low + base;
        }
    };
    // A restored nonvolatile register that held a stack address holds the
    // sampled thread's address: move it onto the copy before it is used.
    const auto rebaseAll = [&] {
        DWORD64* registers[] = {&context.Rsp, &context.Rbp, &context.Rbx, &context.Rsi, &context.Rdi,
                                &context.R12, &context.R13, &context.R14, &context.R15};
        for (DWORD64* reg : registers) {
            rebase(*reg);
        }
    };
    const auto inCopy = [&](DWORD64 address) { return address >= base && address + 8 <= base + copy.bytes(); };
    rebaseAll();
    size_t count = 0;
    while (count < maxFrames && context.Rip != 0 && inCopy(context.Rsp)) {
        frames[count++] = context.Rip;
        const DWORD64 previousSp = context.Rsp;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (function == nullptr) {
            // A leaf function: its return address is on top of the stack.
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += 8;
        } else {
            PVOID handlerData = nullptr;
            DWORD64 establisherFrame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                             &establisherFrame, nullptr);
        }
        rebaseAll();
        if (context.Rsp <= previousSp) {
            break;  // a walk that does not move up the stack is lost
        }
    }
    return count;
#else
    (void)copy;
    (void)frames;
    (void)maxFrames;
    return 0;
#endif
}

int FormatStackFrames(const uint64_t* frames, size_t count, char* buffer, size_t bufferSize) {
    if (buffer == nullptr || bufferSize == 0) {
        return 0;
    }
    buffer[0] = '\0';
    size_t used = 0;
    for (size_t i = 0; i < count && used + 1 < bufferSize; ++i) {
        const void* address = reinterpret_cast<const void*>(static_cast<uintptr_t>(frames[i]));
        HMODULE module = nullptr;
        char name[64] = {};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<const char*>(address), &module) &&
            module != nullptr) {
            char path[MAX_PATH] = {};
            const DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
            const char* slash = length != 0 ? std::strrchr(path, '\\') : nullptr;
            std::snprintf(name, sizeof(name), "%s", slash != nullptr ? slash + 1 : (length != 0 ? path : "?"));
        }
        const int written =
            module != nullptr
                ? std::snprintf(buffer + used, bufferSize - used, " %s+0x%llx", name,
                                static_cast<unsigned long long>(frames[i] - reinterpret_cast<uintptr_t>(module)))
                : std::snprintf(buffer + used, bufferSize - used, " 0x%llx", static_cast<unsigned long long>(frames[i]));
        if (written < 0) {
            break;
        }
        used += static_cast<size_t>(written) < bufferSize - used ? static_cast<size_t>(written) : bufferSize - used - 1;
    }
    return static_cast<int>(used);
}

namespace {

// The armed down-call. One slot: a second presenting thread re-arms it for its
// own call, and the sample goes to whichever call is armed when it fires.
struct Sampler {
    HANDLE timer = nullptr;
    StackCopy copy;
    StackSampleBudget budget;
    std::atomic<HANDLE> target{nullptr};
    std::atomic<DWORD> targetThreadId{0};
    std::atomic<int64_t> armedQpc{0};
    std::atomic<uint64_t> armedCall{0};
    std::atomic<uint64_t> doneCall{0};
    std::atomic<uint64_t> nextCall{0};
};

struct Wanted {
    Sampler* sampler;
    uint64_t call;
};

bool StillInsideTheCall(void* user) {
    const Wanted& wanted = *static_cast<const Wanted*>(user);
    return wanted.sampler->armedCall.load(std::memory_order_acquire) == wanted.call &&
           wanted.sampler->doneCall.load(std::memory_order_acquire) != wanted.call;
}

DWORD WINAPI SamplerThread(void* parameter) {
    Sampler& sampler = *static_cast<Sampler*>(parameter);
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    for (;;) {
        if (WaitForSingleObject(sampler.timer, INFINITE) != WAIT_OBJECT_0) {
            return 0;
        }
        Wanted wanted{&sampler, sampler.armedCall.load(std::memory_order_acquire)};
        if (wanted.call == 0 || !StillInsideTheCall(&wanted)) {
            continue;
        }
        if (!sampler.copy.CopySuspended(sampler.target.load(std::memory_order_acquire), StillInsideTheCall,
                                        &wanted)) {
            continue;
        }
        LARGE_INTEGER sampled{};
        QueryPerformanceCounter(&sampled);
        sampler.budget.Taken(GetTickCount64());
        uint64_t frames[kStackSampleFrames] = {};
        const size_t count = UnwindStackCopy(sampler.copy, frames, kStackSampleFrames);
        char text[1536];
        FormatStackFrames(frames, count, text, sizeof(text));
        const double blockedMs =
            frequency.QuadPart > 0
                ? static_cast<double>(sampled.QuadPart - sampler.armedQpc.load(std::memory_order_relaxed)) * 1000.0 /
                      static_cast<double>(frequency.QuadPart)
                : 0.0;
        HookLogImportant("[PRESENT STALL] stack #%u down-call blocked %.2fms (tid=%lu frames=%zu stackBytes=%zu):%s",
                         sampler.budget.taken(), blockedMs,
                         static_cast<unsigned long>(sampler.targetThreadId.load(std::memory_order_relaxed)), count,
                         sampler.copy.bytes(), text);
    }
}

// Started on the first arm and kept for the process: the hook image stays
// resident until exit, and the thread only ever waits on its timer.
Sampler* SamplerInstance() {
    static Sampler* const s_sampler = []() -> Sampler* {
        auto* sampler = new Sampler();
        sampler->timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                TIMER_ALL_ACCESS);
        if (sampler->timer == nullptr) {
            sampler->timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
        HANDLE thread = sampler->timer != nullptr && sampler->copy.valid()
                            ? CreateThread(nullptr, 0, SamplerThread, sampler, 0, nullptr)
                            : nullptr;
        if (thread == nullptr) {
            HookLogImportant("[PRESENT STALL] stack sampler unavailable (timer=%d buffer=%d error=%lu)",
                             sampler->timer != nullptr ? 1 : 0, sampler->copy.valid() ? 1 : 0, GetLastError());
            return nullptr;  // the partial sampler is kept: nothing references it again
        }
        SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST);
        CloseHandle(thread);
        return sampler;
    }();
    return s_sampler;
}

thread_local uint64_t t_armedCall = 0;

}  // namespace

void ArmStackSample() {
#if defined(_M_X64)
    t_armedCall = 0;
    Sampler* sampler = SamplerInstance();
    if (sampler == nullptr || !sampler->budget.Allows(GetTickCount64())) {
        return;
    }
    thread_local HANDLE t_self = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                            FALSE, GetCurrentThreadId());
    if (t_self == nullptr) {
        return;
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const uint64_t call = sampler->nextCall.fetch_add(1, std::memory_order_relaxed) + 1;
    sampler->target.store(t_self, std::memory_order_relaxed);
    sampler->targetThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    sampler->armedQpc.store(now.QuadPart, std::memory_order_relaxed);
    sampler->armedCall.store(call, std::memory_order_release);
    LARGE_INTEGER due{};
    due.QuadPart = -kStackSampleAfterUs * 10;  // relative, 100 ns units
    if (SetWaitableTimer(sampler->timer, &due, 0, nullptr, nullptr, FALSE)) {
        t_armedCall = call;
    }
#endif
}

void DisarmStackSample() {
    if (t_armedCall == 0) {
        return;
    }
    Sampler* sampler = SamplerInstance();
    const uint64_t call = t_armedCall;
    t_armedCall = 0;
    if (sampler == nullptr) {
        return;
    }
    sampler->doneCall.store(call, std::memory_order_release);
    if (sampler->armedCall.load(std::memory_order_acquire) == call) {
        CancelWaitableTimer(sampler->timer);
    }
}

}  // namespace ce::present_stall
