#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "source_fragment_reader.h"

// GTA session 20260925_165708: after every amd_fidelityfx_dx12.dll reload GTA resolved the FFX exports outside
// CE's GetProcAddress/IAT routes and called ffxCreateContext before the once-a-second cached-slot rescan could
// reroute its stored pointer. CE saw destroy and configure but never create, so the all-FG-contexts-destroyed
// teardown never ran. These source contracts pin the two fixes: an ffxCreateContext entry breakpoint armed at
// module load, and adoption of unseen contexts at their first successful configure.

namespace {

std::string ReadSource(const char* relativePath) {
    return ce::test_source::ReadFile(std::filesystem::current_path() / relativePath);
}

std::string Between(const std::string& source, const std::string& begin, const std::string& end) {
    const size_t start = source.find(begin);
    if (start == std::string::npos) {
        return {};
    }
    const size_t stop = source.find(end, start + begin.size());
    return source.substr(start, stop == std::string::npos ? std::string::npos : stop - start);
}

TEST(FFXCreateObservationSourceTest, CreateBreakpointIsArmedBeforeModuleLoadReturnsAndBeforeSlowRoutes) {
    const std::string install = ReadSource("hook/apis/ffx_hook_install.cpp");
    ASSERT_FALSE(install.empty());
    const std::string body = Between(install, "bool ffx_hook_InstallHooksForModule(", "\nbool WriteFfxExportEntryByte(");
    ASSERT_FALSE(body.empty());

    const size_t refresh = body.find("RefreshDirectOriginalForModuleReload(ffx_hook_g_Original_ffxCreateContext");
    const size_t arm = body.find("ArmFfxCreateContextBreakpoint(hModule, createCtx");
    const size_t iatPatch = body.find("PatchIATAllModules(ffx_hook_moduleName, \"ffxCreateContext\"");
    const size_t slotRefresh = body.find("ce::ffx_cached_pointer_router::Refresh(");
    ASSERT_NE(refresh, std::string::npos);
    ASSERT_NE(arm, std::string::npos);
    ASSERT_NE(iatPatch, std::string::npos);
    ASSERT_NE(slotRefresh, std::string::npos);
    EXPECT_LT(refresh, arm) << "a trapped call resumes in the detour, which forwards to the refreshed original";
    EXPECT_LT(arm, iatPatch) << "arm before the per-module IAT sweeps that take tens of milliseconds";
    EXPECT_LT(arm, slotRefresh);
}

TEST(FFXCreateObservationSourceTest, TrappedCreateResumesInTheDetourInsteadOfRunningInsideTheHandler) {
    const std::string breakpoint = ReadSource("hook/apis/ffx_hook_create_breakpoint.cpp");
    ASSERT_FALSE(breakpoint.empty());
    const std::string veh = Between(breakpoint, "LONG WINAPI FfxCreateContextBreakpointVEH(", "\n#endif");
    ASSERT_FALSE(veh.empty());

    EXPECT_NE(veh.find("ClassifyEntryBreakpoint(hitsTarget, armed, targetByteIsBreakpoint)"), std::string::npos);
    EXPECT_NE(veh.find("ffx_hook_t_FfxCreateContextOriginalOverride = reinterpret_cast<PfnFfxCreateContext>(target)"),
              std::string::npos);
    EXPECT_NE(veh.find("ctx->Rip = reinterpret_cast<DWORD64>(&Hooked_ffxCreateContext)"), std::string::npos);
    // Unlike the ffxConfigure handler, the create (which builds a DXGI swapchain) must not run in dispatch.
    EXPECT_EQ(veh.find("Hooked_ffxCreateContext("), std::string::npos);
    EXPECT_EQ(veh.find("ctx->Rsp"), std::string::npos) << "entry state is intact; the stack must not be rewritten";

    // The armed flag is published before the byte exists, so a racing trap is always accounted for.
    const std::string arm = Between(breakpoint, "bool ArmPinnedCreateBreakpointLocked(", "\n}\n");
    const size_t publish = arm.find("g_CreateBreakpointArmed.store(true");
    const size_t write = arm.find("WriteFfxExportEntryByte(target, 0xCC)");
    ASSERT_NE(publish, std::string::npos);
    ASSERT_NE(write, std::string::npos);
    EXPECT_LT(publish, write);
}

TEST(FFXCreateObservationSourceTest, EveryCreateForwardPausesTheBreakpointAndReArmsAfterwards) {
    const std::string context = ReadSource("hook/apis/ffx_hook_context.cpp");
    const std::string breakpoint = ReadSource("hook/apis/ffx_hook_create_breakpoint.cpp");
    ASSERT_FALSE(context.empty());
    ASSERT_FALSE(breakpoint.empty());

    const std::string create =
        Between(context, "ffxReturnCode_t Hooked_ffxCreateContext(", "ffxReturnCode_t Hooked_ffxDestroyContext(");
    ASSERT_FALSE(create.empty());
    // A raw forward to the armed export would trap again and observe the same create twice.
    EXPECT_EQ(create.find("ffx_hook_g_Original_ffxCreateContext(ffx_hook_context"), std::string::npos);
    const size_t consume = create.find("ffx_hook_t_FfxCreateContextOriginalOverride = nullptr");
    const size_t forward = create.find("CallFfxCreateContextOriginalGuarded(originalCreate, ffx_hook_context");
    ASSERT_NE(consume, std::string::npos);
    ASSERT_NE(forward, std::string::npos);
    EXPECT_LT(consume, forward);

    const std::string guarded = Between(breakpoint, "ffxReturnCode_t CallFfxCreateContextOriginalGuarded(",
                                        "void SuspendFfxCreateContextBreakpoint(");
    const size_t pause = guarded.find("WriteFfxExportEntryByte(target, g_CreateBreakpointOriginalByte)");
    const size_t call = guarded.find("originalCreate(ffx_hook_context, ffx_hook_desc, memCb)");
    const size_t rearm = guarded.find("RearmCurrentCreateBreakpoint(\"post-call rearm\")");
    ASSERT_NE(pause, std::string::npos);
    ASSERT_NE(call, std::string::npos);
    ASSERT_NE(rearm, std::string::npos);
    EXPECT_LT(pause, call);
    EXPECT_LT(call, rearm);
    EXPECT_NE(guarded.find("remainingDepth == 0"), std::string::npos) << "only the outermost forward re-arms";
}

TEST(FFXCreateObservationSourceTest, ArmingProvesTheTargetIsStillTheLoadedModulesExport) {
    const std::string breakpoint = ReadSource("hook/apis/ffx_hook_create_breakpoint.cpp");
    ASSERT_FALSE(breakpoint.empty());
    const std::string pin = Between(breakpoint, "bool PinLiveCreateContextExport(", "\n}\n");
    EXPECT_NE(pin.find("owner != expectedModule"), std::string::npos);
    EXPECT_NE(pin.find("GetProcAddress(owner, \"ffxCreateContext\")) != target"), std::string::npos);
    const std::string arm = Between(breakpoint, "bool ArmFfxCreateContextBreakpoint(", "ffxReturnCode_t CallFfx");
    const size_t pinCall = arm.find("PinLiveCreateContextExport(module, targetAddress, &pinned)");
    const size_t lock = arm.find("std::lock_guard<std::mutex> lock(g_CreateBreakpointMutex)");
    const size_t unpin = arm.find("FreeLibrary(pinned)");
    ASSERT_NE(pinCall, std::string::npos);
    ASSERT_NE(lock, std::string::npos);
    ASSERT_NE(unpin, std::string::npos);
    EXPECT_LT(pinCall, lock);
    EXPECT_LT(lock, unpin) << "the pin is released only after the mutex scope, never under it";
}

TEST(FFXCreateObservationSourceTest, DormantAndShutdownRestoreTheCreateEntryByte) {
    const std::string api = ReadSource("hook/apis/ffx_hook_api.cpp");
    ASSERT_FALSE(api.empty());
    const std::string dormant = Between(api, "void EnterDormant()", "void ReactivateResidentHooks()");
    EXPECT_NE(dormant.find("SuspendFfxCreateContextBreakpoint("), std::string::npos);
    const std::string reactivate = Between(api, "void ReactivateResidentHooks()", "void Shutdown()");
    EXPECT_NE(reactivate.find("ResumeFfxCreateContextBreakpoint("), std::string::npos);

    const std::string shutdown = Between(api, "void Shutdown() {", "\n}\n}");
    const size_t restore = shutdown.find("ShutdownFfxCreateContextBreakpoint()");
    const size_t clearOriginal = shutdown.find("ffx_hook_g_Original_ffxCreateContext = nullptr");
    ASSERT_NE(restore, std::string::npos);
    ASSERT_NE(clearOriginal, std::string::npos);
    EXPECT_LT(restore, clearOriginal);
}

TEST(FFXCreateObservationSourceTest, UnseenContextsAreAdoptedOnEverySuccessfulConfigurePath) {
    const std::string context = ReadSource("hook/apis/ffx_hook_context.cpp");
    ASSERT_FALSE(context.empty());
    const std::string configure = Between(context, "ffxReturnCode_t Hooked_ffxConfigure(", "\n}\n");
    ASSERT_FALSE(configure.empty());

    const size_t tracked = configure.find("contextTracked = !contextHandle ||");
    ASSERT_NE(tracked, std::string::npos);

    // Streamline startup window: forwarded without CE processing, but still tracked.
    const std::string startup = Between(configure, "if (DXGIShared::IsStreamlineStartupTransitionWindowActive())",
                                        "return startupResult;");
    EXPECT_NE(startup.find("AdoptUnobservedFFXContextFromConfigure(contextHandle"), std::string::npos);

    // Normal path: before the FG-only filter, so the swapchain context's RegisterUiResource is adopted too.
    const size_t failedReturn = configure.find("if (result != ffx_hook_FFX_API_RETURN_OK || !ffx_hook_desc)");
    const size_t adopt = configure.find("AdoptUnobservedFFXContextFromConfigure(contextHandle", failedReturn);
    const size_t fgFilter = configure.find("if (!parsed.recognized) {", failedReturn);
    ASSERT_NE(failedReturn, std::string::npos);
    ASSERT_NE(adopt, std::string::npos);
    ASSERT_NE(fgFilter, std::string::npos);
    EXPECT_LT(adopt, fgFilter);
}

TEST(FFXCreateObservationSourceTest, AdoptedDX12FrameGenerationContextsFeedTheDestroyTeardownCount) {
    const std::string adoption = ReadSource("hook/apis/ffx_hook_context_adoption.cpp");
    ASSERT_FALSE(adoption.empty());
    const size_t emplace = adoption.find("ffx_hook_g_ContextTypeMap.emplace(contextHandle, adoption.effectId).second");
    const size_t count = adoption.find("ffx_hook_g_FGContextCount.fetch_add(1");
    ASSERT_NE(emplace, std::string::npos);
    ASSERT_NE(count, std::string::npos);
    EXPECT_LT(emplace, count) << "only a newly tracked context may be counted, or a race double-counts it";
    EXPECT_NE(adoption.find("!adoption.vulkan && (adoption.effectId == ffx_hook_FFX_API_EFFECT_ID_FRAMEGENERATION"),
              std::string::npos);
    EXPECT_NE(adoption.find("ffx_hook_g_VulkanContextSet.insert(contextHandle)"), std::string::npos);
}

// GTA session 20260925_172935: the rescan re-armed the ffxConfigure breakpoint on the startup runtime's address
// 80 ms after GTA had unloaded that image. Only the unmapped page stopped the write; an image mapped there next
// would have received a 0xCC mid-code. Arming and restoring must prove the address is still the export.
TEST(FFXCreateObservationSourceTest, ConfigureBreakpointProvesTheExportBeforeEveryWriteAndDeferral) {
    const std::string install = ReadSource("hook/apis/ffx_hook_install.cpp");
    ASSERT_FALSE(install.empty());

    const std::string proof = Between(install, "static bool IsProvenLiveFfxConfigureExportLocked(", "\n}\n");
    ASSERT_FALSE(proof.empty());
    EXPECT_NE(proof.find("ffx_hook_g_FfxModuleUnloadGeneration.load("), std::string::npos)
        << "a cached proof must expire with every FFX unload";
    EXPECT_NE(proof.find("IsLiveFfxExportEntry(target, \"ffxConfigure\", nullptr)"), std::string::npos);

    const std::string arm = Between(install, "bool ArmFfxConfigureBreakpoint(", "\n}\n");
    ASSERT_FALSE(arm.empty());
    const size_t disarmed = arm.find("ffx_hook_g_ffxConfigureVehPermanentlyDisarmed.load(");
    const size_t armProof = arm.find("IsProvenLiveFfxConfigureExportLocked(reinterpret_cast<void*>(target))");
    const size_t deferral = arm.find("ffx_hook_g_FfxConfigureOriginalForwardDepth.load(");
    const size_t write = arm.find("WriteFfxExportEntryByte(reinterpret_cast<void*>(target), 0xCC)");
    ASSERT_NE(disarmed, std::string::npos);
    ASSERT_NE(armProof, std::string::npos);
    ASSERT_NE(deferral, std::string::npos);
    ASSERT_NE(write, std::string::npos);
    EXPECT_LT(disarmed, armProof) << "the permanently-disarmed fast path stays free";
    EXPECT_LT(armProof, deferral) << "the deferred branch publishes the target as the callable original";
    EXPECT_LT(armProof, write);

    const std::string restore = Between(install, "void RestoreFfxConfigureBreakpointIfCurrent(", "\n}\n");
    const size_t restoreProof = restore.find("IsProvenLiveFfxConfigureExportLocked(target)");
    const size_t restoreWrite = restore.find("WriteFfxExportEntryByte(target, ffx_hook_g_ffxConfigureOriginalFirstByte)");
    ASSERT_NE(restoreProof, std::string::npos);
    ASSERT_NE(restoreWrite, std::string::npos);
    EXPECT_LT(restoreProof, restoreWrite);
}

TEST(FFXCreateObservationSourceTest, FFXRuntimeUnloadDisarmsBothBreakpointsFromTheLoaderNotification) {
    const std::string notify = ReadSource("hook/main_overlay_detect.cpp");
    const std::string api = ReadSource("hook/apis/ffx_hook_api.cpp");
    const std::string breakpoint = ReadSource("hook/apis/ffx_hook_create_breakpoint.cpp");
    ASSERT_FALSE(notify.empty());
    ASSERT_FALSE(api.empty());
    ASSERT_FALSE(breakpoint.empty());

    const size_t unloadedBranch = notify.find("reason == LDR_DLL_NOTIFICATION_REASON_UNLOADED");
    const size_t dispatch = notify.find("FFXHook::OnModuleUnloaded(data->DllBase, data->SizeOfImage, base)");
    ASSERT_NE(unloadedBranch, std::string::npos);
    ASSERT_NE(dispatch, std::string::npos);
    EXPECT_LT(unloadedBranch, dispatch);

    const std::string unload = Between(api, "void OnModuleUnloaded(", "\n}\n");
    ASSERT_FALSE(unload.empty());
    EXPECT_EQ(unload.find("lock_guard"), std::string::npos) << "runs under the loader lock";
    EXPECT_NE(unload.find("ffx_hook_g_FfxModuleUnloadGeneration.fetch_add(1"), std::string::npos);
    EXPECT_NE(unload.find("ffx_hook_g_ffxConfigureVehArmed.exchange(false"), std::string::npos);
    EXPECT_NE(unload.find("InvalidateFfxCreateContextBreakpointForUnloadedImage(moduleBase, moduleSizeBytes)"),
              std::string::npos);
    // The configure target stays: the next install compares it to recognize a reload.
    EXPECT_EQ(unload.find("ffx_hook_g_ffxConfigureTarget.store("), std::string::npos);

    const std::string invalidate =
        Between(breakpoint, "void InvalidateFfxCreateContextBreakpointForUnloadedImage(", "\n}\n");
    ASSERT_FALSE(invalidate.empty());
    EXPECT_EQ(invalidate.find("lock_guard"), std::string::npos);
    EXPECT_NE(invalidate.find("g_CreateBreakpointArmed.exchange(false"), std::string::npos);
}

}  // namespace
