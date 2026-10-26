#pragma once

#include <unordered_set>

// A Vulkan instance can exist only to enumerate adapters in a D3D game. Presentation ownership
// protects actual swapchain creation (the driver's WSI may call DXGI), survives overlapping and
// replacement swapchains and host reconnects, and ends when the last swapchain dies. Failed
// creation withdraws only its temporary ownership. The caller serializes these operations and
// supplies the actual shared-memory publisher; no loader or GPU work belongs in this policy.
namespace ce::vulkan_presentation {

template <typename Handle>
class Ownership {
public:
    template <typename Publish>
    void SetConnected(bool connected, Publish publish) {
        connected_ = connected;
        Update(publish);
    }

    template <typename Publish>
    void BeginCreation(Publish publish) {
        ++creating_;
        Update(publish);
    }

    // A null handle means failure. Retire old and add new in one operation so replacing the
    // sole swapchain never withdraws ownership during a frame-generation or resize transition.
    template <typename Publish>
    void CompleteCreation(Handle swapchain, Handle oldSwapchain, Publish publish) {
        if (creating_ != 0)
            --creating_;
        if (swapchain != Handle{}) {
            if (oldSwapchain != Handle{})
                swapchains_.erase(oldSwapchain);
            swapchains_.insert(swapchain);
        }
        Update(publish);
    }

    template <typename Publish>
    void Destroyed(Handle swapchain, Publish publish) {
        swapchains_.erase(swapchain);
        Update(publish);
    }

private:
    template <typename Publish>
    void Update(Publish publish) {
        const bool active = connected_ && (creating_ != 0 || !swapchains_.empty());
        if (active != published_) {
            published_ = active;
            publish(active);
        }
    }

    std::unordered_set<Handle> swapchains_;
    unsigned creating_ = 0;
    bool connected_ = false;
    bool published_ = false;
};

}  // namespace ce::vulkan_presentation
