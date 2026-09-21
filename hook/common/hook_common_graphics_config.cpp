// Resolution of the active graphics override configuration: the merge of the
// host's shared-memory config with the hook-local one, the cached per-thread
// view every hot path reads, and the DLSS driver-settings values derived from
// the result. Split out of hook_common.cpp, which owns process-global state
// bridging and file logging; this unit is the config half of it.
#include "hook_common.h"
#include <string.h>
#include <windows.h>
#include <mutex>
#include "../../common/shared_defs.h"
#include "../../common/sharpen_policy.h"
#include "ngx_drs_override.h"

// Helpers for Config Overrides
GraphicsConfig GetActiveGraphicsConfig() {
    static GraphicsConfig mergedConfig;
    static uint32_t lastVersion = 0xFFFFFFFF;
    static SharedMemoryLayout* lastSharedMemory = nullptr;
    static uint32_t lastUpdateTick = 0;
    static std::mutex configMutex;

    std::lock_guard<std::mutex> lock(configMutex);

    SharedMemoryLayout* currentSharedMemory = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    uint32_t currentVersion = currentSharedMemory
                                  ? currentSharedMemory->configVersion.load(std::memory_order_acquire)
                                  : 0;

    DWORD now = GetTickCount();
    if (currentSharedMemory == lastSharedMemory && currentVersion == lastVersion && (now - lastUpdateTick < 1000)) {
        return mergedConfig;
    }

    lastSharedMemory = currentSharedMemory;
    lastVersion = currentVersion;
    lastUpdateTick = now;

    if (currentSharedMemory) {
        const auto& shmGfx = currentSharedMemory->graphicsConfig;
        mergedConfig.vsyncMode = shmGfx.vsyncMode;
        mergedConfig.anisotropicFiltering = shmGfx.anisotropicFiltering;
        mergedConfig.samplerOverrideMode = shmGfx.samplerOverrideMode[0] ? shmGfx.samplerOverrideMode : "safe";
        mergedConfig.mipMapping = shmGfx.mipMapping;
        mergedConfig.mipBias = shmGfx.mipBias;
        mergedConfig.mipBiasMode = shmGfx.mipBiasMode;
        mergedConfig.forceMipBiasClamp = shmGfx.forceMipBiasClamp;
        mergedConfig.msaaSamples = shmGfx.msaaSamples;
        mergedConfig.sharpenMode = ce::sharpen::ModeName(static_cast<ce::sharpen::Mode>(shmGfx.sharpenMode));
        mergedConfig.sharpenColorSpace =
            ce::sharpen::ConfiguredSpaceName(static_cast<ce::sharpen::ConfiguredSpace>(shmGfx.sharpenColorSpace));
        mergedConfig.sharpenStrength = ce::sharpen::ClampStrength(shmGfx.sharpenStrength);
        mergedConfig.sharpenIntensity = ce::sharpen::ClampIntensity(shmGfx.sharpenIntensity);
        mergedConfig.nvLodSpreadFix = shmGfx.nvLodSpreadFix;
        mergedConfig.forceRayReconstruction = shmGfx.forceRayReconstruction;
        mergedConfig.legacyD3DNativeOverlay = shmGfx.legacyD3DNativeOverlay;
        mergedConfig.rayReconstructionOptimalSettings = shmGfx.rayReconstructionOptimalSettings;
        mergedConfig.disablePostProcessingEffects = shmGfx.disablePostProcessingEffects;
        mergedConfig.tonemapperSharpen = shmGfx.tonemapperSharpen;
        mergedConfig.internalFpsLimit = shmGfx.internalFpsLimit;
        mergedConfig.internalAnisotropicFiltering = shmGfx.internalAnisotropicFiltering;
        mergedConfig.internalTextureMipBias = shmGfx.internalTextureMipBias;
        mergedConfig.displayGamma = shmGfx.displayGamma;
        mergedConfig.depthOfField = shmGfx.depthOfField;
        mergedConfig.dlssSuperResolution = shmGfx.dlssSuperResolution;
        mergedConfig.dlssScreenPercentage = shmGfx.dlssScreenPercentage;
        mergedConfig.hdrOutput = shmGfx.hdrOutput;
        mergedConfig.hdrPeakLuminance = shmGfx.hdrPeakLuminance;
        mergedConfig.hdrPaperWhite = shmGfx.hdrPaperWhite;
        mergedConfig.hdrUiLuminance = shmGfx.hdrUiLuminance;
        mergedConfig.hdrMinLuminance = shmGfx.hdrMinLuminance;
        mergedConfig.hdrColorGamut = shmGfx.hdrColorGamut;
        mergedConfig.ue5CustomCVarOverrideMask = shmGfx.ue5CustomCVarOverrideMask;
        memcpy(mergedConfig.ue5CustomCVarOverrideValues.data(), shmGfx.ue5CustomCVarOverrideValues,
               sizeof(shmGfx.ue5CustomCVarOverrideValues));
        mergedConfig.cpuPrerenderLimit = shmGfx.prerenderLimit;
        mergedConfig.backbufferCount = shmGfx.backbufferCount;
        mergedConfig.frameLatency = shmGfx.frameLatency;
        mergedConfig.sgssaa = shmGfx.sgssaa;
        mergedConfig.disableAutoMipBias = shmGfx.disableAutoMipBias;
        mergedConfig.dlssAutoExposure = shmGfx.dlssAutoExposure;
        mergedConfig.dlssExposureNormalization = shmGfx.dlssExposureNormalization;

        mergedConfig.parsed.presetDLAA = shmGfx.dlssPresetDLAA;
        mergedConfig.parsed.presetQuality = shmGfx.dlssPresetQuality;
        mergedConfig.parsed.presetBalanced = shmGfx.dlssPresetBalanced;
        mergedConfig.parsed.presetPerformance = shmGfx.dlssPresetPerformance;
        mergedConfig.parsed.presetUltraPerformance = shmGfx.dlssPresetUltraPerformance;
        mergedConfig.parsed.presetUltraQuality = shmGfx.dlssPresetUltraQuality;

        mergedConfig.parsed.rrPresetDLAA = shmGfx.dlssRRPresetDLAA;
        mergedConfig.parsed.rrPresetQuality = shmGfx.dlssRRPresetQuality;
        mergedConfig.parsed.rrPresetBalanced = shmGfx.dlssRRPresetBalanced;
        mergedConfig.parsed.rrPresetPerformance = shmGfx.dlssRRPresetPerformance;
        mergedConfig.parsed.rrPresetUltraPerformance = shmGfx.dlssRRPresetUltraPerformance;
        mergedConfig.parsed.rrPresetUltraQuality = shmGfx.dlssRRPresetUltraQuality;

        mergedConfig.parsed.srPreset = shmGfx.dlssSRPreset;
        mergedConfig.parsed.rrPreset = shmGfx.dlssRRPreset;

        mergedConfig.parsed.dlssSharpening = shmGfx.dlssSharpening;
        mergedConfig.parsed.dlssFGFactor = shmGfx.dlssFGFactor;
        mergedConfig.parsed.fgPreset = NormalizeDLSSFGPreset(shmGfx.dlssFGPreset);
        mergedConfig.parsed.fgMode = IsDlssFGMode(shmGfx.dlssFGMode) ? shmGfx.dlssFGMode : kDlssFGModeDefault;
        mergedConfig.parsed.fgFixedCount = NormalizeDlssFGCount(shmGfx.dlssFGFixedCount);
        mergedConfig.parsed.fgDynamicMax = NormalizeDlssFGCount(shmGfx.dlssFGDynamicMax);
        mergedConfig.parsed.fgTargetFps = NormalizeDlssFGTargetFps(shmGfx.dlssFGTargetFps);

        if (shmGfx.dlssFGPreset > 0) {
            static uint32_t lastLoggedFGPresetSHM = 0;
            if (shmGfx.dlssFGPreset != lastLoggedFGPresetSHM) {
                HookLog("Config: Received FGPreset %u from SHM", shmGfx.dlssFGPreset);
                lastLoggedFGPresetSHM = shmGfx.dlssFGPreset;
            }
        }

        if (shmGfx.dlssSRPreset > 0) {
            static uint32_t lastLoggedSHM = 0;
            if (shmGfx.dlssSRPreset != lastLoggedSHM) {
                HookLog("Config: Received SRPreset %u from SHM", shmGfx.dlssSRPreset);
                lastLoggedSHM = shmGfx.dlssSRPreset;
            }
        }
    } else {
        // No IPC, stick to defaults
        mergedConfig = GraphicsConfig();
    }

    // Shared memory already contains the host's fully resolved per-process
    // profile and remains authoritative for hot reloads. Use the hook-local
    // fully resolved config only before IPC exists; selectively overlaying
    // non-default values made it impossible for a profile to reset a global
    // override back to default/strict/false.
    if (g_pLocalConfig && !(g_IPC && g_IPC->GetSharedMem())) {
        mergedConfig = g_pLocalConfig->graphics;
        if (g_pLocalConfig->graphics.parsed.srPreset > 0) {
            static uint32_t lastLoggedLocal = 0;
            if (g_pLocalConfig->graphics.parsed.srPreset != lastLoggedLocal) {
                HookLog("Config: Local srPreset is %u", g_pLocalConfig->graphics.parsed.srPreset);
                lastLoggedLocal = g_pLocalConfig->graphics.parsed.srPreset;
            }
        }
        if (g_pLocalConfig->graphics.cpuPrerenderLimit > -0.5f) {
            mergedConfig.cpuPrerenderLimit = g_pLocalConfig->graphics.cpuPrerenderLimit;
        }
        if (g_pLocalConfig->graphics.vsyncMode != "default" && !g_pLocalConfig->graphics.vsyncMode.empty()) {
            mergedConfig.vsyncMode = g_pLocalConfig->graphics.vsyncMode;
        }
        if (HasBackbufferCountOverride(g_pLocalConfig->graphics.backbufferCount)) {
            mergedConfig.backbufferCount = g_pLocalConfig->graphics.backbufferCount;
        }
        if (g_pLocalConfig->graphics.frameLatency > 0) {
            mergedConfig.frameLatency = g_pLocalConfig->graphics.frameLatency;
        }
        if (g_pLocalConfig->graphics.sgssaa) {
            mergedConfig.sgssaa = g_pLocalConfig->graphics.sgssaa;
        }
        if (g_pLocalConfig->graphics.disableAutoMipBias) {
            mergedConfig.disableAutoMipBias = g_pLocalConfig->graphics.disableAutoMipBias;
        }
        if (g_pLocalConfig->graphics.dlssAutoExposure != "default" &&
            !g_pLocalConfig->graphics.dlssAutoExposure.empty()) {
            mergedConfig.dlssAutoExposure = g_pLocalConfig->graphics.dlssAutoExposure;
        }
        if (g_pLocalConfig->graphics.dlssExposureNormalization != "default" &&
            !g_pLocalConfig->graphics.dlssExposureNormalization.empty()) {
            mergedConfig.dlssExposureNormalization = g_pLocalConfig->graphics.dlssExposureNormalization;
        }

        // Missing overrides added to fix regression
        if (g_pLocalConfig->graphics.anisotropicFiltering != "default" &&
            !g_pLocalConfig->graphics.anisotropicFiltering.empty()) {
            mergedConfig.anisotropicFiltering = g_pLocalConfig->graphics.anisotropicFiltering;
        }
        mergedConfig.samplerOverrideMode = g_pLocalConfig->graphics.samplerOverrideMode;
        if (g_pLocalConfig->graphics.mipMapping != "default" && !g_pLocalConfig->graphics.mipMapping.empty()) {
            mergedConfig.mipMapping = g_pLocalConfig->graphics.mipMapping;
        }
        if (g_pLocalConfig->graphics.mipBias != "default" && !g_pLocalConfig->graphics.mipBias.empty()) {
            mergedConfig.mipBias = g_pLocalConfig->graphics.mipBias;
        }
        if (g_pLocalConfig->graphics.mipBiasMode != "strict" && !g_pLocalConfig->graphics.mipBiasMode.empty()) {
            mergedConfig.mipBiasMode = g_pLocalConfig->graphics.mipBiasMode;
        }
        if (g_pLocalConfig->graphics.forceMipBiasClamp) {
            mergedConfig.forceMipBiasClamp = true;
        }
        if (g_pLocalConfig->graphics.msaaSamples != "default" && !g_pLocalConfig->graphics.msaaSamples.empty()) {
            mergedConfig.msaaSamples = g_pLocalConfig->graphics.msaaSamples;
        }
        // "off" is a real decision here, not an absent value, so a process-local
        // config can switch sharpening off again over a host that enabled it.
        if (!g_pLocalConfig->graphics.sharpenMode.empty()) {
            mergedConfig.sharpenMode = g_pLocalConfig->graphics.sharpenMode;
            mergedConfig.sharpenColorSpace = g_pLocalConfig->graphics.sharpenColorSpace;
            mergedConfig.sharpenStrength = g_pLocalConfig->graphics.sharpenStrength;
            mergedConfig.sharpenIntensity = g_pLocalConfig->graphics.sharpenIntensity;
        }

        // Apply Preset Overrides from g_pLocalConfig
        if (g_pLocalConfig->graphics.parsed.presetDLAA > 0)
            mergedConfig.parsed.presetDLAA = g_pLocalConfig->graphics.parsed.presetDLAA;
        if (g_pLocalConfig->graphics.parsed.presetQuality > 0)
            mergedConfig.parsed.presetQuality = g_pLocalConfig->graphics.parsed.presetQuality;
        if (g_pLocalConfig->graphics.parsed.presetBalanced > 0)
            mergedConfig.parsed.presetBalanced = g_pLocalConfig->graphics.parsed.presetBalanced;
        if (g_pLocalConfig->graphics.parsed.presetPerformance > 0)
            mergedConfig.parsed.presetPerformance = g_pLocalConfig->graphics.parsed.presetPerformance;
        if (g_pLocalConfig->graphics.parsed.presetUltraPerformance > 0)
            mergedConfig.parsed.presetUltraPerformance = g_pLocalConfig->graphics.parsed.presetUltraPerformance;
        if (g_pLocalConfig->graphics.parsed.presetUltraQuality > 0)
            mergedConfig.parsed.presetUltraQuality = g_pLocalConfig->graphics.parsed.presetUltraQuality;

        if (g_pLocalConfig->graphics.parsed.rrPresetDLAA > 0)
            mergedConfig.parsed.rrPresetDLAA = g_pLocalConfig->graphics.parsed.rrPresetDLAA;
        if (g_pLocalConfig->graphics.parsed.rrPresetQuality > 0)
            mergedConfig.parsed.rrPresetQuality = g_pLocalConfig->graphics.parsed.rrPresetQuality;
        if (g_pLocalConfig->graphics.parsed.rrPresetBalanced > 0)
            mergedConfig.parsed.rrPresetBalanced = g_pLocalConfig->graphics.parsed.rrPresetBalanced;
        if (g_pLocalConfig->graphics.parsed.rrPresetPerformance > 0)
            mergedConfig.parsed.rrPresetPerformance = g_pLocalConfig->graphics.parsed.rrPresetPerformance;
        if (g_pLocalConfig->graphics.parsed.rrPresetUltraPerformance > 0)
            mergedConfig.parsed.rrPresetUltraPerformance = g_pLocalConfig->graphics.parsed.rrPresetUltraPerformance;
        if (g_pLocalConfig->graphics.parsed.rrPresetUltraQuality > 0)
            mergedConfig.parsed.rrPresetUltraQuality = g_pLocalConfig->graphics.parsed.rrPresetUltraQuality;

        // Apply Global Preset Overrides from g_pLocalConfig
        if (g_pLocalConfig->graphics.parsed.srPreset > 0)
            mergedConfig.parsed.srPreset = g_pLocalConfig->graphics.parsed.srPreset;
        if (g_pLocalConfig->graphics.parsed.rrPreset > 0)
            mergedConfig.parsed.rrPreset = g_pLocalConfig->graphics.parsed.rrPreset;

        if (g_pLocalConfig->graphics.parsed.dlssSharpening > -1.5f) {
            mergedConfig.parsed.dlssSharpening = g_pLocalConfig->graphics.parsed.dlssSharpening;
        }
        if (g_pLocalConfig->graphics.parsed.dlssFGFactor > 0) {
            mergedConfig.parsed.dlssFGFactor = g_pLocalConfig->graphics.parsed.dlssFGFactor;
        }
        if (g_pLocalConfig->graphics.parsed.fgPreset > 0) {
            mergedConfig.parsed.fgPreset = NormalizeDLSSFGPreset(g_pLocalConfig->graphics.parsed.fgPreset);
        }
        const auto& localParsed = g_pLocalConfig->graphics.parsed;
        if (localParsed.fgMode != kDlssFGModeDefault && IsDlssFGMode(localParsed.fgMode)) {
            mergedConfig.parsed.fgMode = localParsed.fgMode;
        }
        if (NormalizeDlssFGCount(localParsed.fgFixedCount) > 0) {
            mergedConfig.parsed.fgFixedCount = NormalizeDlssFGCount(localParsed.fgFixedCount);
        }
        if (NormalizeDlssFGCount(localParsed.fgDynamicMax) > 0) {
            mergedConfig.parsed.fgDynamicMax = NormalizeDlssFGCount(localParsed.fgDynamicMax);
        }
        if (NormalizeDlssFGTargetFps(localParsed.fgTargetFps) != kDlssFGTargetFpsDefault) {
            mergedConfig.parsed.fgTargetFps = NormalizeDlssFGTargetFps(localParsed.fgTargetFps);
        }
    }
    // Add other fields as needed

    // Update global performance gating flag
    const bool anyActive =
        (mergedConfig.vsyncMode != "default" && !mergedConfig.vsyncMode.empty()) ||
        (mergedConfig.anisotropicFiltering != "default" && !mergedConfig.anisotropicFiltering.empty()) ||
        (mergedConfig.mipMapping != "default" && !mergedConfig.mipMapping.empty()) ||
        (mergedConfig.mipBias != "default" && !mergedConfig.mipBias.empty()) || mergedConfig.forceMipBiasClamp ||
        (mergedConfig.msaaSamples != "default" && !mergedConfig.msaaSamples.empty()) ||
        mergedConfig.cpuPrerenderLimit > -0.5f || HasBackbufferCountOverride(mergedConfig.backbufferCount) ||
        mergedConfig.frameLatency > 0 || mergedConfig.sgssaa ||
        (mergedConfig.dlssAutoExposure != "default" && !mergedConfig.dlssAutoExposure.empty()) ||
        (mergedConfig.dlssExposureNormalization != "default" && !mergedConfig.dlssExposureNormalization.empty()) ||
        mergedConfig.parsed.presetDLAA > 0 || mergedConfig.parsed.presetQuality > 0 ||
        mergedConfig.parsed.rrPresetDLAA > 0 || mergedConfig.parsed.rrPresetQuality > 0 ||
        mergedConfig.parsed.srPreset > 0 || mergedConfig.parsed.rrPreset > 0 ||
        mergedConfig.parsed.dlssSharpening > -1.5f || mergedConfig.parsed.dlssFGFactor > 0 ||
        mergedConfig.parsed.fgPreset > 0 ||
        HasDlssFGDriverOverride(mergedConfig.parsed.fgMode, mergedConfig.parsed.fgFixedCount,
                                mergedConfig.parsed.fgDynamicMax, mergedConfig.parsed.fgTargetFps);

    g_GraphicsOverridesActive.store(anyActive, std::memory_order_release);

    // Keep the DLSS driver-settings answers tracking the resolved config,
    // including hot reloads. Arming the NvAPI resolution hook stays separate;
    // this only publishes the values the wrapper answers with.
    ce::ngx_drs::DlssDrsOverrides drsOverrides;
    drsOverrides.renderPreset = mergedConfig.parsed.fgPreset;
    drsOverrides.frameGenerationMode = mergedConfig.parsed.fgMode;
    drsOverrides.fixedCountMultiplier = mergedConfig.parsed.fgFixedCount;
    drsOverrides.dynamicMaxMultiplier = mergedConfig.parsed.fgDynamicMax;
    drsOverrides.dynamicTargetFps = mergedConfig.parsed.fgTargetFps;
    // `vsync_mode` reaches the DLSS-G runtime only through this key. CE's own
    // rewrite lands on the real dxgi Present, below Streamline's swapchain
    // proxy, so `shouldEnableVSync` never sees it as an application request -
    // and it consults the driver key first anyway. Resolved from the same
    // VSyncOverride the present path uses so one string has one meaning;
    // GetVSyncOverride() is not called here because it would re-enter this
    // function through GetActiveGraphicsConfig().
    const VSyncOverride mergedVSync = ResolveVSyncOverrideForMode(mergedConfig.vsyncMode);
    drsOverrides.vsyncMode = ce::ngx_drs::DrsVSyncModeForPresentOverride(
        mergedVSync.shouldOverride, mergedVSync.useMailbox, mergedVSync.presentInterval);
    ce::ngx_drs::SetConfiguredOverrides(drsOverrides);

    return mergedConfig;
}

const GraphicsConfig& GetActiveGraphicsConfigCached() {
    thread_local GraphicsConfig cachedConfig;
    thread_local uint32_t cachedVersion = 0xFFFFFFFFu;
    thread_local SharedMemoryLayout* cachedSharedMemory = nullptr;
    thread_local DWORD lastRefreshTick = 0;
    thread_local bool initialized = false;

    SharedMemoryLayout* currentSharedMemory = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    uint32_t currentVersion = 0;
    const bool hasSharedConfig = currentSharedMemory != nullptr;
    if (hasSharedConfig) {
        currentVersion = currentSharedMemory->configVersion.load(std::memory_order_acquire);
    }

    const DWORD now = GetTickCount();
    const bool localRefreshDue = !hasSharedConfig && now - lastRefreshTick >= 1000;
    if (!initialized || currentSharedMemory != cachedSharedMemory || currentVersion != cachedVersion ||
        localRefreshDue) {
        cachedConfig = GetActiveGraphicsConfig();
        cachedSharedMemory = currentSharedMemory;
        cachedVersion = currentVersion;
        lastRefreshTick = now;
        initialized = true;
    }
    return cachedConfig;
}

uint32_t GetActiveGraphicsConfigVersion() {
    if (g_IPC && g_IPC->GetSharedMem()) {
        return g_IPC->GetSharedMem()->configVersion.load(std::memory_order_acquire);
    }
    return 0;
}

float GetActivePrerenderLimit() {
    const auto& cfg = GetActiveGraphicsConfig();
    return cfg.cpuPrerenderLimit;
}
