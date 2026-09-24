#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

// Where the Vulkan sharpen pass keeps its per-swapchain states, and how a state
// that must go is taken off the present thread's critical path.
//
// The pass used to keep ONE state per device. A device may own several live
// swapchains (a launcher plus the game window, an editor's viewports), and the
// second one either rebuilt the whole pipeline on every present or - after the
// 2026-09 fix - was left unfiltered. Each swapchain now owns its own state.
//
// Tearing a state down means waiting for CE's own submissions that still read
// it. On the present thread that wait is a stall (up to one second per slot),
// so a state that must go while its swapchain lives on - the filter switched
// off, or a rebuild because the present queue family, format or image count
// changed - is RETIRED instead: moved aside with its resources intact and
// destroyed on a later present once every fence it submitted has signalled.
// Only the paths that must not continue without it wait: the swapchain's
// destruction (its images die with it) and the device's.
//
// Lifetime rules this keeps, unchanged from the one-state design:
//   - every state over a swapchain, live or retired, is released before the
//     driver destroys that swapchain (and when it is retired as oldSwapchain);
//   - present-wait semaphores still go to the deferred store, destroyed only
//     after the swapchain's destruction returns;
//   - a state records the route it was built for; the compute route on a
//     compute-only present queue is chosen per state, per present.

namespace ce::vulkan_sharpen_registry {

template <typename State, typename Device>
class Registry {
public:
    // The state for `swapchain` on `device`, default-constructed on first use.
    State& Live(Device device, uint64_t swapchain) { return live_[Key{device, swapchain}]; }

    const State* FindLive(Device device, uint64_t swapchain) const {
        const auto it = live_.find(Key{device, swapchain});
        return it == live_.end() ? nullptr : &it->second;
    }

    // Moves the live state aside without touching its resources. The next
    // Live() for the same swapchain starts from a fresh state.
    void Retire(Device device, uint64_t swapchain) {
        const auto it = live_.find(Key{device, swapchain});
        if (it == live_.end()) {
            return;
        }
        retired_.push_back(Retired{device, swapchain, std::move(it->second)});
        live_.erase(it);
    }

    void RetireAll(Device device) {
        for (auto it = live_.begin(); it != live_.end();) {
            if (it->first.first == device) {
                retired_.push_back(Retired{device, it->first.second, std::move(it->second)});
                it = live_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Retired states on `device` for which `ready(state)` holds - the caller's
    // non-blocking "every submission has signalled" test.
    template <typename Ready>
    std::vector<State> TakeReadyRetired(Device device, Ready&& ready) {
        std::vector<State> taken;
        for (auto it = retired_.begin(); it != retired_.end();) {
            if (it->device == device && ready(it->state)) {
                taken.push_back(std::move(it->state));
                it = retired_.erase(it);
            } else {
                ++it;
            }
        }
        return taken;
    }

    // Every state, live or retired, built over `swapchain`: the destroy path.
    std::vector<State> TakeForSwapchain(Device device, uint64_t swapchain) {
        std::vector<State> taken;
        const auto live = live_.find(Key{device, swapchain});
        if (live != live_.end()) {
            taken.push_back(std::move(live->second));
            live_.erase(live);
        }
        for (auto it = retired_.begin(); it != retired_.end();) {
            if (it->device == device && it->swapchain == swapchain) {
                taken.push_back(std::move(it->state));
                it = retired_.erase(it);
            } else {
                ++it;
            }
        }
        return taken;
    }

    // Everything on `device`: the device teardown path.
    std::vector<State> TakeForDevice(Device device) {
        std::vector<State> taken;
        for (auto it = live_.begin(); it != live_.end();) {
            if (it->first.first == device) {
                taken.push_back(std::move(it->second));
                it = live_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = retired_.begin(); it != retired_.end();) {
            if (it->device == device) {
                taken.push_back(std::move(it->state));
                it = retired_.erase(it);
            } else {
                ++it;
            }
        }
        return taken;
    }

    size_t LiveCount(Device device) const {
        size_t count = 0;
        for (const auto& entry : live_) {
            count += entry.first.first == device ? 1 : 0;
        }
        return count;
    }

    size_t RetiredCount(Device device) const {
        size_t count = 0;
        for (const Retired& entry : retired_) {
            count += entry.device == device ? 1 : 0;
        }
        return count;
    }

private:
    using Key = std::pair<Device, uint64_t>;
    struct Retired {
        Device device;
        uint64_t swapchain;
        State state;
    };
    std::map<Key, State> live_;
    std::vector<Retired> retired_;
};

}  // namespace ce::vulkan_sharpen_registry
