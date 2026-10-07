#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace ce::runtime::detail {
enum class ChildRole { Inject, Media, Logger, Sensors };
using ChildToken = uintptr_t;
enum class ChildWait { Exited, Messages, Timeout, Failed };

// Private operating-system seam. The lifecycle below, not its adapters, owns
// admission, generations and every active/retired process token.
class ChildProcessEffects {
public:
    virtual ~ChildProcessEffects() = default;
    virtual ChildToken Spawn(ChildRole role) = 0;
    virtual bool Running(ChildToken process) const = 0;
    virtual bool Connected(ChildRole role) const = 0;
    virtual void Disconnect(ChildRole role) = 0;
    virtual void RequestShutdown(ChildRole role) = 0;
    virtual void Close(ChildToken process) = 0;
    virtual bool Terminate(ChildToken process) = 0;
    virtual uint64_t NowMs() const = 0;
    virtual ChildWait Wait(const ChildToken* processes, size_t count, uint32_t timeoutMs) = 0;
    virtual void PumpMessages() = 0;
    virtual bool AcceptingWork() const = 0;
};

class NativeHostChildren;

class ChildProcessLifecycle {
public:
    explicit ChildProcessLifecycle(ChildProcessEffects& effects) : effects_(effects) {}
    ChildProcessLifecycle(const ChildProcessLifecycle&) = delete;
    ChildProcessLifecycle& operator=(const ChildProcessLifecycle&) = delete;

    bool Ensure(ChildRole role, uint32_t timeoutMs) {
        auto& slot = slots_[Index(role)];
        if (closing_ || !effects_.AcceptingWork() || slot.installing)
            return false;
        struct Installing {
            explicit Installing(bool& value) : value_(value) {
                value_ = true;
            }
            ~Installing() {
                value_ = false;
            }
            bool& value_;
        } installing(slot.installing);
        CollectRetired();
        if (Ready(role))
            return true;
        const uint64_t ticket = slot.generation;
        if (slot.process && effects_.Running(slot.process)) {
            // A broken authenticated channel must not be attributed to a new
            // child while the old process still exists.
            const uint64_t deadline = effects_.NowMs() + std::min<uint32_t>(timeoutMs, 2000);
            while (effects_.Running(slot.process)) {
                const uint64_t now = effects_.NowMs();
                if (now >= deadline)
                    return false;
                const ChildToken process = slot.process;
                const auto wait = effects_.Wait(&process, 1, static_cast<uint32_t>(deadline - now));
                if (wait == ChildWait::Messages)
                    effects_.PumpMessages();
                if (closing_ || slot.generation != ticket || !effects_.AcceptingWork())
                    return false;
                if (wait == ChildWait::Timeout || wait == ChildWait::Failed)
                    return false;
            }
        }
        if (slot.process) {
            effects_.Disconnect(role);
            effects_.Close(slot.process);
            slot.process = 0;
        }
        const uint64_t launch = ++slot.generation;
        const ChildToken process = effects_.Spawn(role);
        if (!process)
            return false;
        // Adopt before any allocation/callback: even an obsolete successful
        // spawn must never become an unowned process on an error path.
        slot.process = process;
        slot.ready = false;
        if (closing_ || launch != slot.generation || !effects_.AcceptingWork()) {
            // A callback can stop startup before Spawn returns. Its successful
            // result still belongs to us, but may not resurrect the active slot.
            effects_.RequestShutdown(role);
            effects_.Disconnect(role);
            retired_.push_back({role, process, launch});
            slot.process = 0;
            CollectRetired();
            return false;
        }
        slot.ready = true;
        return Ready(role);
    }

    bool Ready(ChildRole role) const {
        const auto& slot = slots_[Index(role)];
        return !closing_ && slot.ready && slot.process && effects_.Running(slot.process) && effects_.Connected(role);
    }
    bool Present(ChildRole role) const {
        return slots_[Index(role)].process != 0;
    }
    bool Running(ChildRole role) const {
        const auto process = slots_[Index(role)].process;
        return process && effects_.Running(process);
    }
    uint64_t Generation(ChildRole role) const {
        return slots_[Index(role)].generation;
    }

    // Media finalizes asynchronously. Retirement removes its active identity
    // immediately, allowing a fresh recording, while retaining the old process
    // until actual exit. It never closes a live process just to forget it.
    void Retire(ChildRole role) {
        auto& slot = slots_[Index(role)];
        ++slot.generation;
        slot.ready = false;
        effects_.Disconnect(role);
        if (slot.process) {
            retired_.push_back({role, slot.process, slot.generation - 1});
            slot.process = 0;
        }
        CollectRetired();
    }
    void CollectRetired() {
        for (auto it = retired_.begin(); it != retired_.end();) {
            if (effects_.Running(it->process)) {
                ++it;
            } else {
                effects_.Close(it->process);
                it = retired_.erase(it);
            }
        }
    }
    bool HasRetired(ChildRole role) const {
        return std::any_of(retired_.begin(), retired_.end(),
                           [role](const Retired& child) { return child.role == role; });
    }

    void BeginShutdown() {
        if (beginningShutdown_)
            return;
        struct Beginning {
            explicit Beginning(bool& value) : value_(value) {
                value_ = true;
            }
            ~Beginning() {
                value_ = false;
            }
            bool& value_;
        } beginning(beginningShutdown_);
        // Reserve before admission changes; failed allocation retains every slot.
        // A shutdown effect that throws can be retried without forgetting active children.
        retired_.reserve(retired_.size() + slots_.size());
        closing_ = true;
        for (size_t i = 0; i < slots_.size(); ++i) {
            const auto role = static_cast<ChildRole>(i);
            if (slots_[i].process || slots_[i].installing)
                effects_.RequestShutdown(role);
            Retire(role);
        }
    }
    bool Drain(uint32_t timeoutMs, bool forceAfterTimeout) {
        const uint64_t deadline = effects_.NowMs() + timeoutMs;
        while (true) {
            CollectRetired();
            if (retired_.empty())
                return true;
            const uint64_t now = effects_.NowMs();
            if (now >= deadline)
                break;
            std::vector<ChildToken> waiting;
            waiting.reserve(retired_.size());
            for (const auto& child : retired_)
                waiting.push_back(child.process);
            const auto result = effects_.Wait(waiting.data(), waiting.size(), static_cast<uint32_t>(deadline - now));
            if (result == ChildWait::Messages)
                effects_.PumpMessages();
            if (result == ChildWait::Timeout || result == ChildWait::Failed)
                break;
        }
        if (forceAfterTimeout) {
            // TerminateProcess is asynchronous. Keep ownership if exit has not
            // been observed; callers must not pretend that teardown completed.
            std::vector<ChildToken> terminating;
            terminating.reserve(retired_.size());
            for (const auto& child : retired_)
                terminating.push_back(child.process);
            for (const auto process : terminating) {
                if (!Owns(process) || !effects_.Terminate(process))
                    continue;
                while (Owns(process) && effects_.Running(process)) {
                    const auto result = effects_.Wait(&process, 1, UINT32_MAX);
                    if (result == ChildWait::Messages)
                        effects_.PumpMessages();
                    if (result == ChildWait::Failed || result == ChildWait::Timeout)
                        break;
                }
            }
            CollectRetired();
        }
        return retired_.empty();
    }

private:
    friend class NativeHostChildren;
    struct Slot {
        ChildToken process = 0;
        uint64_t generation = 0;
        bool installing = false;
        bool ready = false;
    };
    struct Retired {
        ChildRole role;
        ChildToken process;
        uint64_t generation;
    };
    static size_t Index(ChildRole role) {
        return static_cast<size_t>(role);
    }
    ChildToken Process(ChildRole role) const {
        return slots_[Index(role)].process;
    }
    bool Owns(ChildToken process) const {
        return std::any_of(retired_.begin(), retired_.end(),
                           [process](const Retired& child) { return child.process == process; });
    }
    ChildProcessEffects& effects_;
    std::array<Slot, 4> slots_{};
    std::vector<Retired> retired_;
    bool closing_ = false;
    bool beginningShutdown_ = false;
};
}  // namespace ce::runtime::detail
