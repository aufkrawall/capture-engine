#include "present_stall_stack.h"
#include "present_stall_sampler_internal.h"

#include "hook/runtime/hook_common.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

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

namespace detail {

// One diagnostic slot. A competing call leaves the existing arm in place.
// The sampler owns a reference to the target for the entire stack copy.
struct SampleRequest {
    std::shared_ptr<void> target;
    DWORD threadId = 0;
    const char* what = nullptr;
    int64_t armedQpc = 0;
    uint64_t call = 0;
};

struct Sampler {
    explicit Sampler(const SamplerStartupOps& startup) : ops(startup) {}
    ~Sampler() {
        if (timer != nullptr) {
            ops.closeHandle(ops.context, timer);
        }
    }

    SamplerStartupOps ops;
    HANDLE timer = nullptr;
    StackCopy copy;
    StackSampleBudget budget;
    std::mutex requests;
    SampleRequest pending;
    std::atomic<uint64_t> armedCall{0};
    uint64_t nextCall = 0;
};

}  // namespace detail

namespace {

using detail::Sampler;

struct Wanted {
    Sampler* sampler;
    uint64_t call;
};

bool StillInsideTheCall(void* user) {
    const Wanted& wanted = *static_cast<const Wanted*>(user);
    return wanted.sampler->armedCall.load(std::memory_order_acquire) == wanted.call;
}

DWORD WINAPI SamplerThread(void* parameter) {
    Sampler& sampler = *static_cast<Sampler*>(parameter);
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    for (;;) {
        if (WaitForSingleObject(sampler.timer, INFINITE) != WAIT_OBJECT_0) {
            return 0;
        }
        detail::SampleRequest request;
        {
            std::lock_guard lock(sampler.requests);
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            // A wake may belong to an earlier request canceled before we acquired the lock.
            if (!sampler.pending.target ||
                now.QuadPart - sampler.pending.armedQpc < frequency.QuadPart * kStackSampleAfterUs / 1'000'000) {
                continue;
            }
            request = std::move(sampler.pending);
        }
        Wanted wanted{&sampler, request.call};
        if (!StillInsideTheCall(&wanted) ||
            !sampler.copy.CopySuspended(request.target.get(), StillInsideTheCall, &wanted)) {
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
                ? static_cast<double>(sampled.QuadPart - request.armedQpc) * 1000.0 /
                      static_cast<double>(frequency.QuadPart)
                : 0.0;
        HookLogImportant("[PRESENT STALL] stack #%u %s blocked %.2fms (tid=%lu frames=%zu stackBytes=%zu):%s",
                         sampler.budget.taken(), request.what, blockedMs, static_cast<unsigned long>(request.threadId), count,
                         sampler.copy.bytes(), text);
    }
}

HANDLE CreateSamplerTimer(void*) {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    return timer != nullptr ? timer : CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
}

HANDLE CreateSamplerThread(void*, LPTHREAD_START_ROUTINE entry, void* argument) {
    return CreateThread(nullptr, 0, entry, argument, 0, nullptr);
}

void CloseSamplerHandle(void*, HANDLE handle) {
    CloseHandle(handle);
}

}  // namespace

namespace detail {

Sampler* StartSampler(const SamplerStartupOps& ops) {
    auto sampler = std::make_unique<Sampler>(ops);
    sampler->timer = ops.createTimer(ops.context);
    HANDLE thread = sampler->timer != nullptr && sampler->copy.valid()
                        ? ops.createThread(ops.context, SamplerThread, sampler.get())
                        : nullptr;
    if (thread == nullptr) {
        HookLogImportant("[PRESENT STALL] stack sampler unavailable (timer=%d buffer=%d error=%lu)",
                         sampler->timer != nullptr ? 1 : 0, sampler->copy.valid() ? 1 : 0, GetLastError());
        return nullptr;
    }
    SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST);
    ops.closeHandle(ops.context, thread);
    return sampler.release();
}

}  // namespace detail

namespace {

// Started on the first arm and kept for the process: the hook image stays
// resident until exit, and the thread only ever waits on its timer.
Sampler* SamplerInstance() {
    static Sampler* const s_sampler = detail::StartSampler(
        {nullptr, CreateSamplerTimer, CreateSamplerThread, CloseSamplerHandle});
    return s_sampler;
}

}  // namespace

uint64_t ArmStackSample(const char* what) {
#if defined(_M_X64)
    Sampler* sampler = SamplerInstance();
    if (sampler == nullptr || !sampler->budget.Allows(GetTickCount64())) {
        return 0;
    }
    std::unique_lock lock(sampler->requests, std::try_to_lock);
    if (!lock || sampler->armedCall.load(std::memory_order_relaxed) != 0) {
        return 0;
    }
    const DWORD threadId = GetCurrentThreadId();
    thread_local std::shared_ptr<void> self;
    if (!self) {
        HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                   FALSE, threadId);
        if (!handle) {
            return 0;
        }
        self.reset(handle, [](HANDLE h) { CloseHandle(h); });
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    LARGE_INTEGER due{};
    due.QuadPart = -kStackSampleAfterUs * 10;  // relative, 100 ns units
    if (!SetWaitableTimer(sampler->timer, &due, 0, nullptr, nullptr, FALSE)) {
        return 0;
    }
    const uint64_t call = ++sampler->nextCall;
    sampler->pending = {self, threadId, what, now.QuadPart, call};
    sampler->armedCall.store(call, std::memory_order_release);
    return call;
#else
    (void)what;
    return 0;
#endif
}

void DisarmStackSample(uint64_t call) {
    if (call == 0) {
        return;
    }
    Sampler* sampler = SamplerInstance();
    std::lock_guard lock(sampler->requests);
    if (sampler->armedCall.load(std::memory_order_relaxed) == call) {
        sampler->armedCall.store(0, std::memory_order_release);
        sampler->pending = {};
        CancelWaitableTimer(sampler->timer);
    }
}

}  // namespace ce::present_stall
