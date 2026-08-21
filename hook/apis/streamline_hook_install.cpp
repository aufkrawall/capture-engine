#include "streamline_hook_internal.h"

#include "../common/streamline_api_generation.h"
#include "streamline_bridge.h"
#include "streamline_bridge_policy.h"
#include "streamline_hook_v1.h"
#include "streamline_v1_feature_probe.h"

namespace {

using ce::streamline_api::Generation;

std::atomic<int> g_streamlineApiGeneration{static_cast<int>(Generation::Unknown)};
std::atomic<bool> g_abiSensitiveDynamicHooksRegistered{false};

bool ExportsAnyOf(HMODULE module, const char* const* names, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (GetProcAddress(module, names[i]) != nullptr) {
            return true;
        }
    }
    return false;
}

// Which Streamline generation THIS module speaks. Deliberately not cached across modules.
//
// A process normally runs exactly one Streamline distribution, so a single latched answer
// used to be enough. `streamline_upgrade` breaks that assumption on purpose: the bridge
// leaves the game's 1.x interposer resident and adds a CE-owned 2.x runtime, so both
// generations are loaded at once. A process-wide latch would then let whichever module CE
// happened to see first decide the ABI for both - and installing the 2.x-shaped hooks on a
// 1.x module is exactly the argument truncation that killed The Witcher 3
// (`20260820_221409`). Only a module that actually exports the ABI-sensitive entry points
// can answer for itself: sl.common exports neither, so it never votes.
Generation ClassifyModuleGeneration(HMODULE module) {
    if (!module || (GetProcAddress(module, "slSetTag") == nullptr &&
                    GetProcAddress(module, "slEvaluateFeature") == nullptr)) {
        return Generation::Unknown;
    }
    const bool sawV2 =
        ExportsAnyOf(module, ce::streamline_api::kV2OnlyExports, ce::streamline_api::kV2OnlyExportCount);
    const bool sawV1 =
        ExportsAnyOf(module, ce::streamline_api::kV1OnlyExports, ce::streamline_api::kV1OnlyExportCount);
    return ce::streamline_api::Classify(sawV2, sawV1);
}

// The per-module answer, logged once per generation so a bridged process shows both.
Generation ResolveStreamlineGeneration(HMODULE module, const char* moduleBaseName) {
    const Generation generation = ClassifyModuleGeneration(module);
    if (generation == Generation::Unknown && module == nullptr) {
        return generation;
    }

    static std::atomic<uint32_t> loggedGenerations{0};
    const uint32_t bit = 1u << static_cast<uint32_t>(generation);
    if ((loggedGenerations.fetch_or(bit, std::memory_order_relaxed) & bit) == 0) {
        HookLogImportant(
            "Streamline Hook: %s speaks %s - CE installs only the hooks whose signatures match that module's "
            "generation",
            moduleBaseName ? moduleBaseName : "the Streamline module", ce::streamline_api::Describe(generation));
        if (generation == Generation::Unknown) {
            HookLogImportant(
                "Streamline Hook: slSetTag/slEvaluateFeature stay unhooked because their calling convention "
                "cannot be established - CE never guesses a foreign ABI, and a guess here truncates the "
                "caller's command list pointer");
        }
    }
    return generation;
}

// The generation every PROCESS-WIDE decision uses. Today that is the single
// GetProcAddress-time route per ABI-sensitive symbol, which cannot be per-module because it
// is keyed on the symbol name alone.
//
// With the bridge active the game's Streamline calls all reach CE's thunks and then the 2.x
// runtime, so that runtime is authoritative even though the 1.x interposer is still resident
// and was almost certainly classified first.
Generation AuthoritativeStreamlineGeneration(Generation moduleGeneration) {
    int expected = static_cast<int>(Generation::Unknown);
    g_streamlineApiGeneration.compare_exchange_strong(expected, static_cast<int>(moduleGeneration),
                                                      std::memory_order_acq_rel);
    const auto firstSeen = static_cast<Generation>(g_streamlineApiGeneration.load(std::memory_order_acquire));
    return ce::streamline_bridge::AuthoritativeProcessGeneration(ce::streamline_bridge::IsActive(), firstSeen);
}

// The dynamic (GetProcAddress-time) routes for the two ABI-sensitive exports can only be
// registered once the generation is known, because the detour differs per generation.
void RegisterAbiSensitiveDynamicHooksOnce(Generation generation) {
    if (generation == Generation::Unknown) {
        return;
    }
    if (g_abiSensitiveDynamicHooksRegistered.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    void* setTagDetour = generation == Generation::V1
                             ? reinterpret_cast<void*>(&ce::streamline_v1::Hooked_slSetTagV1)
                             : reinterpret_cast<void*>(Hooked_slSetTag);
    void* evaluateDetour = generation == Generation::V1
                               ? reinterpret_cast<void*>(&ce::streamline_v1::Hooked_slEvaluateFeatureV1)
                               : reinterpret_cast<void*>(Hooked_slEvaluateFeature);
    IATHook::RegisterDynamicHookFiltered("slSetTag", setTagDetour,
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slSetTag),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slEvaluateFeature", evaluateDetour,
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slEvaluateFeature),
                                         IsStreamlineCoreDynamicHookModule);
    HookLogImportant("Streamline Hook: registered the %s slSetTag/slEvaluateFeature dynamic routes",
                     ce::streamline_api::Describe(generation));
}

}  // namespace


void RegisterDynamicHooksOnce() {


    if (streamline_hook_g_DynamicHooksRegistered.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    IATHook::RegisterDynamicHookFiltered("slGetFeatureFunction", reinterpret_cast<void*>(Hooked_slGetFeatureFunction),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slGetFeatureFunction),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slGetPluginFunction", reinterpret_cast<void*>(Hooked_slGetPluginFunction),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slGetPluginFunction),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slSetD3DDevice", reinterpret_cast<void*>(Hooked_slSetD3DDevice),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slSetD3DDevice),
                                         IsStreamlineCoreDynamicHookModule);
    // slSetTag and slEvaluateFeature are deliberately absent here: their signatures differ
    // between Streamline generations, so they are registered by
    // RegisterAbiSensitiveDynamicHooksOnce as soon as the generation is known.
    // slSetTagForFrame exists only in 2.x, so its name can never resolve on a 1.x process.
    IATHook::RegisterDynamicHookFiltered("slSetTagForFrame", reinterpret_cast<void*>(Hooked_slSetTagForFrame),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slSetTagForFrame),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slDLSSGSetOptions", reinterpret_cast<void*>(Hooked_slDLSSGSetOptions),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slDLSSGSetOptions),
                                         IsStreamlineDLSSGDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slDLSSGGetState", reinterpret_cast<void*>(Hooked_slDLSSGGetState),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slDLSSGGetState),
                                         IsStreamlineDLSSGDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slReflexSleep", reinterpret_cast<void*>(Hooked_slReflexSleep),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSleep),
                                         IsStreamlineReflexDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slReflexSetOptions", reinterpret_cast<void*>(Hooked_slReflexSetOptions),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSetOptions),
                                         IsStreamlineReflexDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slReflexSetConstants", reinterpret_cast<void*>(Hooked_slReflexSetConstants),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSetConstants),
                                         IsStreamlineReflexDynamicHookModule);
    HookLogImportant(
        "Streamline Hook: Registered module-filtered dynamic hooks for core Streamline exports and owned feature "
        "exports");

}


bool InstallHooksForModule(HMODULE module,  const char* moduleNameOrPath) {


    if (!module || !IsStreamlineModuleName(moduleNameOrPath)) {
        return false;
    }

    g_FGCompat.SetStreamlineSupportPresent(true);

    RegisterDynamicHooksOnce();

    const char* moduleBaseName = GetModuleBaseName(moduleNameOrPath);
    // Establish the ABI before anything is patched. slSetTag and slEvaluateFeature are the
    // only exports whose signature changed between Streamline generations, and installing
    // the wrong one truncates the caller's arguments on the way back into Streamline.
    const Generation generation = ResolveStreamlineGeneration(module, moduleBaseName);
    // The dynamic route is keyed on the symbol name alone, so it takes the process-wide
    // answer; the inline/IAT hooks below are per module and must take this module's own.
    RegisterAbiSensitiveDynamicHooksOnce(AuthoritativeStreamlineGeneration(generation));

    // A module the generation bridge has routed around gets no hooks at all. Its exports are
    // unreachable - every import slot that led to them now leads to CE - so hooking it only
    // occupies CE's single forward pointer per symbol and makes the hook on the runtime that
    // IS being called get refused as a duplicate. See StreamlineModuleSupersededByBridge.
    if (ce::streamline_bridge::StreamlineModuleSupersededByBridge(ce::streamline_bridge::IsActive(), generation)) {
        static std::atomic<uint32_t> loggedModules{0};
        const uint32_t bit = GetModuleMaskBit(moduleBaseName);
        if (bit != 0 && (loggedModules.fetch_or(bit, std::memory_order_relaxed) & bit) == 0) {
            HookLogImportant(
                "Streamline Hook: leaving %s unhooked - the generation bridge routed every call away from this "
                "1.x module, and hooking it would take CE's forward pointer away from the 2.x runtime that is "
                "actually being called",
                moduleBaseName ? moduleBaseName : "a Streamline 1.x module");
        }
        return false;
    }
    const bool mayHookV2Abi = ce::streamline_api::MayInstallAbiSensitiveHook(generation, Generation::V2);
    const bool mayHookV1Abi = ce::streamline_api::MayInstallAbiSensitiveHook(generation, Generation::V1);
    const bool mayHookAbiSensitive = mayHookV1Abi || mayHookV2Abi;
    const bool shouldHookCoreExports = ShouldHookStreamlineCoreExports(moduleBaseName);
    const uint32_t moduleBit = GetModuleMaskBit(moduleBaseName);
    const auto originalGetFeatureFunction =
        reinterpret_cast<PFN_slGetFeatureFunction>(GetProcAddress(module, "slGetFeatureFunction"));
    const auto originalGetPluginFunction =
        reinterpret_cast<PFN_slGetPluginFunction>(GetProcAddress(module, "slGetPluginFunction"));
    const auto originalSetD3DDevice = reinterpret_cast<PFN_slSetD3DDevice>(GetProcAddress(module, "slSetD3DDevice"));
    const auto originalSetTag = reinterpret_cast<PFN_slSetTag>(GetProcAddress(module, "slSetTag"));
    const auto originalSetTagForFrame =
        reinterpret_cast<PFN_slSetTagForFrame>(GetProcAddress(module, "slSetTagForFrame"));
    const auto originalEvaluateFeature =
        reinterpret_cast<PFN_slEvaluateFeature>(GetProcAddress(module, "slEvaluateFeature"));
    const auto originalDLSSGSetOptions =
        reinterpret_cast<PFN_slDLSSGSetOptions>(GetProcAddress(module, "slDLSSGSetOptions"));
    const auto originalDLSSGGetState = reinterpret_cast<PFN_slDLSSGGetState>(GetProcAddress(module, "slDLSSGGetState"));
    const auto originalReflexSleep = reinterpret_cast<PFN_slReflexSleep>(GetProcAddress(module, "slReflexSleep"));
    const auto originalReflexSetOptions =
        reinterpret_cast<PFN_slReflexSetOptions>(GetProcAddress(module, "slReflexSetOptions"));
    const auto originalReflexSetConstants =
        reinterpret_cast<PFN_slReflexSetConstants>(GetProcAddress(module, "slReflexSetConstants"));

    if (!originalGetFeatureFunction && !originalGetPluginFunction && !originalSetD3DDevice && !originalSetTag &&
        !originalSetTagForFrame && !originalEvaluateFeature && !originalDLSSGSetOptions && !originalDLSSGGetState &&
        !originalReflexSleep && !originalReflexSetOptions && !originalReflexSetConstants) {
        return false;
    }

    if (moduleBit != 0 && (streamline_hook_g_InstalledModuleMask.load(std::memory_order_acquire) & moduleBit) != 0) {
        // Self-heal for unload/reload generations when the unload notification
        // was unavailable: the mask claims this core module is hooked, but the
        // stored core targets must belong to the ARRIVING instance. If none
        // do, the mask refers to a previous unloaded generation (whose address
        // range may since have been re-mapped by a different module —
        // 20260612_003407 crash) and the fresh instance must be re-hooked.
        const auto targetWithinModule = [](std::atomic<void*>& targetSlot, HMODULE candidate) {
            if (!candidate) {
                return false;
            }
            void* target = targetSlot.load(std::memory_order_acquire);
            return ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(
                target, nullptr, reinterpret_cast<const void*>(candidate), GetModuleImageSizeBytes(candidate));
        };
        const bool anyCoreHookTargetWithinModule = targetWithinModule(streamline_hook_g_SLGetFeatureFunctionTarget, module) ||
                                                   targetWithinModule(streamline_hook_g_SLGetPluginFunctionTarget, module) ||
                                                   targetWithinModule(streamline_hook_g_SLSetD3DDeviceTarget, module) ||
                                                   targetWithinModule(streamline_hook_g_SLSetTagTarget, module) ||
                                                   targetWithinModule(streamline_hook_g_SLSetTagForFrameTarget, module) ||
                                                   targetWithinModule(streamline_hook_g_SLEvaluateFeatureTarget, module);
        if (!ce::streamline_runtime_policy::IsInstalledStreamlineModuleMaskStaleForReloadedModule(
                true, anyCoreHookTargetWithinModule)) {
            return false;
        }

        // Clear only the core slots that no longer belong to ANY live core
        // module instance; a still-loaded sibling core module's valid slots
        // must survive this self-heal.
        const HMODULE liveInterposer = GetModuleHandleA("sl.interposer.dll");
        const HMODULE liveCommon = GetModuleHandleA("sl.common.dll");
        struct CoreSlotView {
            const char* name;
            std::atomic<void*>* target;
            std::atomic<bool>* installed;
            void* volatile* original;
        };
        CoreSlotView coreSlots[] = {
            {"slGetFeatureFunction", &streamline_hook_g_SLGetFeatureFunctionTarget, &streamline_hook_g_SLGetFeatureFunctionHooked,
             reinterpret_cast<void* volatile*>(&streamline_hook_g_Original_slGetFeatureFunction)},
            {"slGetPluginFunction", &streamline_hook_g_SLGetPluginFunctionTarget, &streamline_hook_g_SLGetPluginFunctionHooked,
             reinterpret_cast<void* volatile*>(&streamline_hook_g_Original_slGetPluginFunction)},
            {"slSetD3DDevice", &streamline_hook_g_SLSetD3DDeviceTarget, &streamline_hook_g_SLSetD3DDeviceHooked,
             reinterpret_cast<void* volatile*>(&streamline_hook_g_Original_slSetD3DDevice)},
            {"slSetTag", &streamline_hook_g_SLSetTagTarget, &streamline_hook_g_SLSetTagHooked, reinterpret_cast<void* volatile*>(&streamline_hook_g_Original_slSetTag)},
            {"slSetTagForFrame", &streamline_hook_g_SLSetTagForFrameTarget, &streamline_hook_g_SLSetTagForFrameHooked,
             reinterpret_cast<void* volatile*>(&streamline_hook_g_Original_slSetTagForFrame)},
            {"slEvaluateFeature", &streamline_hook_g_SLEvaluateFeatureTarget, &streamline_hook_g_SLEvaluateFeatureHooked,
             reinterpret_cast<void* volatile*>(&streamline_hook_g_Original_slEvaluateFeature)},
        };
        int healedSlots = 0;
        for (CoreSlotView& slot : coreSlots) {
            void* target = slot.target->load(std::memory_order_acquire);
            if (!target || targetWithinModule(*slot.target, module) ||
                targetWithinModule(*slot.target, liveInterposer) || targetWithinModule(*slot.target, liveCommon)) {
                continue;
            }
            InterlockedExchangePointer(slot.original, nullptr);
            slot.target->store(nullptr, std::memory_order_release);
            slot.installed->store(false, std::memory_order_release);
            ++healedSlots;
        }
        HookLogImportant(
            "Streamline Hook: %s reloaded at %p but the installed-module mask refers to a previous unloaded "
            "generation — cleared %d stale core slot(s) and re-hooking the fresh instance (liveInterposer=%p "
            "liveCommon=%p)",
            moduleBaseName, module, healedSlots, liveInterposer, liveCommon);
        streamline_hook_g_InstalledModuleMask.fetch_and(~moduleBit, std::memory_order_acq_rel);
        streamline_hook_g_IATPatchesMask.fetch_and(~moduleBit, std::memory_order_acq_rel);
    }

    if (!shouldHookCoreExports && (originalGetFeatureFunction || originalGetPluginFunction || originalSetD3DDevice)) {
        LogSkippedStreamlineCoreExportsOnce(moduleBaseName, module, originalGetFeatureFunction != nullptr,
                                            originalGetPluginFunction != nullptr, originalSetD3DDevice != nullptr);
    }

    bool hookedAnything = false;
    {
        std::lock_guard<std::mutex> lock(streamline_hook_g_ModuleHookMutex);

        if (shouldHookCoreExports && originalGetFeatureFunction) {
            if (!streamline_hook_g_Original_slGetFeatureFunction) {
                streamline_hook_g_Original_slGetFeatureFunction = originalGetFeatureFunction;
            }

            hookedAnything |= InstallInlineHookOnce(reinterpret_cast<void*>(originalGetFeatureFunction),
                                                    reinterpret_cast<void*>(Hooked_slGetFeatureFunction),
                                                    streamline_hook_g_Original_slGetFeatureFunction, streamline_hook_g_SLGetFeatureFunctionHooked,
                                                    streamline_hook_g_SLGetFeatureFunctionTarget, "slGetFeatureFunction");
        }

        if (shouldHookCoreExports && originalGetPluginFunction) {
            if (!streamline_hook_g_Original_slGetPluginFunction) {
                streamline_hook_g_Original_slGetPluginFunction = originalGetPluginFunction;
            }

            hookedAnything |= InstallInlineHookOnce(reinterpret_cast<void*>(originalGetPluginFunction),
                                                    reinterpret_cast<void*>(Hooked_slGetPluginFunction),
                                                    streamline_hook_g_Original_slGetPluginFunction, streamline_hook_g_SLGetPluginFunctionHooked,
                                                    streamline_hook_g_SLGetPluginFunctionTarget, "slGetPluginFunction");
        }

        if (shouldHookCoreExports && originalSetD3DDevice) {
            if (!streamline_hook_g_Original_slSetD3DDevice) {
                streamline_hook_g_Original_slSetD3DDevice = originalSetD3DDevice;
            }

            hookedAnything |= InstallInlineHookOnce(
                reinterpret_cast<void*>(originalSetD3DDevice), reinterpret_cast<void*>(Hooked_slSetD3DDevice),
                streamline_hook_g_Original_slSetD3DDevice, streamline_hook_g_SLSetD3DDeviceHooked, streamline_hook_g_SLSetD3DDeviceTarget, "slSetD3DDevice");
        }

        if (shouldHookCoreExports && originalSetTag && mayHookAbiSensitive) {
            if (!streamline_hook_g_Original_slSetTag) {
                streamline_hook_g_Original_slSetTag = originalSetTag;
            }

            void* setTagDetour = mayHookV1Abi ? reinterpret_cast<void*>(&ce::streamline_v1::Hooked_slSetTagV1)
                                              : reinterpret_cast<void*>(Hooked_slSetTag);
            hookedAnything |=
                InstallInlineHookOnce(reinterpret_cast<void*>(originalSetTag), setTagDetour,
                                      streamline_hook_g_Original_slSetTag, streamline_hook_g_SLSetTagHooked, streamline_hook_g_SLSetTagTarget, "slSetTag");
        }

        if (shouldHookCoreExports && originalSetTagForFrame) {
            if (!streamline_hook_g_Original_slSetTagForFrame) {
                streamline_hook_g_Original_slSetTagForFrame = originalSetTagForFrame;
            }

            hookedAnything |= InstallInlineHookOnce(
                reinterpret_cast<void*>(originalSetTagForFrame), reinterpret_cast<void*>(Hooked_slSetTagForFrame),
                streamline_hook_g_Original_slSetTagForFrame, streamline_hook_g_SLSetTagForFrameHooked, streamline_hook_g_SLSetTagForFrameTarget, "slSetTagForFrame");
        }

        if (shouldHookCoreExports && originalEvaluateFeature && mayHookAbiSensitive) {
            if (!streamline_hook_g_Original_slEvaluateFeature) {
                streamline_hook_g_Original_slEvaluateFeature = originalEvaluateFeature;
            }

            void* evaluateDetour = mayHookV1Abi
                                       ? reinterpret_cast<void*>(&ce::streamline_v1::Hooked_slEvaluateFeatureV1)
                                       : reinterpret_cast<void*>(Hooked_slEvaluateFeature);
            hookedAnything |=
                InstallInlineHookOnce(reinterpret_cast<void*>(originalEvaluateFeature), evaluateDetour,
                                      streamline_hook_g_Original_slEvaluateFeature,
                                      streamline_hook_g_SLEvaluateFeatureHooked, streamline_hook_g_SLEvaluateFeatureTarget, "slEvaluateFeature");
        }

        // The two 1.x calls whose payload layout is unpublished. Recorded on the way
        // through and forwarded unchanged, so an ordinary session with the game's own
        // Streamline working normally produces the layouts the generation bridge needs.
        // Both exports exist only in 1.x, and the V1 gate is applied anyway.
        if (shouldHookCoreExports && mayHookV1Abi) {
            if (auto* originalSetFeatureConstants = reinterpret_cast<ce::streamline_v1::PFN_slSetFeatureConstantsV1>(
                    GetProcAddress(module, "slSetFeatureConstants"))) {
                if (!ce::streamline_v1::g_Original_slSetFeatureConstantsV1) {
                    ce::streamline_v1::g_Original_slSetFeatureConstantsV1 = originalSetFeatureConstants;
                }
                hookedAnything |= InstallInlineHookOnce(
                    reinterpret_cast<void*>(originalSetFeatureConstants),
                    reinterpret_cast<void*>(&ce::streamline_v1::Hooked_slSetFeatureConstantsV1),
                    ce::streamline_v1::g_Original_slSetFeatureConstantsV1,
                    ce::streamline_v1::g_SetFeatureConstantsV1Hooked,
                    ce::streamline_v1::g_SetFeatureConstantsV1Target, "slSetFeatureConstants");
            }
            if (auto* originalGetFeatureSettings = reinterpret_cast<ce::streamline_v1::PFN_slGetFeatureSettingsV1>(
                    GetProcAddress(module, "slGetFeatureSettings"))) {
                if (!ce::streamline_v1::g_Original_slGetFeatureSettingsV1) {
                    ce::streamline_v1::g_Original_slGetFeatureSettingsV1 = originalGetFeatureSettings;
                }
                hookedAnything |= InstallInlineHookOnce(
                    reinterpret_cast<void*>(originalGetFeatureSettings),
                    reinterpret_cast<void*>(&ce::streamline_v1::Hooked_slGetFeatureSettingsV1),
                    ce::streamline_v1::g_Original_slGetFeatureSettingsV1,
                    ce::streamline_v1::g_GetFeatureSettingsV1Hooked,
                    ce::streamline_v1::g_GetFeatureSettingsV1Target, "slGetFeatureSettings");
            }
        }

        if (shouldHookCoreExports && (originalSetTag || originalEvaluateFeature) && !mayHookAbiSensitive) {
            static std::atomic<bool> s_refusalLogged{false};
            if (!s_refusalLogged.exchange(true, std::memory_order_acq_rel)) {
                HookLogImportant(
                    "Streamline Hook: leaving slSetTag/slEvaluateFeature unhooked on %s - CE has no hook whose "
                    "calling convention matches %s. Overlay, capture and every generation-independent Streamline "
                    "hook are unaffected",
                    moduleBaseName, ce::streamline_api::Describe(generation));
            }
        }

        if (shouldHookCoreExports && moduleBit != 0 &&
            (streamline_hook_g_IATPatchesMask.load(std::memory_order_acquire) & moduleBit) == 0) {
            void* dummy = nullptr;
            if (originalGetFeatureFunction) {
                IATHook::PatchIATAllModules(moduleBaseName, "slGetFeatureFunction",
                                            reinterpret_cast<void*>(Hooked_slGetFeatureFunction), &dummy);
            }
            if (originalGetPluginFunction) {
                IATHook::PatchIATAllModules(moduleBaseName, "slGetPluginFunction",
                                            reinterpret_cast<void*>(Hooked_slGetPluginFunction), &dummy);
            }
            if (originalSetD3DDevice) {
                IATHook::PatchIATAllModules(moduleBaseName, "slSetD3DDevice",
                                            reinterpret_cast<void*>(Hooked_slSetD3DDevice), &dummy);
            }
            if (originalSetTag && mayHookAbiSensitive) {
                IATHook::PatchIATAllModules(moduleBaseName, "slSetTag",
                                            mayHookV1Abi
                                                ? reinterpret_cast<void*>(&ce::streamline_v1::Hooked_slSetTagV1)
                                                : reinterpret_cast<void*>(Hooked_slSetTag),
                                            &dummy);
            }
            if (originalSetTagForFrame) {
                IATHook::PatchIATAllModules(moduleBaseName, "slSetTagForFrame",
                                            reinterpret_cast<void*>(Hooked_slSetTagForFrame), &dummy);
            }
            if (originalEvaluateFeature && mayHookAbiSensitive) {
                IATHook::PatchIATAllModules(
                    moduleBaseName, "slEvaluateFeature",
                    mayHookV1Abi ? reinterpret_cast<void*>(&ce::streamline_v1::Hooked_slEvaluateFeatureV1)
                                 : reinterpret_cast<void*>(Hooked_slEvaluateFeature),
                    &dummy);
            }
            streamline_hook_g_IATPatchesMask.fetch_or(moduleBit, std::memory_order_acq_rel);
        }

        if (originalDLSSGSetOptions && ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad(
                                           "slDLSSGSetOptions", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slDLSSGSetOptions", reinterpret_cast<void*>(Hooked_slDLSSGSetOptions),
                reinterpret_cast<void*>(originalDLSSGSetOptions),
                reinterpret_cast<void**>(&streamline_hook_g_Original_slDLSSGSetOptions), "slDLSSGSetOptions");
        }

        if (originalDLSSGGetState &&
            ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slDLSSGGetState", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slDLSSGGetState", reinterpret_cast<void*>(Hooked_slDLSSGGetState),
                reinterpret_cast<void*>(originalDLSSGGetState), reinterpret_cast<void**>(&streamline_hook_g_Original_slDLSSGGetState),
                "slDLSSGGetState");
        }

        if (originalReflexSleep &&
            ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slReflexSleep", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slReflexSleep", reinterpret_cast<void*>(Hooked_slReflexSleep),
                reinterpret_cast<void*>(originalReflexSleep), reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSleep),
                "slReflexSleep");
        }

        if (originalReflexSetOptions && ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad(
                                            "slReflexSetOptions", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slReflexSetOptions", reinterpret_cast<void*>(Hooked_slReflexSetOptions),
                reinterpret_cast<void*>(originalReflexSetOptions),
                reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSetOptions), "slReflexSetOptions");
        }

        if (originalReflexSetConstants && ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad(
                                              "slReflexSetConstants", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slReflexSetConstants", reinterpret_cast<void*>(Hooked_slReflexSetConstants),
                reinterpret_cast<void*>(originalReflexSetConstants),
                reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSetConstants), "slReflexSetConstants");
        }
    }

    if (hookedAnything) {
        if (moduleBit != 0) {
            streamline_hook_g_InstalledModuleMask.fetch_or(moduleBit, std::memory_order_acq_rel);
        }
        HookLogImportant("Streamline Hook: Installed hooks for %s (%p)", moduleBaseName, module);
    }
    return true;

}


bool OpenLoadedModuleSnapshotWithRetry(HANDLE& snapshot,  MODULEENTRY32& firstEntry,  DWORD& error,  int& attempts, 
                                       bool& failedOnFirstEntry) {


    snapshot = INVALID_HANDLE_VALUE;
    error = ERROR_SUCCESS;
    attempts = 0;
    failedOnFirstEntry = false;

    constexpr int kMaxSnapshotAttempts = 4;
    for (int attempt = 1; attempt <= kMaxSnapshotAttempts; ++attempt) {
        attempts = attempt;
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
        if (snapshot == INVALID_HANDLE_VALUE) {
            error = GetLastError();
            if (ce::streamline_runtime_policy::IsRetryableLoadedModuleSnapshotError(static_cast<uint32_t>(error)) &&
                attempt < kMaxSnapshotAttempts) {
                continue;
            }
            return false;
        }

        firstEntry = {};
        firstEntry.dwSize = sizeof(firstEntry);
        if (Module32First(snapshot, &firstEntry)) {
            error = ERROR_SUCCESS;
            return true;
        }

        error = GetLastError();
        failedOnFirstEntry = true;
        CloseHandle(snapshot);
        snapshot = INVALID_HANDLE_VALUE;
        if (!ce::streamline_runtime_policy::IsRetryableLoadedModuleSnapshotError(static_cast<uint32_t>(error)) ||
            attempt == kMaxSnapshotAttempts) {
            return false;
        }
    }

    return false;

}


bool ScanLoadedStreamlineModules(bool pinFeatureResolution) {


    HANDLE snapshot = INVALID_HANDLE_VALUE;
    MODULEENTRY32 entry = {};
    DWORD error = ERROR_SUCCESS;
    int attempts = 0;
    bool failedOnFirstEntry = false;
    if (!OpenLoadedModuleSnapshotWithRetry(snapshot, entry, error, attempts, failedOnFirstEntry)) {
        if (!streamline_hook_g_ModuleSnapshotFailureLogged.exchange(true, std::memory_order_acq_rel)) {
            HookLogImportant(
                failedOnFirstEntry
                    ? "Streamline Hook: Loaded-module enumeration was empty for feature hooks error=%lu attempts=%d "
                      "retryable=%d"
                    : "Streamline Hook: Failed to enumerate loaded modules for feature hooks error=%lu attempts=%d "
                      "retryable=%d",
                static_cast<unsigned long>(error), attempts,
                ce::streamline_runtime_policy::IsRetryableLoadedModuleSnapshotError(static_cast<uint32_t>(error)) ? 1
                                                                                                                  : 0);
        }
        return false;
    }

    streamline_hook_g_ModuleSnapshotFailureLogged.store(false, std::memory_order_release);

    bool foundModule = false;
    size_t streamlineModuleCount = 0;
    size_t hookedModuleCount = 0;
    do {
        const char* moduleNameOrPath = entry.szExePath[0] != '\0' ? entry.szExePath : entry.szModule;
        if (!ce::streamline_runtime_policy::IsStreamlineModuleNameForFeatureHooking(moduleNameOrPath)) {
            continue;
        }

        foundModule = true;
        ++streamlineModuleCount;
        g_FGCompat.SetStreamlineSupportPresent(true);
        if (InstallHooksForModule(entry.hModule, moduleNameOrPath)) {
            ++hookedModuleCount;
        }
    } while (Module32Next(snapshot, &entry));

    const DWORD iterationError = GetLastError();
    CloseHandle(snapshot);

    // Late-inject / runtime retry: proactively resolve the DLSS-G and Reflex
    // feature functions through the interposer now that every Streamline
    // module is loaded and the runtime is stable (no loader lock). The startup
    // path defers this to the app's own slGetFeatureFunction / slSetD3DDevice
    // calls; under late injection those already happened before hook
    // installation, so the game's cached feature pointers (e.g.
    // slDLSSGSetOptions, resolved at startup) would never become observable
    // and the FG multiplier / Reflex state signals would stay dead. Hooking
    // them here makes the game's next slDLSSGSetOptions call flow through CE
    // (session 20260811_230524: Talos runs 4x MFG via
    // slDLSSGSetOptions(numFramesToGenerate=3) but the overlay reported DLSS
    // 2x because the CreateFeature parameter object carries no multiplier).
    // Proactive scan: the HookThread can race the app tearing the Streamline runtime down
    // (DLSS -> FSR switch unloads sl.dlss_g/sl.reflex before sl.interposer; crash
    // 20260812_042259). When pinning is enabled the query path pins the queried modules and
    // fails closed on teardown. Runtime-activity callers (RetryResolve*) keep pinning off —
    // they can run under the loader lock (SL DllMain), where LoadLibrary is not allowed.
    const bool resolvedDLSSG = TryResolveDLSSGFeatureHooks(pinFeatureResolution);
    const bool resolvedReflex = TryResolveReflexFeatureHooks(pinFeatureResolution);
    if (resolvedDLSSG || resolvedReflex) {
        // The runtime retry path re-scans while FG is active; only log the
        // resolution summary until it is fully complete and then sparsely.
        static std::atomic<int> s_featureResolveLogCount{0};
        const int resolveLogCount = s_featureResolveLogCount.fetch_add(1, std::memory_order_relaxed);
        const bool allFeatureHooksComplete = streamline_hook_g_DLSSGSetOptionsHooked.load(std::memory_order_acquire) &&
                                             streamline_hook_g_DLSSGGetStateHooked.load(std::memory_order_acquire) &&
                                             AreReflexFeatureHooksComplete();
        if (resolveLogCount < 5 || !allFeatureHooksComplete || (resolveLogCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: Resolved feature hooks after loaded-module scan "
                "(dlssgSetOptionsHooked=%d dlssgGetStateHooked=%d reflexSleepHooked=%d reflexSetOptionsHooked=%d "
                "reflexSetConstantsHooked=%d log=%d)",
                streamline_hook_g_DLSSGSetOptionsHooked.load(std::memory_order_acquire) ? 1 : 0,
                streamline_hook_g_DLSSGGetStateHooked.load(std::memory_order_acquire) ? 1 : 0,
                streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire) ? 1 : 0,
                streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire) ? 1 : 0,
                streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire) ? 1 : 0,
                resolveLogCount + 1);
        }
    }

    if (attempts > 1 && !streamline_hook_g_ModuleSnapshotRetrySuccessLogged.exchange(true, std::memory_order_acq_rel)) {
        HookLogImportant(
            "Streamline Hook: Loaded-module snapshot recovered after transient retry (attempts=%d modules=%zu "
            "hooked=%zu)",
            attempts, streamlineModuleCount, hookedModuleCount);
    }
    if (iterationError != ERROR_SUCCESS && iterationError != ERROR_NO_MORE_FILES &&
        !streamline_hook_g_ModuleSnapshotFailureLogged.exchange(true, std::memory_order_acq_rel)) {
        HookLogImportant(
            "Streamline Hook: Loaded-module enumeration ended unexpectedly for feature hooks error=%lu "
            "(modules=%zu hooked=%zu)",
            static_cast<unsigned long>(iterationError), streamlineModuleCount, hookedModuleCount);
    }
    return foundModule;

}


bool AreReflexFeatureHooksComplete() {


    return streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire) &&
           streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire) &&
           (streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire) ||
            streamline_hook_g_ReflexSetConstantsUnavailableQueries.load(std::memory_order_acquire) >=
                kReflexSetConstantsUnavailableQueryLimit);

}


void RetryResolveReflexFeatureHooksForRuntimeActivity(const char* source) {


    if (AreReflexFeatureHooksComplete()) {
        return;
    }

    constexpr ULONGLONG kRetryIntervalMs = 2500;
    const ULONGLONG nowMs = GetTickCount64();
    ULONGLONG previousMs = streamline_hook_g_ReflexFeatureHookRetryLastMs.load(std::memory_order_acquire);
    if (previousMs != 0 && nowMs >= previousMs && (nowMs - previousMs) < kRetryIntervalMs) {
        return;
    }

    if (!streamline_hook_g_ReflexFeatureHookRetryLastMs.compare_exchange_strong(previousMs, nowMs, std::memory_order_acq_rel,
                                                                std::memory_order_acquire)) {
        return;
    }

    const bool foundModule = ScanLoadedStreamlineModules();
    const bool resolved = TryResolveReflexFeatureHooks();
    static std::atomic<int> s_lateReflexRetryLogCount{0};
    const int logCount = s_lateReflexRetryLogCount.fetch_add(1, std::memory_order_relaxed);
    if (resolved || logCount < 10 || (logCount % 24) == 0) {
        HookLogImportant(
            "Streamline Hook: Late Reflex feature hook retry during DLSSG runtime activity "
            "(source=%s foundModule=%d resolved=%d sleepHooked=%d setOptionsHooked=%d setConstantsHooked=%d "
            "manualLimiter=%d targetIntervalUs=%u)",
            source ? source : "unknown", foundModule ? 1 : 0, resolved ? 1 : 0,
            streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire) ? 1 : 0,
            streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire) ? 1 : 0,
            streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_ReflexLimiter.IsManualLimiterConfiguredOrActive() ? 1 : 0, g_ReflexLimiter.GetTargetIntervalUs());
    }

}
