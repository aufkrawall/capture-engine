#include "ffx_hook_internal.h"
#include "../../common/log_meter.h"

// Fallback for contexts whose ffxCreateContext CE never observed (see ClassifyUnobservedContextFromConfigure).
// Without it their destroy logs "Non-FG Context destroyed": the FG context count never reaches zero, and neither
// the all-contexts-destroyed teardown nor the protected-startup retirement for a destroyed swapchain context runs.
// The create-time entry breakpoint is the primary route; this keeps the destroy side correct if it misses one.

namespace {

bool IsServedByVulkanRuntime(void* runtimeExport) {
    HMODULE owner = nullptr;
    if (!runtimeExport ||
        !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(runtimeExport), &owner) ||
        !owner) {
        return false;
    }
    char path[MAX_PATH] = {};
    return GetModuleFileNameA(owner, path, sizeof(path)) != 0 && ce::ffx_api::IsVulkanFFXRuntimeModuleName(path);
}

}  // namespace

void AdoptUnobservedFFXContextFromConfigure(ffxContext contextHandle, ffxStructType_t configureType,
                                            void* runtimeExport) {
    if (!contextHandle || !ce::ffx_api::IsFrameGenerationEffectType(configureType)) {
        return;
    }
    // Only the generic frame-generation effect needs the module to name its backend.
    const bool vulkanRuntime = ce::ffx_api::GetEffectId(configureType) == ce::ffx_api::kEffectIdFrameGeneration &&
                               IsServedByVulkanRuntime(runtimeExport);
    const auto adoption = ce::ffx_api::ClassifyUnobservedContextFromConfigure(configureType, vulkanRuntime);
    if (!adoption.adopt) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(ffx_hook_g_ContextMapMutex);
        if (!ffx_hook_g_ContextTypeMap.emplace(contextHandle, adoption.effectId).second) {
            return;  // a create or an earlier configure already tracks it
        }
        if (adoption.vulkan) {
            ffx_hook_g_VulkanContextSet.insert(contextHandle);
        }
    }

    // Mirrors Hooked_ffxCreateContext: only DX12 FG contexts feed the live count the destroy teardown keys on.
    const bool countsTowardLiveContexts =
        !adoption.vulkan && (adoption.effectId == ffx_hook_FFX_API_EFFECT_ID_FRAMEGENERATION ||
                             adoption.effectId == ffx_hook_FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN);
    const int liveContexts = countsTowardLiveContexts
                                 ? ffx_hook_g_FGContextCount.fetch_add(1, std::memory_order_acq_rel) + 1
                                 : ffx_hook_g_FGContextCount.load(std::memory_order_acquire);

    static std::atomic<uint32_t> s_adoptedCount{0};
    const uint32_t adopted = s_adoptedCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ce::log_meter::ShouldLogCadence(adopted, 20, 100)) {
        HookLogImportant(
            "FFX Hook: Adopted %s frame-generation context %p at its first ffxConfigure (type=0x%llx effectId=0x%x "
            "liveContexts=%d adopted=%u) - its ffxCreateContext was not observed; destroy-time teardown now runs",
            adoption.vulkan ? "Vulkan" : "DX12", contextHandle, static_cast<unsigned long long>(configureType),
            adoption.effectId, liveContexts, adopted);
    }
}
