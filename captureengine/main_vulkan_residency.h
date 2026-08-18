#pragma once

// Controller-side ownership of the CaptureEngine Vulkan implicit-layer
// registration. Split out of main_internal.h to keep that header inside the
// project's file-size ceiling.

#include <filesystem>

#include "../common/logging.h"
#include "../common/vulkan_layer_registration.h"

inline ce::vulkan_layer::RegistrationPlan BuildControllerVulkanRegistrationPlan() {
    std::filesystem::path baseDir;
    if (!ce::vulkan_layer::GetCurrentExecutableDirectory(&baseDir)) {
        LogError("[Controller] Failed to resolve executable directory for Vulkan layer registration");
        return {};
    }

    return ce::vulkan_layer::BuildRegistrationPlan(baseDir, ce::vulkan_layer::RegistrationMode::Auto,
                                                   ce::vulkan_layer::IsCurrentProcessElevated());
}

// The Vulkan implicit-layer registration is deliberately RESIDENT, not scoped to
// this process. The Vulkan loader composes a process's layer chain exactly once,
// inside vkCreateInstance, from the ImplicitLayers key as it reads at that
// moment. A registration torn down on controller exit therefore makes late
// injection structurally impossible for Vulkan: a title launched while
// CaptureEngine is not running never gets VK_LAYER_CE_overlay.dll into its
// chain, and no later injection can add one, because CE's whole Vulkan
// present/overlay path lives in that layer DLL rather than in the injected hook.
// Session logs/20260818_224257 (Strange Brigade Vulkan, CE started second) shows
// the consequence: no vulkan_layer*.log at all, vulkanLayerActive never set, and
// the hook falling through to the D3D path with no overlay.
//
// Residency is safe because the layer is inert without a host: it finds no
// discovery mapping, reports itself not whitelisted, stays in passthrough for
// every entry point, and logs nothing. When a host does appear, the injector
// signals the per-PID Vulkan reactivation event and the resident layer wakes up
// and late-initializes the overlay on its next present. This is also how every
// other resident Vulkan overlay on this system is registered (Steam, OBS, RTSS,
// EOS). `DISABLE_CE_VULKAN_LAYER=1` disables it per app, and
// `layer_register.exe --unregister` removes it entirely.
class VulkanLayerResidency {
public:
    VulkanLayerResidency() : plan_(BuildControllerVulkanRegistrationPlan()) {
        ce::vulkan_layer::LogRegistrationPlan(plan_);
        // Prune only superseded CE entries (previous install directories, wrong
        // registry view, manifests no longer on disk) while leaving this
        // instance's own entries in place, so the live registration is never
        // momentarily absent for a title starting right now.
        if (!ce::vulkan_layer::RepairOwnedRegistrations(plan_)) {
            LogWarn("[Controller] Vulkan layer registration repair was incomplete");
        }
        active_ = ce::vulkan_layer::ApplyRegistrationPlan(plan_, true);
        if (!active_) {
            LogError("[Controller] Vulkan layer registration failed");
        } else {
            LogInfo(
                "[Controller] Vulkan layer registration is resident; it survives this process so Vulkan titles "
                "started before CaptureEngine can still be injected late");
        }
    }

    bool IsActive() const {
        return active_;
    }

private:
    ce::vulkan_layer::RegistrationPlan plan_;
    bool active_ = false;
};
