#pragma once

#include "tests/flow/call_barrier.h"
#include "tests/flow/flow_host.h"

#include <atomic>
#include <thread>
#include <type_traits>

namespace ce::flow {

// Independent foreign code forwards through the physical entry it captured, without CE policy.
template <bool IsPresent1>
class BasicPresentInterposer {
public:
    using Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    using Method = std::conditional_t<IsPresent1, Present1, Present>;

    explicit BasicPresentInterposer(IDXGISwapChain* swapchain, bool nestedTest = false, CallBarrier* barrier = nullptr)
        : swapchain_(swapchain),
          slot_(&(*reinterpret_cast<void***>(swapchain))[IsPresent1 ? 22 : 8]),
          next_(reinterpret_cast<Method>(*slot_)),
          nestedTest_(nestedTest),
          barrier_(barrier) {
        BasicPresentInterposer* empty = nullptr;
        claimed_ = current_.compare_exchange_strong(empty, this);
        EXPECT_TRUE(claimed_);
        if (claimed_)
            installed_ = Replace(next_, DetourEntry());
        EXPECT_TRUE(installed_);
    }
    ~BasicPresentInterposer() {
        EXPECT_EQ(active_.load(), 0u);
        if (installed_)
            EXPECT_TRUE(Detach());
        if (claimed_)
            current_.store(nullptr, std::memory_order_release);
    }
    BasicPresentInterposer(const BasicPresentInterposer&) = delete;
    BasicPresentInterposer& operator=(const BasicPresentInterposer&) = delete;

    bool Installed() const {
        return installed_;
    }
    bool Detach() {
        if (!installed_)
            return true;
        if (!Replace(DetourEntry(), next_))
            return false;
        installed_ = false;
        return true;
    }
    void* CurrentEntry() const {
        return *slot_;
    }
    void* Entry() const {
        return reinterpret_cast<void*>(DetourEntry());
    }
    uint32_t Calls() const {
        return calls_.load();
    }
    uint32_t ActiveCalls() const {
        return active_.load();
    }
    HRESULT TestPresent() {
        DXGI_PRESENT_PARAMETERS parameters{};
        return Invoke(reinterpret_cast<Method>(*slot_), swapchain_.Get(), 0, DXGI_PRESENT_TEST, &parameters);
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
    static Method DetourEntry() {
        if constexpr (IsPresent1)
            return &Detour1;
        else
            return &Detour;
    }
    static HRESULT Invoke(Method target, IDXGISwapChain* swapchain, UINT interval, UINT flags,
                          const DXGI_PRESENT_PARAMETERS* parameters) {
        if constexpr (IsPresent1)
            return target(swapchain, interval, flags, parameters);
        else
            return target(swapchain, interval, flags);
    }
    static HRESULT STDMETHODCALLTYPE Detour(IDXGISwapChain* swapchain, UINT interval, UINT flags) {
        return Intercept(swapchain, interval, flags, nullptr);
    }
    static HRESULT STDMETHODCALLTYPE Detour1(IDXGISwapChain* swapchain, UINT interval, UINT flags,
                                             const DXGI_PRESENT_PARAMETERS* parameters) {
        return Intercept(swapchain, interval, flags, parameters);
    }
    static HRESULT Intercept(IDXGISwapChain* swapchain, UINT interval, UINT flags,
                             const DXGI_PRESENT_PARAMETERS* parameters) {
        auto* owner = current_.load(std::memory_order_acquire);
        if (!owner)
            return E_FAIL;
        ++owner->active_;
        ++owner->calls_;
        if (owner->barrier_)
            owner->barrier_->Enter();
        if (owner->nestedTest_ && !(flags & DXGI_PRESENT_TEST)) {
            const HRESULT probe = Invoke(owner->next_, swapchain, interval, flags | DXGI_PRESENT_TEST, parameters);
            EXPECT_TRUE(SUCCEEDED(probe));
        }
        const HRESULT result = Invoke(owner->next_, swapchain, interval, flags, parameters);
        --owner->active_;
        return result;
    }

    inline static std::atomic<BasicPresentInterposer*> current_{nullptr};
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

using PresentInterposer = BasicPresentInterposer<false>;
using Present1Interposer = BasicPresentInterposer<true>;

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
