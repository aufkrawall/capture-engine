#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace testapp::vkfg {

enum class FgMode : uint8_t {
    Off,
    Dlss,
    Fsr,
};

// Shared camera contract for the Vulkan shader and both SDK integrations. It intentionally
// matches dx12fg::SceneRenderer so depth, motion vectors, and visual composition describe the
// same scene to DLSS and FidelityFX rather than merely looking similar on the final surface.
struct SceneCameraPolicy {
    std::array<float, 3> position{};
    std::array<float, 3> right{};
    std::array<float, 3> up{};
    std::array<float, 3> forward{};
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    float verticalFov = 1.04719755f;
    float aspect = 1.0f;
};

struct SceneProjectionPolicy {
    std::array<float, 16> viewToClip{};
    std::array<float, 16> clipToView{};
};

inline SceneCameraPolicy BuildSceneCameraPolicy(float aspect) {
    SceneCameraPolicy camera{};
    camera.position = {0.0f, 2.4f, -5.5f};
    const std::array<float, 3> target = {0.0f, 0.7f, 1.4f};
    const float forwardX = target[0] - camera.position[0];
    const float forwardY = target[1] - camera.position[1];
    const float forwardZ = target[2] - camera.position[2];
    const float forwardLength = std::sqrt(forwardX * forwardX + forwardY * forwardY +
                                          forwardZ * forwardZ);
    camera.forward = {forwardX / forwardLength, forwardY / forwardLength,
                      forwardZ / forwardLength};
    // cross(worldUp, forward) is exactly +X for this centered camera.
    camera.right = {1.0f, 0.0f, 0.0f};
    camera.up = {0.0f, camera.forward[2], -camera.forward[1]};
    camera.aspect = aspect > 0.0f ? aspect : 1.0f;
    return camera;
}

inline SceneProjectionPolicy BuildSceneProjectionPolicy(const SceneCameraPolicy& camera) {
    SceneProjectionPolicy projection{};
    const float yScale = 1.0f / std::tan(camera.verticalFov * 0.5f);
    const float xScale = yScale / camera.aspect;
    const float zScale = camera.farPlane / (camera.farPlane - camera.nearPlane);
    const float zOffset = -camera.nearPlane * camera.farPlane /
                          (camera.farPlane - camera.nearPlane);
    projection.viewToClip = {xScale, 0.0f, 0.0f, 0.0f,
                             0.0f, yScale, 0.0f, 0.0f,
                             0.0f, 0.0f, zScale, 1.0f,
                             0.0f, 0.0f, zOffset, 0.0f};
    projection.clipToView = {1.0f / xScale, 0.0f, 0.0f, 0.0f,
                             0.0f, 1.0f / yScale, 0.0f, 0.0f,
                             0.0f, 0.0f, 0.0f, 1.0f / zOffset,
                             0.0f, 0.0f, 1.0f, -zScale / zOffset};
    return projection;
}

enum class SwapchainOwner : uint8_t {
    Native,
    Streamline,
    FidelityFX,
};

enum class VulkanWsiRoute : uint8_t {
    Loader,
    StreamlineProxy,
    FidelityFXReplacement,
};

inline const char* OwnerName(SwapchainOwner owner) {
    switch (owner) {
        case SwapchainOwner::Native:
            return "native";
        case SwapchainOwner::Streamline:
            return "streamline";
        case SwapchainOwner::FidelityFX:
            return "fidelityfx";
        default:
            return "unknown";
    }
}

inline const char* WsiRouteName(VulkanWsiRoute route) {
    switch (route) {
        case VulkanWsiRoute::Loader:
            return "loader";
        case VulkanWsiRoute::StreamlineProxy:
            return "streamline-proxy";
        case VulkanWsiRoute::FidelityFXReplacement:
            return "ffx-replacement";
        default:
            return "unknown";
    }
}

inline SwapchainOwner OwnerForMode(FgMode mode) {
    switch (mode) {
        case FgMode::Dlss:
            return SwapchainOwner::Streamline;
        case FgMode::Fsr:
            return SwapchainOwner::FidelityFX;
        case FgMode::Off:
        default:
            return SwapchainOwner::Native;
    }
}

inline VulkanWsiRoute ExpectedWsiRoute(SwapchainOwner owner) {
    switch (owner) {
        case SwapchainOwner::Streamline:
            return VulkanWsiRoute::StreamlineProxy;
        case SwapchainOwner::FidelityFX:
            return VulkanWsiRoute::FidelityFXReplacement;
        case SwapchainOwner::Native:
        default:
            return VulkanWsiRoute::Loader;
    }
}

inline bool IsOwnerDispatchPairValid(SwapchainOwner owner, VulkanWsiRoute route) {
    return ExpectedWsiRoute(owner) == route;
}

// FFX wrapper handles cannot be consumed by another WSI route, and FidelityFX must receive a
// loader-owned bridge rather than a Streamline proxy so the latter observes its own destroy hook.
// The FFX API replacement function also requires its current wrapper to be destroyed before the
// same context can create another one. Native and Streamline handles remain ordinary Vulkan
// handles and can participate directly in each other's standard oldSwapchain replacement path.
inline bool ShouldForwardOldSwapchain(SwapchainOwner currentOwner,
                                      SwapchainOwner replacementOwner) {
    return currentOwner != SwapchainOwner::FidelityFX &&
           replacementOwner != SwapchainOwner::FidelityFX;
}

struct VulkanFsrVersionResolution {
    int requested = 0;
    int resolved = 3;
    bool mlFallback = false;
    bool invalidRequest = false;
};

// FidelityFX SDK 1.1.4 exposes the non-ML FSR 3.1.4 Vulkan provider. FSR 4 ML SR/FG has no
// Vulkan backend, so a request for 4 is explicit fallback rather than loss of FSR support.
inline VulkanFsrVersionResolution ResolveVulkanFsrVersion(int requested) {
    VulkanFsrVersionResolution result{};
    result.requested = requested;
    result.mlFallback = requested == 4;
    result.invalidRequest = requested != 0 && requested != 3 && requested != 4;
    return result;
}

enum class PresentMode : uint8_t {
    Immediate,
    Mailbox,
    Fifo,
    FifoRelaxed,
};

enum class FgResourceRole : uint8_t {
    SceneColor,
    MotionVectors,
    Depth,
    Mask,
    HudlessColor,
    HistoryColor,
    UiColor,
    ComposedColor,
    Presentation,
};

struct FgResourceMetadata {
    uint8_t channels = 0;
    uint8_t bitsPerChannel = 0;
    bool floatingPoint = false;
    bool depth = false;
    bool fullResolution = false;
};

inline FgResourceMetadata DescribeFgResource(FgResourceRole role) {
    switch (role) {
        case FgResourceRole::SceneColor:
            return {4, 16, true, false, false};
        case FgResourceRole::MotionVectors:
            return {2, 16, true, false, false};
        case FgResourceRole::Depth:
            return {1, 32, true, true, false};
        case FgResourceRole::Mask:
            return {1, 8, false, false, false};
        case FgResourceRole::HudlessColor:
        case FgResourceRole::HistoryColor:
        case FgResourceRole::ComposedColor:
            return {4, 16, true, false, true};
        case FgResourceRole::UiColor:
        case FgResourceRole::Presentation:
            return {4, 8, false, false, true};
        default:
            return {};
    }
}

enum class VulkanImageLayoutClass : uint8_t {
    General,
    Present,
    TransferSource,
    TransferDestination,
    ColorAttachment,
    ShaderRead,
};

struct FfxResourceStateBits {
    uint32_t present = 0;
    uint32_t copySource = 0;
    uint32_t copyDestination = 0;
    uint32_t renderTarget = 0;
    uint32_t unorderedAccess = 0;
    uint32_t pixelComputeRead = 0;
};

inline VulkanImageLayoutClass ResolveFfxResourceLayout(uint32_t state,
                                                       const FfxResourceStateBits& bits) {
    if ((state & bits.present) != 0) {
        return VulkanImageLayoutClass::Present;
    }
    if ((state & bits.copySource) != 0) {
        return VulkanImageLayoutClass::TransferSource;
    }
    if ((state & bits.copyDestination) != 0) {
        return VulkanImageLayoutClass::TransferDestination;
    }
    if ((state & bits.renderTarget) != 0) {
        return VulkanImageLayoutClass::ColorAttachment;
    }
    if ((state & bits.unorderedAccess) != 0) {
        return VulkanImageLayoutClass::General;
    }
    if ((state & bits.pixelComputeRead) != 0) {
        return VulkanImageLayoutClass::ShaderRead;
    }
    return VulkanImageLayoutClass::General;
}

// A failed transition may preserve a genuinely newer request, but the request which caused the
// failure must be consumed. Otherwise rollback immediately launches the same failing preparation
// again on every frame.
inline FgMode ResolveRequestedModeAfterTransitionFailure(FgMode requestedMode,
                                                         FgMode failedTarget,
                                                         FgMode currentMode) {
    return requestedMode == failedTarget ? currentMode : requestedMode;
}

inline PresentMode SelectPresentMode(bool vsync, const std::vector<PresentMode>& available) {
    auto has = [&available](PresentMode mode) {
        return std::find(available.begin(), available.end(), mode) != available.end();
    };
    if (vsync) {
        return has(PresentMode::Fifo) ? PresentMode::Fifo
                                     : (available.empty() ? PresentMode::Fifo : available.front());
    }
    if (has(PresentMode::Immediate)) {
        return PresentMode::Immediate;
    }
    if (has(PresentMode::Mailbox)) {
        return PresentMode::Mailbox;
    }
    if (has(PresentMode::FifoRelaxed)) {
        return PresentMode::FifoRelaxed;
    }
    return PresentMode::Fifo;
}

struct VulkanQueueFamilyCaps {
    uint32_t familyIndex = 0;
    uint32_t queueCount = 0;
    bool graphics = false;
    bool compute = false;
    bool transfer = false;
    bool present = false;
    bool opticalFlow = false;
};

struct VulkanQueueRef {
    uint32_t familyIndex = UINT32_MAX;
    uint32_t queueIndex = UINT32_MAX;

    bool Valid() const {
        return familyIndex != UINT32_MAX && queueIndex != UINT32_MAX;
    }

    bool operator==(const VulkanQueueRef& other) const {
        return familyIndex == other.familyIndex && queueIndex == other.queueIndex;
    }
};

struct VulkanQueueRequirements {
    bool requestFidelityFX = true;
    bool requestAsyncPresent = false;
    uint32_t streamlineGraphicsQueues = 0;
    uint32_t streamlineComputeQueues = 0;
    uint32_t streamlineOpticalFlowQueues = 0;
};

struct VulkanQueuePlan {
    VulkanQueueRef game;
    VulkanQueueRef asyncPresent;
    VulkanQueueRef ffxAsyncCompute;
    VulkanQueueRef ffxPresent;
    VulkanQueueRef ffxImageAcquire;
    std::vector<VulkanQueueRef> streamlineGraphics;
    std::vector<VulkanQueueRef> streamlineCompute;
    std::vector<VulkanQueueRef> streamlineOpticalFlow;
    std::vector<uint32_t> requestedQueueCounts;
    bool baseAvailable = false;
    bool asyncPresentAvailable = false;
    bool fidelityFxAvailable = false;
    bool streamlineAvailable = false;
};

namespace detail {

struct QueueAllocator {
    explicit QueueAllocator(const std::vector<VulkanQueueFamilyCaps>& capsIn) : caps(capsIn), used(capsIn.size()) {
        for (size_t i = 0; i < caps.size(); ++i) {
            used[i].assign(caps[i].queueCount, false);
        }
    }

    template <typename Predicate>
    VulkanQueueRef Take(Predicate predicate, bool preferDedicated = false) {
        for (int pass = 0; pass < (preferDedicated ? 2 : 1); ++pass) {
            for (size_t family = 0; family < caps.size(); ++family) {
                if (!predicate(caps[family])) {
                    continue;
                }
                if (preferDedicated && pass == 0 && caps[family].graphics) {
                    continue;
                }
                for (uint32_t index = 0; index < caps[family].queueCount; ++index) {
                    if (!used[family][index]) {
                        used[family][index] = true;
                        return {caps[family].familyIndex, index};
                    }
                }
            }
        }
        return {};
    }

    std::vector<VulkanQueueFamilyCaps> caps;
    std::vector<std::vector<bool>> used;
};

inline void UpdateRequestedQueueCounts(const std::vector<VulkanQueueFamilyCaps>& caps, const VulkanQueueRef& ref,
                                       std::vector<uint32_t>* counts) {
    if (!ref.Valid()) {
        return;
    }
    for (size_t i = 0; i < caps.size(); ++i) {
        if (caps[i].familyIndex == ref.familyIndex) {
            (*counts)[i] = std::max((*counts)[i], ref.queueIndex + 1);
            return;
        }
    }
}

}  // namespace detail

inline VulkanQueuePlan BuildVulkanQueuePlan(const std::vector<VulkanQueueFamilyCaps>& caps,
                                            const VulkanQueueRequirements& requirements) {
    VulkanQueuePlan plan{};
    plan.requestedQueueCounts.assign(caps.size(), 0);
    detail::QueueAllocator allocator(caps);
    plan.game = allocator.Take([](const VulkanQueueFamilyCaps& family) {
        return family.graphics && family.present;
    });
    plan.baseAvailable = plan.game.Valid();
    if (!plan.baseAvailable) {
        return plan;
    }

    auto takeMany = [](detail::QueueAllocator* source, uint32_t count, auto predicate,
                       bool preferDedicated, std::vector<VulkanQueueRef>* output) {
        for (uint32_t i = 0; i < count; ++i) {
            VulkanQueueRef ref = source->Take(predicate, preferDedicated);
            if (!ref.Valid()) {
                output->clear();
                return false;
            }
            output->push_back(ref);
        }
        return true;
    };

    // Reserve Streamline's complete queue set atomically. Failed partial reservations must not
    // consume queues that can keep FidelityFX FG available.
    detail::QueueAllocator streamlineAllocator = allocator;
    const bool slGraphics = takeMany(
        &streamlineAllocator, requirements.streamlineGraphicsQueues,
        [](const VulkanQueueFamilyCaps& family) { return family.graphics; }, false, &plan.streamlineGraphics);
    const bool slCompute = takeMany(
        &streamlineAllocator, requirements.streamlineComputeQueues,
        [](const VulkanQueueFamilyCaps& family) { return family.compute; }, true, &plan.streamlineCompute);
    const bool slOptical = takeMany(
        &streamlineAllocator, requirements.streamlineOpticalFlowQueues,
        [](const VulkanQueueFamilyCaps& family) { return family.opticalFlow; }, false,
        &plan.streamlineOpticalFlow);
    plan.streamlineAvailable = slGraphics && slCompute && slOptical;
    if (plan.streamlineAvailable) {
        allocator = std::move(streamlineAllocator);
    } else {
        plan.streamlineGraphics.clear();
        plan.streamlineCompute.clear();
        plan.streamlineOpticalFlow.clear();
    }

    if (requirements.requestFidelityFX) {
        plan.ffxAsyncCompute = allocator.Take(
            [](const VulkanQueueFamilyCaps& family) { return family.compute; }, true);
        plan.ffxPresent = allocator.Take([](const VulkanQueueFamilyCaps& family) {
            return family.transfer && family.present;
        });
        plan.ffxImageAcquire = allocator.Take([](const VulkanQueueFamilyCaps&) { return true; });
        plan.fidelityFxAvailable =
            plan.ffxAsyncCompute.Valid() && plan.ffxPresent.Valid() && plan.ffxImageAcquire.Valid();
        if (!plan.fidelityFxAvailable) {
            plan.ffxAsyncCompute = {};
            plan.ffxPresent = {};
            plan.ffxImageAcquire = {};
        }
    }

    if (requirements.requestAsyncPresent) {
        // Keep the application-owned present queue in the game queue family. This gives Native,
        // Streamline, and FidelityFX one valid queue-separation stress path without swapchain
        // ownership transfers. FidelityFX also requires the queue on which its replacement
        // vkQueuePresentKHR is invoked to have both graphics and compute capability.
        plan.asyncPresent = allocator.Take([gameFamily = plan.game.familyIndex](
                                               const VulkanQueueFamilyCaps& family) {
            return family.familyIndex == gameFamily && family.graphics && family.compute &&
                   family.present;
        });
        plan.asyncPresentAvailable = plan.asyncPresent.Valid();
    }

    detail::UpdateRequestedQueueCounts(caps, plan.game, &plan.requestedQueueCounts);
    detail::UpdateRequestedQueueCounts(caps, plan.asyncPresent, &plan.requestedQueueCounts);
    detail::UpdateRequestedQueueCounts(caps, plan.ffxAsyncCompute, &plan.requestedQueueCounts);
    detail::UpdateRequestedQueueCounts(caps, plan.ffxPresent, &plan.requestedQueueCounts);
    detail::UpdateRequestedQueueCounts(caps, plan.ffxImageAcquire, &plan.requestedQueueCounts);
    for (const VulkanQueueRef& ref : plan.streamlineGraphics) {
        detail::UpdateRequestedQueueCounts(caps, ref, &plan.requestedQueueCounts);
    }
    for (const VulkanQueueRef& ref : plan.streamlineCompute) {
        detail::UpdateRequestedQueueCounts(caps, ref, &plan.requestedQueueCounts);
    }
    for (const VulkanQueueRef& ref : plan.streamlineOpticalFlow) {
        detail::UpdateRequestedQueueCounts(caps, ref, &plan.requestedQueueCounts);
    }
    return plan;
}

enum class TransitionStage : uint8_t {
    Idle,
    OldPassthroughPending,
    PreparingReplacement,
    ReplacingSwapchain,
    ReplacementPresentPending,
    Activating,
    Rollback,
    DeviceLost,
};

struct FgTransitionState {
    uint64_t epoch = 0;
    FgMode currentMode = FgMode::Off;
    FgMode targetMode = FgMode::Off;
    SwapchainOwner owner = SwapchainOwner::Native;
    SwapchainOwner oldOwner = SwapchainOwner::Native;
    SwapchainOwner targetOwner = SwapchainOwner::Native;
    TransitionStage stage = TransitionStage::Idle;
    bool suspended = false;
    bool oldFgDisabled = true;
    bool replacementCommitted = false;
};

inline bool BeginModeTransition(FgTransitionState* state, FgMode target) {
    if (!state || state->stage == TransitionStage::DeviceLost) {
        return false;
    }
    if (state->stage != TransitionStage::Idle) {
        return false;
    }
    if (state->currentMode == target) {
        if (target != FgMode::Off) {
            state->suspended = !state->suspended;
        }
        return false;
    }
    ++state->epoch;
    state->targetMode = target;
    state->oldOwner = state->owner;
    state->targetOwner = OwnerForMode(target);
    state->stage = TransitionStage::OldPassthroughPending;
    state->oldFgDisabled = state->currentMode == FgMode::Off || state->suspended;
    state->replacementCommitted = false;
    state->suspended = false;
    return true;
}

inline void MarkOldFgDisabled(FgTransitionState* state) {
    if (state && state->stage == TransitionStage::OldPassthroughPending) {
        state->oldFgDisabled = true;
    }
}

inline bool MarkOldPassthroughPresented(FgTransitionState* state, bool success) {
    if (!state || state->stage != TransitionStage::OldPassthroughPending || !state->oldFgDisabled || !success) {
        return false;
    }
    state->stage = TransitionStage::PreparingReplacement;
    return true;
}

inline bool MarkReplacementPrepared(FgTransitionState* state, bool success) {
    if (!state || state->stage != TransitionStage::PreparingReplacement) {
        return false;
    }
    state->stage = success ? TransitionStage::ReplacingSwapchain : TransitionStage::Rollback;
    return success;
}

inline bool MarkReplacementCreated(FgTransitionState* state, bool success) {
    if (!state || state->stage != TransitionStage::ReplacingSwapchain) {
        return false;
    }
    if (!success) {
        state->stage = TransitionStage::Rollback;
        return false;
    }
    state->owner = state->targetOwner;
    state->replacementCommitted = true;
    state->stage = TransitionStage::ReplacementPresentPending;
    return true;
}

inline bool MarkReplacementPresented(FgTransitionState* state, bool success) {
    if (!state || state->stage != TransitionStage::ReplacementPresentPending || !success) {
        return false;
    }
    state->stage = TransitionStage::Activating;
    return true;
}

inline bool MarkSameOwnerReplacementCommitted(FgTransitionState* state) {
    if (!state || state->stage != TransitionStage::Idle || state->currentMode == FgMode::Off) {
        return false;
    }
    state->oldOwner = state->owner;
    state->targetOwner = state->owner;
    state->targetMode = state->currentMode;
    state->oldFgDisabled = true;
    state->replacementCommitted = true;
    state->suspended = false;
    state->stage = TransitionStage::ReplacementPresentPending;
    return true;
}

inline bool MarkTargetActivated(FgTransitionState* state, bool success) {
    if (!state || state->stage != TransitionStage::Activating) {
        return false;
    }
    if (!success) {
        // The new surface is already visible. Keep it and remain FG-off instead of destroying the
        // presentation surface a second time; a later request can retry transactionally.
        state->currentMode = FgMode::Off;
        state->targetMode = FgMode::Off;
        state->stage = TransitionStage::Idle;
        return false;
    }
    state->currentMode = state->targetMode;
    state->stage = TransitionStage::Idle;
    return true;
}

inline bool RollbackPreparedTransition(FgTransitionState* state) {
    if (!state || state->stage != TransitionStage::Rollback || state->replacementCommitted) {
        return false;
    }
    state->owner = state->oldOwner;
    state->targetMode = state->currentMode;
    state->targetOwner = state->owner;
    state->stage = TransitionStage::Idle;
    return true;
}

inline bool CancelModeTransitionBeforeReplacement(FgTransitionState* state) {
    if (!state || state->replacementCommitted ||
        (state->stage != TransitionStage::OldPassthroughPending &&
         state->stage != TransitionStage::PreparingReplacement)) {
        return false;
    }
    state->targetMode = state->currentMode;
    state->targetOwner = state->oldOwner;
    state->owner = state->oldOwner;
    state->stage = TransitionStage::Idle;
    state->oldFgDisabled = false;
    state->suspended = false;
    return true;
}

inline void MarkDeviceLost(FgTransitionState* state) {
    if (state) {
        state->stage = TransitionStage::DeviceLost;
    }
}

}  // namespace testapp::vkfg
