#pragma once

namespace DXGIShared::detail {
// Vtable, inline, Present1 fallback and foreign forwarding views share one
// receiver transaction. Independent nested swapchains keep independent pacing.
class PresentPacingScope {
public:
    PresentPacingScope(const void* receiver, bool allowed)
        : receiver_(receiver), allowed_(allowed), previous_(current_), owner_(this) {
        for (auto* prior = previous_; prior; prior = prior->previous_) {
            if (prior->receiver_ == receiver) { owner_ = prior->owner_; break; }
        }
        current_ = this;
    }
    ~PresentPacingScope() { current_ = previous_; }
    PresentPacingScope(const PresentPacingScope&) = delete;
    PresentPacingScope& operator=(const PresentPacingScope&) = delete;
    void Suppress() { owner_->allowed_ = false; }
    static bool ClaimWait(const void* receiver) {
        for (auto* scope = current_; scope; scope = scope->previous_) {
            if (scope->receiver_ != receiver) continue;
            auto* owner = scope->owner_;
            if (!scope->allowed_ || !owner->allowed_ || owner->waited_) return false;
            owner->waited_ = true;
            return true;
        }
        return true;
    }
private:
    inline static thread_local PresentPacingScope* current_ = nullptr;
    const void* receiver_;
    bool allowed_, waited_ = false;
    PresentPacingScope* previous_;
    PresentPacingScope* owner_;
};
}  // namespace DXGIShared::detail
