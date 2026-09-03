#include "inject_config.h"

#include "../common/config.h"
#include "../common/logging.h"
#include "../common/shared_defs.h"

#include <cstring>
#include <filesystem>
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

    sharedMemory->benchmark.startDelaySeconds.store(config.benchmark.startDelaySeconds, std::memory_order_release);
    sharedMemory->benchmark.durationSeconds.store(config.benchmark.durationSeconds, std::memory_order_release);

    std::string resolvedOutputDir = config.benchmark.outputDir;
    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH) > 0) {
        std::string baseDir = exePath;
        const size_t slash = baseDir.find_last_of("\\/");
        if (slash != std::string::npos) {
            baseDir = baseDir.substr(0, slash);
            if (resolvedOutputDir.empty()) {
                resolvedOutputDir = baseDir + "\\benchmarks";
            } else {
                std::filesystem::path configuredPath(resolvedOutputDir);
                if (configuredPath.is_relative()) {
                    resolvedOutputDir = (std::filesystem::path(baseDir) / configuredPath).string();
                }
            }
        }
    }
    if (resolvedOutputDir.empty()) {
        resolvedOutputDir = "benchmarks";
    }
    strncpy(sharedMemory->benchmark.outputDir, resolvedOutputDir.c_str(),
            sizeof(sharedMemory->benchmark.outputDir) - 1);
    sharedMemory->benchmark.outputDir[sizeof(sharedMemory->benchmark.outputDir) - 1] = '\0';

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
    graphics.depthOfField = config.graphics.depthOfField;
    graphics.dlssSuperResolution = config.graphics.dlssSuperResolution;
    graphics.dlssScreenPercentage = config.graphics.dlssScreenPercentage;
    graphics.hdrOutput = config.graphics.hdrOutput;
    graphics.hdrPeakLuminance = config.graphics.hdrPeakLuminance;
    graphics.hdrPaperWhite = config.graphics.hdrPaperWhite;
    graphics.hdrUiLuminance = config.graphics.hdrUiLuminance;
    graphics.hdrMinLuminance = config.graphics.hdrMinLuminance;
    graphics.hdrColorGamut = config.graphics.hdrColorGamut;
    graphics.ue5CustomCVarOverrideMask = config.graphics.ue5CustomCVarOverrideMask;
    memcpy(graphics.ue5CustomCVarOverrideValues, config.graphics.ue5CustomCVarOverrideValues.data(),
           sizeof(graphics.ue5CustomCVarOverrideValues));
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
    strncpy(graphics.dlssSrDllPath, config.graphics.dlssSrDllPath.c_str(), sizeof(graphics.dlssSrDllPath) - 1);
    graphics.dlssSrDllPath[sizeof(graphics.dlssSrDllPath) - 1] = '\0';
    strncpy(graphics.dlssRrDllPath, config.graphics.dlssRrDllPath.c_str(), sizeof(graphics.dlssRrDllPath) - 1);
    graphics.dlssRrDllPath[sizeof(graphics.dlssRrDllPath) - 1] = '\0';
    strncpy(graphics.dlssFgDllPath, config.graphics.dlssFgDllPath.c_str(), sizeof(graphics.dlssFgDllPath) - 1);
    graphics.dlssFgDllPath[sizeof(graphics.dlssFgDllPath) - 1] = '\0';
    strncpy(graphics.streamlineDllPath, config.graphics.streamlineDllPath.c_str(),
            sizeof(graphics.streamlineDllPath) - 1);
    graphics.streamlineDllPath[sizeof(graphics.streamlineDllPath) - 1] = '\0';
    strncpy(graphics.dlssDebugOverlay, config.graphics.dlssDebugOverlay.c_str(),
            sizeof(graphics.dlssDebugOverlay) - 1);
    graphics.dlssDebugOverlay[sizeof(graphics.dlssDebugOverlay) - 1] = '\0';
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
        // The publication summary is the only record that a setting reached the
        // hook. Leaving the overlay booleans out of the hash made a live
        // screenshot_include_overlay change publish completely silently.
        (static_cast<uint64_t>(sharedMemory->overlayConfig.showOverlay) << 18) ^
        (static_cast<uint64_t>(sharedMemory->overlayConfig.observerOnly) << 19) ^
        (static_cast<uint64_t>(sharedMemory->overlayConfig.captureIncludeOverlay) << 20) ^
        (static_cast<uint64_t>(sharedMemory->overlayConfig.screenshotIncludeOverlay) << 21) ^
        (static_cast<uint64_t>(sharedMemory->overlayConfig.frameTimeSource) << 32) ^
        (static_cast<uint64_t>(sharedMemory->overlayConfig.showSystemLatency) << 33) ^
        (static_cast<uint64_t>(graphics.forceRayReconstruction) << 10) ^
        (static_cast<uint64_t>(graphics.rayReconstructionOptimalSettings) << 11) ^
        (static_cast<uint64_t>(graphics.disablePostProcessingEffects) << 12) ^
        (std::hash<float>{}(graphics.tonemapperSharpen) << 13) ^
        (std::hash<float>{}(graphics.internalFpsLimit) << 14) ^
        (static_cast<uint64_t>(graphics.internalAnisotropicFiltering) << 15) ^
        (std::hash<float>{}(graphics.internalTextureMipBias) << 16) ^
        (std::hash<float>{}(graphics.displayGamma) << 17) ^
        (static_cast<uint64_t>(graphics.depthOfField) << 22) ^
        (static_cast<uint64_t>(graphics.dlssSuperResolution) << 23) ^
        (std::hash<float>{}(graphics.dlssScreenPercentage) << 24) ^
        (static_cast<uint64_t>(graphics.hdrOutput) << 25) ^
        (static_cast<uint64_t>(graphics.hdrPeakLuminance) << 26) ^
        (std::hash<float>{}(graphics.hdrPaperWhite) << 27) ^
        (std::hash<float>{}(graphics.hdrUiLuminance) << 28) ^
        (std::hash<float>{}(graphics.hdrMinLuminance) << 29) ^
        (static_cast<uint64_t>(graphics.hdrColorGamut) << 30) ^
        std::hash<std::string>{}(graphics.dlssSrDllPath) ^
        std::hash<std::string>{}(graphics.dlssRrDllPath) ^
        std::hash<std::string>{}(graphics.dlssFgDllPath) ^
        std::hash<std::string>{}(graphics.streamlineDllPath) ^
        std::hash<std::string>{}(graphics.dlssDebugOverlay) ^
        (static_cast<uint64_t>(graphics.dlssFGPreset) << 31) ^
        graphics.ue5CustomCVarOverrideMask;

    uint64_t completeSummaryHash = summaryHash;
    for (std::size_t index = 0; index < UE5_CVAR_OVERRIDE_CAPACITY; ++index) {
        if (graphics.ue5CustomCVarOverrideMask & (uint64_t{1} << index))
            completeSummaryHash = (completeSummaryHash * 1099511628211ull) ^
                                  graphics.ue5CustomCVarOverrideValues[index];
    }

    if (completeSummaryHash != configSummaryHash) {
        LogInfo(
            "[Inject] SharedMem config updated: logLevel=%s vsync=%s af=%s mipBias=%s mode=%s cpuPrerender=%.2f "
            "backBuffer=%d fpsLimit=%d(%s) overlayEnabled=%d observerOnly=%d observerPolicyOnly=%d "
            "observerStartupPresentOnly=%d captureOverlay=%d screenshotOverlay=%d frameTiming=%s systemLatency=%d "
            "dlssAutoExp=%s sharpen=%.2f srPreset=%u rrPreset=%u fgPreset=%u indicator=%s "
            "runtimePaths=%d%d%d%d forceRR=%d ue5RROptimal=%d "
            "ue5DisablePost=%d ue5Sharpen=%.2f ue5InternalFpsLimit=%.2f ue5InternalAF=%d "
            "ue5InternalTextureMipBias=%.2f ue5DisplayGamma=%.2f ue5DepthOfField=%d ue5DlssSR=%d "
            "ue5DlssScreenPercentage=%.2f ue5HdrOutput=%d ue5HdrPeak=%d ue5HdrPaperWhite=%.1f "
            "ue5HdrUiLuminance=%.1f ue5HdrMinLuminance=%.4f ue5HdrColorGamut=%d "
            "ue5CustomMask=0x%016llX",
            LogLevelToConfigString(config.logLevel), graphics.vsyncMode, graphics.anisotropicFiltering,
            graphics.mipBias, graphics.mipBiasMode, graphics.prerenderLimit, graphics.backbufferCount,
            sharedMemory->fpsLimiter.GetGeneralFps(),
            sharedMemory->fpsLimiter.GetGeneralEnabled() ? "ON" : "OFF", sharedMemory->overlayConfig.showOverlay,
            sharedMemory->overlayConfig.observerOnly, sharedMemory->overlayConfig.observerPolicyOnly,
            sharedMemory->overlayConfig.observerStartupPresentOnly,
            sharedMemory->overlayConfig.captureIncludeOverlay,
            sharedMemory->overlayConfig.screenshotIncludeOverlay,
            sharedMemory->overlayConfig.frameTimeSource == FrameTimeSource::DisplayChange ? "display-change"
                                                                                           : "presentation",
            sharedMemory->overlayConfig.showSystemLatency,
            graphics.dlssAutoExposure,
            graphics.dlssSharpening, graphics.dlssSRPreset, graphics.dlssRRPreset,
            graphics.dlssFGPreset, graphics.dlssDebugOverlay,
            graphics.dlssSrDllPath[0] ? 1 : 0, graphics.dlssRrDllPath[0] ? 1 : 0,
            graphics.dlssFgDllPath[0] ? 1 : 0, graphics.streamlineDllPath[0] ? 1 : 0,
            graphics.forceRayReconstruction ? 1 : 0,
            static_cast<int>(graphics.rayReconstructionOptimalSettings),
            graphics.disablePostProcessingEffects ? 1 : 0, graphics.tonemapperSharpen,
            graphics.internalFpsLimit, graphics.internalAnisotropicFiltering,
            graphics.internalTextureMipBias, graphics.displayGamma, graphics.depthOfField,
            graphics.dlssSuperResolution, graphics.dlssScreenPercentage, graphics.hdrOutput,
            graphics.hdrPeakLuminance, graphics.hdrPaperWhite, graphics.hdrUiLuminance,
            graphics.hdrMinLuminance, graphics.hdrColorGamut,
            static_cast<unsigned long long>(graphics.ue5CustomCVarOverrideMask));
        configSummaryHash = completeSummaryHash;
    }
}
