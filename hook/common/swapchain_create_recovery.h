#pragma once

namespace ce::swapchain_create {

// Inline and below-foreign-chain hooks observe the same logical create. Only its
// outermost scope may recover. A different HWND is an independent nested create.
class RecoveryScope {
public:
    explicit RecoveryScope(const void* window) : previous_(active_), window_(window) {
        owner_ = this;
        for (auto* scope = previous_; scope; scope = scope->previous_) {
            if (scope->window_ == window_) {
                owner_ = scope->owner_;
                ++owner_->nestedCalls_;
                break;
            }
        }
        active_ = this;
    }
    ~RecoveryScope() { active_ = previous_; }
    RecoveryScope(const RecoveryScope&) = delete;
    RecoveryScope& operator=(const RecoveryScope&) = delete;
    bool OwnsRecovery() const { return owner_ == this; }
    unsigned NestedCalls() const { return owner_->nestedCalls_; }

private:
    inline static thread_local RecoveryScope* active_ = nullptr;
    RecoveryScope* previous_;
    RecoveryScope* owner_;
    const void* window_;
    unsigned nestedCalls_ = 0;
};

}  // namespace ce::swapchain_create
