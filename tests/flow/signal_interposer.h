#pragma once

#include "tests/flow/flow_host.h"
#include "tests/flow/queue_dispatch_probe.h"
#include "tests/flow/call_barrier.h"

#include <thread>

namespace ce::flow {

// Independent foreign code joins the physical chain. It never chooses a CE target or mocks CE policy.
class SignalInterposer {
public:
    using Method = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);

    SignalInterposer(FlowGame& game, QueueDispatchProbe& probe, CallBarrier* barrier = nullptr)
        : game_(game),
          probe_(probe),
          slot_(&(*reinterpret_cast<void***>(&probe))[14]),
          next_(reinterpret_cast<Method>(probe.CurrentSignal())),
          barrier_(barrier) {
        SignalInterposer* empty = nullptr;
        EXPECT_TRUE(current_.compare_exchange_strong(empty, this));
        installed_ = Replace(next_, &Detour);
        EXPECT_TRUE(installed_);
    }
    ~SignalInterposer() {
        EXPECT_EQ(active_.load(), 0u);
        if (installed_ && !Detach()) {
            // CE installed above this provider; restore that layer first, then remove this layer.
            EXPECT_TRUE(game_.RemoveSignalQueue(&probe_));
            EXPECT_TRUE(Detach());
        }
        current_.store(nullptr, std::memory_order_release);
    }
    SignalInterposer(const SignalInterposer&) = delete;
    SignalInterposer& operator=(const SignalInterposer&) = delete;

    bool Installed() const {
        return installed_;
    }
    bool Detach() {
        if (!installed_)
            return true;
        if (!Replace(&Detour, next_))
            return false;
        installed_ = false;
        return true;
    }
    uint32_t Calls() const {
        return calls_.load();
    }
    uint32_t ActiveCalls() const {
        return active_.load();
    }
    void* Entry() const {
        return reinterpret_cast<void*>(&Detour);
    }

private:
    bool Replace(Method expected, Method replacement) {
        DWORD previous = 0;
        if (!VirtualProtect(slot_, sizeof(void*), PAGE_READWRITE, &previous))
            return false;
        void* before =
            InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(slot_),
                                              reinterpret_cast<void*>(replacement), reinterpret_cast<void*>(expected));
        DWORD unused = 0;
        const bool protectedAgain = VirtualProtect(slot_, sizeof(void*), previous, &unused) != FALSE;
        EXPECT_TRUE(protectedAgain);
        return before == reinterpret_cast<void*>(expected);
    }
    static HRESULT STDMETHODCALLTYPE Detour(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
        SignalInterposer* owner = current_.load(std::memory_order_acquire);
        if (!owner)
            return E_FAIL;
        ++owner->active_;
        ++owner->calls_;
        if (owner->barrier_)
            owner->barrier_->Enter();
        const HRESULT result = owner->next_(queue, fence, value);
        --owner->active_;
        return result;
    }

    inline static std::atomic<SignalInterposer*> current_{nullptr};
    FlowGame& game_;
    QueueDispatchProbe& probe_;
    void** const slot_;
    const Method next_;
    CallBarrier* const barrier_;
    std::atomic<uint32_t> calls_{0};
    std::atomic<uint32_t> active_{0};
    bool installed_ = false;
};

// Release and join on every exit, including a fatal assertion in the test body.
class BlockedSignalCall {
public:
    BlockedSignalCall(QueueDispatchProbe& probe, CallBarrier& barrier)
        : barrier_(barrier),
          thread_([this, &probe] {
              result_ = probe.InvokeSignal(91);
              barrier_.Complete();
          }) {}
    ~BlockedSignalCall() {
        Finish();
    }
    HRESULT Finish() {
        barrier_.Release();
        if (thread_.joinable())
            thread_.join();
        return result_;
    }

private:
    CallBarrier& barrier_;
    HRESULT result_ = E_FAIL;
    std::thread thread_;
};

}  // namespace ce::flow
