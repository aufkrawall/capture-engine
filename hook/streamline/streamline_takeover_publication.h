#pragma once

#include <atomic>
#include <mutex>

namespace ce::streamline_interposer_takeover {

// The Windows loader serializes concurrent loads of the same absolute path.
// Never add an application lock around load/prepare: a DLL callback may query
// this publication while holding the loader lock needed by another caller.
template <typename Module>
class RuntimePublication {
public:
    Module Query() const { return module_.load(std::memory_order_acquire); }

    template <typename Load, typename Prepare, typename Publish>
    Module LoadAndPublish(Load load, Prepare prepare, Publish publish) {
        if (Module ready = Query())
            return ready;
        if (failed_.load(std::memory_order_acquire))
            return Module{};
        Module loaded = load();
        const bool prepared = loaded && prepare(loaded);
        // Only publish local prepared data here; no loader calls or callbacks
        // into other lock owners. A failure and a competing success therefore
        // cannot both commit contradictory process-global runtime targets.
        std::lock_guard<std::mutex> lock(publicationMutex_);
        if (Module ready = Query())
            return ready;
        if (failed_.load(std::memory_order_relaxed))
            return Module{};
        if (!prepared) {
            failed_.store(true, std::memory_order_release);
            return Module{};
        }
        publish(loaded);
        module_.store(loaded, std::memory_order_release);
        return loaded;
    }

    template <typename Load, typename Prepare>
    Module LoadAndPublish(Load load, Prepare prepare) {
        return LoadAndPublish(load, prepare, [](Module) {});
    }

    // Export targets are ready before publication. One caller finishes IAT
    // binding; peers may already forward through the prepared takeover thunks.
    bool ClaimBinding() { return !bindingClaimed_.exchange(true, std::memory_order_acq_rel); }

private:
    std::atomic<Module> module_{};
    std::mutex publicationMutex_;
    std::atomic<bool> failed_{false};
    std::atomic<bool> bindingClaimed_{false};
};

}  // namespace ce::streamline_interposer_takeover
