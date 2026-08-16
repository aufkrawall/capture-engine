#include "inject_config.h"

#include "../common/config.h"
#include "../common/logging.h"
#include "../common/shared_defs.h"

#include <cstring>
#include <functional>
#include <string>

void UpdateSharedMemoryFromConfig(SharedMemoryLayout* sharedMemory, const AppConfig& config) {
    if (!sharedMemory)
        return;

    static uint64_t configSummaryHash = 0;

    sharedMemory->SetDebugLogging(IsDebugLoggingEnabled(config.logLevel));
    sharedMemory->SetLogLevel(config.logLevel);
    strncpy(sharedMemory->logFilePath, config.logFilePath.c_str(), sizeof(sharedMemory->logFilePath) - 1);
    sharedMemory->logFilePath[sizeof(sharedMemory->logFilePath) - 1] = '\0';

    auto& graphics = sharedMemory->graphicsConfig;
    strncpy(graphics.vsyncMode, config.graphics.vsyncMode.c_str(), 31);
    strncpy(graphics.anisotropicFiltering, config.graphics.anisotropicFiltering.c_str(), 31);
    strncpy(graphics.samplerOverrideMode, config.graphics.samplerOverrideMode.c_str(),
            sizeof(graphics.samplerOverrideMode) - 1);
    graphics.samplerOverrideMode[sizeof(graphics.samplerOverrideMode) - 1] = '\0';
    strncpy(graphics.mipMapping, config.graphics.mipMapping.c_str(), 31);
    strncpy(graphics.mipBias, config.graphics.mipBias.c_str(), 31);
    strncpy(graphics.mipBiasMode, config.graphics.mipBiasMode.c_str(), 31);
    graphics.forceMipBiasClamp = config.graphics.forceMipBiasClamp;
    strncpy(graphics.msaaSamples, config.graphics.msaaSamples.c_str(), 31);
    graphics.nvLodSpreadFix = config.graphics.nvLodSpreadFix;
    graphics.forceRayReconstruction = config.graphics.forceRayReconstruction;
    graphics.rayReconstructionOptimalSettings = config.graphics.rayReconstructionOptimalSettings;
    graphics.disablePostProcessingEffects = config.graphics.disablePostProcessingEffects;
    graphics.tonemapperSharpen = config.graphics.tonemapperSharpen;
    graphics.internalFpsLimit = config.graphics.internalFpsLimit;
    graphics.internalAnisotropicFiltering = config.graphics.internalAnisotropicFiltering;
    graphics.internalTextureMipBias = config.graphics.internalTextureMipBias;
    graphics.displayGamma = config.graphics.displayGamma;
    graphics.prerenderLimit = config.graphics.cpuPrerenderLimit;
    graphics.backbufferCount = config.graphics.backbufferCount;
    graphics.sgssaa = config.graphics.sgssaa;
    graphics.disableAutoMipBias = config.graphics.disableAutoMipBias;
    strncpy(graphics.dlssAutoExposure, config.graphics.dlssAutoExposure.c_str(), 31);
    strncpy(graphics.dlssExposureNormalization, config.graphics.dlssExposureNormalization.c_str(), 31);

    graphics.dlssPresetDLAA = ParseDlssPreset(config.graphics.dlssPresetDLAA);
    graphics.dlssPresetQuality = ParseDlssPreset(config.graphics.dlssPresetQuality);
    graphics.dlssPresetBalanced = ParseDlssPreset(config.graphics.dlssPresetBalanced);
    graphics.dlssPresetPerformance = ParseDlssPreset(config.graphics.dlssPresetPerformance);
    graphics.dlssPresetUltraPerformance = ParseDlssPreset(config.graphics.dlssPresetUltraPerformance);
    graphics.dlssPresetUltraQuality = ParseDlssPreset(config.graphics.dlssPresetUltraQuality);

    graphics.dlssRRPresetDLAA = ParseDlssRRPreset(config.graphics.dlssRRPresetDLAA);
    graphics.dlssRRPresetQuality = ParseDlssRRPreset(config.graphics.dlssRRPresetQuality);
    graphics.dlssRRPresetBalanced = ParseDlssRRPreset(config.graphics.dlssRRPresetBalanced);
    graphics.dlssRRPresetPerformance = ParseDlssRRPreset(config.graphics.dlssRRPresetPerformance);
    graphics.dlssRRPresetUltraPerformance = ParseDlssRRPreset(config.graphics.dlssRRPresetUltraPerformance);
    graphics.dlssRRPresetUltraQuality = ParseDlssRRPreset(config.graphics.dlssRRPresetUltraQuality);

    graphics.dlssSRPreset = ParseDlssPreset(config.graphics.dlssSRPreset);
    graphics.dlssRRPreset = ParseDlssRRPreset(config.graphics.dlssRRPreset);
    graphics.dlssSharpening = ParseDlssSharpening(config.graphics.dlssSharpening);
    graphics.dlssFGFactor = config.graphics.parsed.dlssFGFactor;
    graphics.dlssFGPreset = NormalizeDLSSFGPreset(config.graphics.parsed.fgPreset);
    sharedMemory->configVersion.fetch_add(1, std::memory_order_release);

    sharedMemory->BeginWriteOverlayConfig();
    sharedMemory->overlayConfig = config.overlay;
    sharedMemory->EndWriteOverlayConfig();

    sharedMemory->SetGpuPriority(config.video.gpuPriority);
    if (config.copyQueuePriority == "low")
        sharedMemory->SetCopyQueuePriority(0);
    else if (config.copyQueuePriority == "high")
        sharedMemory->SetCopyQueuePriority(2);
    else
        sharedMemory->SetCopyQueuePriority(1);
    sharedMemory->SetFenceWaitMode(config.fenceWaitMode);
    sharedMemory->SetUseGameQueue(config.useGameQueue);

    sharedMemory->fpsLimiter.SetCaptureSyncEnabled(config.fpsLimiter.captureSyncEnabled);
    sharedMemory->fpsLimiter.SetCaptureSyncMultiplier(config.fpsLimiter.captureSyncMultiplier);
    sharedMemory->fpsLimiter.SetCaptureSyncLimiterMode(
        static_cast<uint32_t>(config.fpsLimiter.captureSyncLimiterMode));
    sharedMemory->fpsLimiter.SetGeneralEnabled(config.fpsLimiter.generalEnabled);
    sharedMemory->fpsLimiter.SetGeneralFps(config.fpsLimiter.generalFps);
    sharedMemory->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(config.fpsLimiter.generalLimiterMode));
    sharedMemory->fpsLimiter.SetCaptureFps(config.video.fps);
    sharedMemory->fpsLimiter.SetUseVFR(config.video.useVFR);

    const uint64_t summaryHash =
        std::hash<std::string>{}(std::string(graphics.vsyncMode)) ^
        (std::hash<std::string>{}(std::string(graphics.anisotropicFiltering)) << 1) ^
        (std::hash<std::string>{}(std::string(graphics.samplerOverrideMode)) << 2) ^
        (std::hash<std::string>{}(std::string(graphics.mipBias)) << 3) ^
        (static_cast<uint64_t>(graphics.backbufferCount) << 3) ^
        (static_cast<uint64_t>(sharedMemory->fpsLimiter.GetGeneralFps()) << 4) ^
        (static_cast<uint64_t>(sharedMemory->fpsLimiter.GetGeneralEnabled()) << 5) ^
        (static_cast<uint64_t>(graphics.dlssSRPreset) << 6) ^
        (static_cast<uint64_t>(config.logLevel) << 7) ^
        (static_cast<uint64_t>(sharedMemory->overlayConfig.observerPolicyOnly) << 8) ^
        (static_cast<uint64_t>(sharedMemory->overlayConfig.observerStartupPresentOnly) << 9) ^
        (static_cast<uint64_t>(graphics.forceRayReconstruction) << 10) ^
        (static_cast<uint64_t>(graphics.rayReconstructionOptimalSettings) << 11) ^
        (static_cast<uint64_t>(graphics.disablePostProcessingEffects) << 12) ^
        (std::hash<float>{}(graphics.tonemapperSharpen) << 13) ^
        (std::hash<float>{}(graphics.internalFpsLimit) << 14) ^
        (static_cast<uint64_t>(graphics.internalAnisotropicFiltering) << 15) ^
        (std::hash<float>{}(graphics.internalTextureMipBias) << 16) ^
        (std::hash<float>{}(graphics.displayGamma) << 17);

    if (summaryHash != configSummaryHash) {
        LogInfo(
            "[Inject] SharedMem config updated: logLevel=%s vsync=%s af=%s mipBias=%s mode=%s cpuPrerender=%.2f "
            "backBuffer=%d fpsLimit=%d(%s) overlayEnabled=%d observerOnly=%d observerPolicyOnly=%d "
            "observerStartupPresentOnly=%d captureOverlay=%d screenshotOverlay=%d "
            "dlssAutoExp=%s sharpen=%.2f srPreset=%u forceRR=%d ue5RROptimal=%d "
            "ue5DisablePost=%d ue5Sharpen=%.2f ue5InternalFpsLimit=%.2f ue5InternalAF=%d "
            "ue5InternalTextureMipBias=%.2f ue5DisplayGamma=%.2f",
            LogLevelToConfigString(config.logLevel), graphics.vsyncMode, graphics.anisotropicFiltering,
            graphics.mipBias, graphics.mipBiasMode, graphics.prerenderLimit, graphics.backbufferCount,
            sharedMemory->fpsLimiter.GetGeneralFps(),
            sharedMemory->fpsLimiter.GetGeneralEnabled() ? "ON" : "OFF", sharedMemory->overlayConfig.showOverlay,
            sharedMemory->overlayConfig.observerOnly, sharedMemory->overlayConfig.observerPolicyOnly,
            sharedMemory->overlayConfig.observerStartupPresentOnly,
            sharedMemory->overlayConfig.captureIncludeOverlay,
            sharedMemory->overlayConfig.screenshotIncludeOverlay, graphics.dlssAutoExposure,
            graphics.dlssSharpening, graphics.dlssSRPreset, graphics.forceRayReconstruction ? 1 : 0,
            graphics.rayReconstructionOptimalSettings ? 1 : 0,
            graphics.disablePostProcessingEffects ? 1 : 0, graphics.tonemapperSharpen,
            graphics.internalFpsLimit, graphics.internalAnisotropicFiltering,
            graphics.internalTextureMipBias, graphics.displayGamma);
        configSummaryHash = summaryHash;
    }
}
