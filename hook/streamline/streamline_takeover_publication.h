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

    template <typename Load, typename Prepare, typename Bind, typename Publish>
    Module LoadAndPublish(Load load, Prepare prepare, Bind bind, Publish publish) {
        if (Module ready = Query())
            return ready;
        if (failed_.load(std::memory_order_acquire))
            return Module{};
        Module loaded = load();
        const bool prepared = loaded && prepare(loaded);
        Finalization expected = Finalization::Pending;
        if (!finalization_.compare_exchange_strong(expected, Finalization::Running, std::memory_order_acq_rel)) {
            // Entry callers join only after leaving load/prepare. DLL callbacks
            // use Query(), which remains nonblocking throughout import binding.
            while (expected != Finalization::Done) {
                finalization_.wait(expected, std::memory_order_acquire);
                expected = finalization_.load(std::memory_order_acquire);
            }
            return Query();
        }
        bool bound = false;
        try {
            bound = prepared && bind(loaded);
        } catch (...) {
            bound = false;
        }
        // Binding runs once, outside application locks. Only local prepared
        // data is published here; no loader calls or callbacks into lock owners.
        std::lock_guard<std::mutex> lock(publicationMutex_);
        if (!bound) {
            failed_.store(true, std::memory_order_release);
        } else {
            publish(loaded);
            module_.store(loaded, std::memory_order_release);
        }
        finalization_.store(Finalization::Done, std::memory_order_release);
        finalization_.notify_all();
        return Query();
    }

    template <typename Load, typename Prepare, typename Publish>
    Module LoadAndPublish(Load load, Prepare prepare, Publish publish) {
        return LoadAndPublish(load, prepare, [](Module) { return true; }, publish);
    }

    template <typename Load, typename Prepare>
    Module LoadAndPublish(Load load, Prepare prepare) {
        return LoadAndPublish(load, prepare, [](Module) {});
    }

    // One caller performs optional work after publication.
    bool ClaimBinding() { return !bindingClaimed_.exchange(true, std::memory_order_acq_rel); }

private:
    enum class Finalization { Pending, Running, Done };
    std::atomic<Finalization> finalization_{Finalization::Pending};
    std::atomic<Module> module_{};
    std::mutex publicationMutex_;
    std::atomic<bool> failed_{false};
    std::atomic<bool> bindingClaimed_{false};
};

}  // namespace ce::streamline_interposer_takeover
