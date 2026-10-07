#pragma once

#include "tests/flow/call_barrier.h"
#include "tests/flow/flow_host.h"

#include <atomic>
#include <thread>

namespace ce::flow {

// Independent foreign code forwards through the physical entry it captured, without CE policy.
class PresentInterposer {
public:
    using Method = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

    explicit PresentInterposer(IDXGISwapChain* swapchain, bool nestedTest = false, CallBarrier* barrier = nullptr)
        : swapchain_(swapchain),
          slot_(&(*reinterpret_cast<void***>(swapchain))[8]),
          next_(reinterpret_cast<Method>(*slot_)),
          nestedTest_(nestedTest),
          barrier_(barrier) {
        PresentInterposer* empty = nullptr;
        claimed_ = current_.compare_exchange_strong(empty, this);
        EXPECT_TRUE(claimed_);
        if (claimed_)
            installed_ = Replace(next_, &Detour);
        EXPECT_TRUE(installed_);
    }
    ~PresentInterposer() {
        EXPECT_EQ(active_.load(), 0u);
        if (installed_)
            EXPECT_TRUE(Detach());
        if (claimed_)
            current_.store(nullptr, std::memory_order_release);
    }
    PresentInterposer(const PresentInterposer&) = delete;
    PresentInterposer& operator=(const PresentInterposer&) = delete;

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
    void* CurrentEntry() const {
        return *slot_;
    }
    void* Entry() const {
        return reinterpret_cast<void*>(&Detour);
    }
    uint32_t Calls() const {
        return calls_.load();
    }
    uint32_t ActiveCalls() const {
        return active_.load();
    }
    HRESULT TestPresent() {
        return swapchain_->Present(0, DXGI_PRESENT_TEST);
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
        EXPECT_TRUE(VirtualProtect(slot_, sizeof(void*), previous, &unused));
        return before == reinterpret_cast<void*>(expected);
    }
    static HRESULT STDMETHODCALLTYPE Detour(IDXGISwapChain* swapchain, UINT interval, UINT flags) {
        auto* owner = current_.load(std::memory_order_acquire);
        if (!owner)
            return E_FAIL;
        ++owner->active_;
        ++owner->calls_;
        if (owner->barrier_)
            owner->barrier_->Enter();
        if (owner->nestedTest_ && !(flags & DXGI_PRESENT_TEST)) {
            const HRESULT probe = owner->next_(swapchain, interval, flags | DXGI_PRESENT_TEST);
            EXPECT_TRUE(SUCCEEDED(probe));
        }
        const HRESULT result = owner->next_(swapchain, interval, flags);
        --owner->active_;
        return result;
    }

    inline static std::atomic<PresentInterposer*> current_{nullptr};
    ComPtr<IDXGISwapChain> swapchain_;
    void** const slot_;
    const Method next_;
    const bool nestedTest_;
    CallBarrier* const barrier_;
    std::atomic<uint32_t> calls_{0};
    std::atomic<uint32_t> active_{0};
    bool claimed_ = false;
    bool installed_ = false;
};

class BlockedPresentCall {
public:
    BlockedPresentCall(PresentInterposer& interposer, CallBarrier& barrier)
        : barrier_(barrier),
          thread_([this, &interposer] {
              result_ = interposer.TestPresent();
              barrier_.Complete();
          }) {}
    ~BlockedPresentCall() {
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
