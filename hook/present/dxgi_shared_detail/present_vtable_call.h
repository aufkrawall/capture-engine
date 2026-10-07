#pragma once

#include <dxgi1_2.h>

// Private production call transaction, exposed here for focused native tests.
// Targets and physical binding publication remain owned by the vtable installer.
namespace DXGIShared::detail {
template <typename Target>
class ScopedVTableCall {
public:
    ScopedVTableCall(IDXGISwapChain* receiver, Target target, const void* caller = nullptr)
        : receiver_(receiver),
          target_(target),
          caller_(caller),
          previous_(current_) {
        current_ = this;
    }
    ~ScopedVTableCall() {
        current_ = previous_;
    }
    ScopedVTableCall(const ScopedVTableCall&) = delete;
    ScopedVTableCall& operator=(const ScopedVTableCall&) = delete;

    static const void* Caller(IDXGISwapChain* receiver, const void* inlineCaller) {
        const auto* call = current_;
        return call && call->receiver_ == receiver && !call->forwarding_ && call->caller_ ? call->caller_
                                                                                          : inlineCaller;
    }

    template <typename... Arguments>
    static bool TryForward(IDXGISwapChain* receiver, HRESULT& result, Arguments... arguments) {
        auto* call = current_;
        if (!call || call->receiver_ != receiver || call->forwarding_)
            return false;
        // The captured native entry may itself reach CE's inline/body detour.
        // That invocation must not restart this vtable's foreign predecessor.
        struct ForwardScope {
            explicit ForwardScope(ScopedVTableCall& frame) : frame_(frame) {
                frame_.forwarding_ = true;
            }
            ~ForwardScope() {
                frame_.forwarding_ = false;
            }
            ScopedVTableCall& frame_;
        } forwarding(*call);
        result = call->target_ ? call->target_(receiver, arguments...) : DXGI_ERROR_INVALID_CALL;
        return true;
    }

private:
    inline static thread_local ScopedVTableCall* current_ = nullptr;
    IDXGISwapChain* const receiver_;
    const Target target_;
    const void* const caller_;
    ScopedVTableCall* const previous_;
    bool forwarding_ = false;
};
}  // namespace DXGIShared::detail
