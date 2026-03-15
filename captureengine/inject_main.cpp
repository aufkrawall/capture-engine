// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include "../common/config.h"
#include "../common/crash_handler.h"
#include "../common/logging.h"
#include "../common/process_ipc.h"
#include "../common/shared_defs.h"
#include "host_metrics.h"
#include "injection.h"

namespace fs = std::filesystem;

static std::atomic<bool> g_Running{true};

// Console control handler for graceful cleanup
BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        LogInfo("[Inject] Console interrupt received, shutting down...");
        g_Running = false;
        return TRUE;
    }
    return FALSE;
}

// Helper: Get process name from PID
std::string GetProcessNameFromPID(DWORD pid) {
    char buffer[MAX_PATH] = "unknown";
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (hProcess) {
        if (GetModuleBaseNameA(hProcess, NULL, buffer, MAX_PATH)) {
            // Success
        }
        CloseHandle(hProcess);
    }
    // Strip path if present
    std::string name = buffer;
    size_t lastSlash = name.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        name = name.substr(lastSlash + 1);
    }
    return name;
}

struct InjectorConfigState {
    AppConfig config;
    bool allowInjection = false;
};

static InjectorConfigState BuildInjectorConfigState(const AppConfig& config) {
    const bool screenGrabMode = (config.captureMethod == "screengrab" || config.captureMethod == "framegrab" ||
                                 config.captureMethod == "desktop_dup");
    const bool overlayOnlyInjection = screenGrabMode && !config.overlayWhitelist.empty();

    InjectorConfigState state;
    state.config = config;
    state.allowInjection = !screenGrabMode || overlayOnlyInjection;

    if (!state.allowInjection) {
        state.config.gameWhitelist.clear();
        state.config.overlayWhitelist.clear();
    } else if (overlayOnlyInjection) {
        state.config.gameWhitelist.clear();
    }

    return state;
}

static bool ShouldRescanForConfigChange(const AppConfig& oldBaseConfig, const InjectorConfigState& oldState,
                                        const AppConfig& newBaseConfig, const InjectorConfigState& newState) {
    return oldState.allowInjection != newState.allowInjection ||
           oldBaseConfig.debugLogging != newBaseConfig.debugLogging ||
           oldState.config.gameWhitelist != newState.config.gameWhitelist ||
           oldState.config.overlayWhitelist != newState.config.overlayWhitelist;
}

static AppConfig ResolveActiveTargetConfig(const std::string& configPath, SharedMemoryLayout* pSharedMem,
                                           const AppConfig& baseConfig) {
    AppConfig activeConfig = baseConfig;
    if (!pSharedMem) {
        return activeConfig;
    }

    const uint32_t sourcePid = pSharedMem->GetSourcePid();
    if (sourcePid == 0) {
        return activeConfig;
    }

    const std::string processName = GetProcessNameFromPID(sourcePid);
    if (!processName.empty() && processName != "unknown") {
        LoadConfig(configPath, activeConfig, processName);
    }

    return activeConfig;
}

// Helper: Update shared memory from config
static void UpdateSharedMemoryFromConfig(SharedMemoryLayout* pSharedMem, const AppConfig& config) {
    if (!pSharedMem)
        return;

    pSharedMem->SetDebugLogging(config.debugLogging);
    pSharedMem->SetLogLevel(LogLevel::Info);
    strncpy(pSharedMem->logFilePath, config.logFilePath.c_str(), sizeof(pSharedMem->logFilePath) - 1);
    pSharedMem->logFilePath[sizeof(pSharedMem->logFilePath) - 1] = '\0';

    // Graphics
    strncpy(pSharedMem->graphicsConfig.vsyncMode, config.graphics.vsyncMode.c_str(), 31);
    strncpy(pSharedMem->graphicsConfig.anisotropicFiltering, config.graphics.anisotropicFiltering.c_str(), 31);
    strncpy(pSharedMem->graphicsConfig.mipMapping, config.graphics.mipMapping.c_str(), 31);
    strncpy(pSharedMem->graphicsConfig.mipBias, config.graphics.mipBias.c_str(), 31);
    strncpy(pSharedMem->graphicsConfig.mipBiasMode, config.graphics.mipBiasMode.c_str(), 31);
    pSharedMem->graphicsConfig.forceMipBiasClamp = config.graphics.forceMipBiasClamp;
    strncpy(pSharedMem->graphicsConfig.msaaSamples, config.graphics.msaaSamples.c_str(), 31);
    pSharedMem->graphicsConfig.prerenderLimit = config.graphics.cpuPrerenderLimit;
    pSharedMem->graphicsConfig.backbufferCount = config.graphics.backbufferCount;
    pSharedMem->graphicsConfig.sgssaa = config.graphics.sgssaa;
    pSharedMem->graphicsConfig.disableAutoMipBias = config.graphics.disableAutoMipBias;
    strncpy(pSharedMem->graphicsConfig.dlssAutoExposure, config.graphics.dlssAutoExposure.c_str(), 31);
    strncpy(pSharedMem->graphicsConfig.dlssExposureNormalization, config.graphics.dlssExposureNormalization.c_str(),
            31);

    // DLSS Presets
    pSharedMem->graphicsConfig.dlssPresetDLAA = ParseDlssPreset(config.graphics.dlssPresetDLAA);
    pSharedMem->graphicsConfig.dlssPresetQuality = ParseDlssPreset(config.graphics.dlssPresetQuality);
    pSharedMem->graphicsConfig.dlssPresetBalanced = ParseDlssPreset(config.graphics.dlssPresetBalanced);
    pSharedMem->graphicsConfig.dlssPresetPerformance = ParseDlssPreset(config.graphics.dlssPresetPerformance);
    pSharedMem->graphicsConfig.dlssPresetUltraPerformance = ParseDlssPreset(config.graphics.dlssPresetUltraPerformance);
    pSharedMem->graphicsConfig.dlssPresetUltraQuality = ParseDlssPreset(config.graphics.dlssPresetUltraQuality);

    // RR Presets
    pSharedMem->graphicsConfig.dlssRRPresetDLAA = ParseDlssRRPreset(config.graphics.dlssRRPresetDLAA);
    pSharedMem->graphicsConfig.dlssRRPresetQuality = ParseDlssRRPreset(config.graphics.dlssRRPresetQuality);
    pSharedMem->graphicsConfig.dlssRRPresetBalanced = ParseDlssRRPreset(config.graphics.dlssRRPresetBalanced);
    pSharedMem->graphicsConfig.dlssRRPresetPerformance = ParseDlssRRPreset(config.graphics.dlssRRPresetPerformance);
    pSharedMem->graphicsConfig.dlssRRPresetUltraPerformance =
        ParseDlssRRPreset(config.graphics.dlssRRPresetUltraPerformance);
    pSharedMem->graphicsConfig.dlssRRPresetUltraQuality = ParseDlssRRPreset(config.graphics.dlssRRPresetUltraQuality);

    pSharedMem->graphicsConfig.dlssSRPreset = ParseDlssPreset(config.graphics.dlssSRPreset);
    pSharedMem->graphicsConfig.dlssRRPreset = ParseDlssRRPreset(config.graphics.dlssRRPreset);

    pSharedMem->graphicsConfig.dlssSharpening = ParseDlssSharpening(config.graphics.dlssSharpening);
    pSharedMem->graphicsConfig.dlssFGFactor = config.graphics.parsed.dlssFGFactor;

    pSharedMem->configVersion.fetch_add(1, std::memory_order_release);

    // Overlay — use seqlock to prevent torn reads by the hook
    pSharedMem->BeginWriteOverlayConfig();
    pSharedMem->overlayConfig = config.overlay;
    pSharedMem->EndWriteOverlayConfig();

    // Performance / Priority
    pSharedMem->SetGpuPriority(config.video.gpuPriority);
    if (config.copyQueuePriority == "low")
        pSharedMem->SetCopyQueuePriority(0);
    else if (config.copyQueuePriority == "high")
        pSharedMem->SetCopyQueuePriority(2);
    else
        pSharedMem->SetCopyQueuePriority(1);

    // Synchronization
    pSharedMem->SetFenceWaitMode(config.fenceWaitMode);
    pSharedMem->SetUseGameQueue(config.useGameQueue);

    // FPS Limiter
    pSharedMem->fpsLimiter.SetCaptureSyncEnabled(config.fpsLimiter.captureSyncEnabled);
    pSharedMem->fpsLimiter.SetCaptureSyncMultiplier(config.fpsLimiter.captureSyncMultiplier);
    pSharedMem->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(config.fpsLimiter.captureSyncLimiterMode));
    pSharedMem->fpsLimiter.SetGeneralEnabled(config.fpsLimiter.generalEnabled);
    pSharedMem->fpsLimiter.SetGeneralFps(config.fpsLimiter.generalFps);
    pSharedMem->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(config.fpsLimiter.generalLimiterMode));
    // Note: captureFps is usually set dynamically during recording start,
    // but we can update it here if it's based on config.video.fps
    pSharedMem->fpsLimiter.SetCaptureFps(config.video.fps);
    pSharedMem->fpsLimiter.SetUseVFR(config.video.useVFR);

    LogInfo(
        "[Inject] Updated SharedMem Config: VSync=%s, AF=%s, MipBias=%s "
        "(Mode=%s), CPUPrerender=%.2f, BackBuffer=%d, FPS Limit=%d (%s), "
        "CaptureOverlay=%d, DLSS[AutoExp=%s Sharpening=%.2f SRPreset=%u]",
        pSharedMem->graphicsConfig.vsyncMode, pSharedMem->graphicsConfig.anisotropicFiltering,
        pSharedMem->graphicsConfig.mipBias, pSharedMem->graphicsConfig.mipBiasMode,
        pSharedMem->graphicsConfig.prerenderLimit, pSharedMem->graphicsConfig.backbufferCount,
        pSharedMem->fpsLimiter.GetGeneralFps(), pSharedMem->fpsLimiter.GetGeneralEnabled() ? "ON" : "OFF",
        pSharedMem->overlayConfig.captureIncludeOverlay, pSharedMem->graphicsConfig.dlssAutoExposure,
        pSharedMem->graphicsConfig.dlssSharpening, pSharedMem->graphicsConfig.dlssSRPreset);
}

static void PopulateWhitelistCache(DiscoveryInfo* pDisc, const AppConfig& config) {
    if (!pDisc)
        return;
    memset(pDisc->processWhitelist, 0, sizeof(pDisc->processWhitelist));

    char* p = pDisc->processWhitelist;
    char* end = pDisc->processWhitelist + sizeof(pDisc->processWhitelist) - 2;  // -2 for double null

    auto addName = [&](const std::string& name) {
        if (name.empty())
            return;
        size_t len = name.length();
        if (p + len + 1 < end) {
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            memcpy(p, lower.c_str(), len);
            p += len;
            *p++ = '\0';
        }
    };

    for (const auto& entry : config.gameWhitelist) {
        addName(entry.pattern);
        LogInfo("[Inject] Added game to whitelist cache: %s", entry.pattern.c_str());
    }
    for (const auto& entry : config.overlayWhitelist) {
        addName(entry.pattern);
        LogInfo("[Inject] Added overlay target to whitelist cache: %s", entry.pattern.c_str());
    }

    // Always whitelist our test processes too
    if (config.debugLogging) {
        addName("dx12_test.exe");
        addName("dx11_test.exe");
        addName("vulkan_test.exe");
    }

    *p = '\0';  // Double null terminator
}

int InjectProcessMain(const AppConfig& config) {
    // Install crash handler for this process
    InstallCrashHandler();

    // Register console control handler
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    AppConfig currentConfig = config;
    InjectorConfigState injectorState = BuildInjectorConfigState(currentConfig);

    // Setup IPC server
    ProcessIPCServer ipc(ProcessMode::Inject);
    if (!ipc.Init()) {
        LogError("[Inject] Failed to initialize IPC");
        return 1;
    }

    // Deduce config path (same logic as main.cpp)
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    std::string exePath = buffer;
    std::string baseDir = exePath.substr(0, exePath.find_last_of("\\/"));
    std::string configPath = baseDir + "\\config.ini";

    HMODULE hHookDll = NULL;

    // Create shared memory with unique name based on our PID
    wchar_t sharedMemName[64];
    GenerateSharedMemName(sharedMemName, 64, GetCurrentProcessId());

    HANDLE hMapFile =
        CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(SharedMemoryLayout), sharedMemName);

    if (hMapFile == NULL) {
        LogError("[Inject] Failed to create shared memory: %lu", GetLastError());
        return 1;
    }

    LogInfo("[Inject] Created shared memory: %ls", sharedMemName);

    SharedMemoryLayout* pSharedMem =
        (SharedMemoryLayout*)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedMemoryLayout));

    if (pSharedMem == NULL) {
        LogError("[Inject] Failed to map shared memory");
        CloseHandle(hMapFile);
        return 1;
    }

    // Discovery shared memory
    HANDLE hDiscoveryFile =
        CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(DiscoveryInfo), SHARED_MEM_DISCOVERY);
    DiscoveryInfo* pDiscovery = nullptr;
    if (hDiscoveryFile) {
        pDiscovery = (DiscoveryInfo*)MapViewOfFile(hDiscoveryFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DiscoveryInfo));
        if (pDiscovery) {
            pDiscovery->injectPid = GetCurrentProcessId();
            pDiscovery->magic = DISCOVERY_MAGIC;
            PopulateWhitelistCache(pDiscovery, currentConfig);
            // Set logs path for Vulkan layer to use
            std::string logsDir = baseDir + "\\logs";
            strncpy(pDiscovery->logsPath, logsDir.c_str(), sizeof(pDiscovery->logsPath) - 1);
            pDiscovery->logsPath[sizeof(pDiscovery->logsPath) - 1] = '\0';
            LogInfo("[Inject] Created discovery memory with whitelist cache");
        }
    }

    // Initialize shared memory
    ZeroMemory(pSharedMem, sizeof(SharedMemoryLayout));
    pSharedMem->SetHostPID(GetCurrentProcessId());
    pSharedMem->SetDebugLogging(currentConfig.debugLogging);
    pSharedMem->SetLogLevel(LogLevel::Info);

    // Copy log path
    std::string logPath = currentConfig.logFilePath;
    strncpy(pSharedMem->logFilePath, logPath.c_str(), sizeof(pSharedMem->logFilePath) - 1);

    // Copy priority settings
    pSharedMem->SetGpuPriority(currentConfig.video.gpuPriority);
    if (currentConfig.copyQueuePriority == "low")
        pSharedMem->SetCopyQueuePriority(0);
    else if (currentConfig.copyQueuePriority == "high")
        pSharedMem->SetCopyQueuePriority(2);
    else
        pSharedMem->SetCopyQueuePriority(1);

    pSharedMem->SetFenceWaitMode(currentConfig.fenceWaitMode);
    pSharedMem->SetUseGameQueue(currentConfig.useGameQueue);

    // Create separate Shmem mapping for large buffer
    wchar_t shmemName[64];
    GenerateShmemName(shmemName, 64, GetCurrentProcessId());

    // Host always creates mapping large enough for 4K
    size_t maxMappingSize = ShmemBuffer::CalculateSize(3840, 2160);

    HANDLE hMapShmem = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, maxMappingSize, shmemName);

    ShmemBuffer* pShmem = nullptr;
    if (hMapShmem) {
        pShmem = (ShmemBuffer*)MapViewOfFile(hMapShmem, FILE_MAP_ALL_ACCESS, 0, 0, maxMappingSize);
        if (pShmem) {
            new (pShmem) ShmemBuffer();  // Properly construct std::atomic members
            pShmem->max_width = 3840;
            pShmem->max_height = 2160;
            pShmem->slot_size = 3840 * 2160 * 4;
            pSharedMem->SetShmemMappingCreated(true);
            pSharedMem->SetShmemMappingSize(maxMappingSize);
            LogInfo("[Inject] Created separate Shmem mapping: %ls", shmemName);
        }
    }

    // Copy FPS limiter settings
    pSharedMem->fpsLimiter.SetCaptureSyncEnabled(currentConfig.fpsLimiter.captureSyncEnabled);
    pSharedMem->fpsLimiter.SetCaptureSyncMultiplier(currentConfig.fpsLimiter.captureSyncMultiplier);
    pSharedMem->fpsLimiter.SetCaptureSyncLimiterMode(
        static_cast<uint32_t>(currentConfig.fpsLimiter.captureSyncLimiterMode));
    pSharedMem->fpsLimiter.SetGeneralEnabled(currentConfig.fpsLimiter.generalEnabled);
    pSharedMem->fpsLimiter.SetGeneralFps(currentConfig.fpsLimiter.generalFps);
    pSharedMem->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(currentConfig.fpsLimiter.generalLimiterMode));
    pSharedMem->fpsLimiter.SetCaptureFps(currentConfig.video.fps);

    // Copy overlay config (seqlock for consistency with hook readers)
    pSharedMem->BeginWriteOverlayConfig();
    pSharedMem->overlayConfig = currentConfig.overlay;
    pSharedMem->EndWriteOverlayConfig();

    // Copy graphics overrides
    UpdateSharedMemoryFromConfig(pSharedMem, currentConfig);

    // Create FPS limiter events (named for cross-process access)
    wchar_t releaseEventName[64];
    wchar_t requestEventName[64];
    swprintf(releaseEventName, 64, L"Local\\CE_LR_%08X", GetCurrentProcessId());
    swprintf(requestEventName, 64, L"Local\\CE_LQ_%08X", GetCurrentProcessId());
    // SECURITY FIX: Use wcscpy_s instead of wcscpy to prevent buffer overflow
    wcscpy_s(pSharedMem->fpsLimiter.releaseEventName, 64, releaseEventName);
    wcscpy_s(pSharedMem->fpsLimiter.requestEventName, 64, requestEventName);

    HANDLE hLimiterReleaseEvent = CreateEventW(NULL, FALSE, FALSE, releaseEventName);  // Auto-reset
    HANDLE hLimiterRequestEvent = CreateEventW(NULL, FALSE, FALSE, requestEventName);  // Auto-reset

    if (!hLimiterReleaseEvent || !hLimiterRequestEvent) {
        LogError("[Inject] Failed to create limiter events: %lu", GetLastError());
    } else {
        LogInfo("[Inject] Created limiter events");
    }

    LogInfo("[Inject] Shared memory created and initialized");

    // Initialize injector (WMI based)
    // In screengrab/desktop_dup mode we still allow explicit overlay targets, but
    // restrict WMI injection to overlay_whitelist entries only.
    if (injectorState.allowInjection && injectorState.config.gameWhitelist.empty() &&
        !currentConfig.gameWhitelist.empty()) {
        LogInfo("[Inject] Screengrab mode + overlay_whitelist: enabling overlay-only injection");
    }
    std::shared_ptr<InjectionManager> injector;
    auto configureInjector = [&](const std::shared_ptr<InjectionManager>& manager) {
        if (!manager) {
            return;
        }

        manager->SetOnInjectCallback([&](const std::string& processName) {
            LogInfo("[Inject] Reloading config for target: %s", processName.c_str());

            // Load fresh config with process-specific overrides
            AppConfig targetConfig;
            LoadConfig(configPath, targetConfig, processName);

            // Update Shared Memory with new values
            UpdateSharedMemoryFromConfig(pSharedMem, targetConfig);
        });
    };

    if (injectorState.allowInjection) {
        // CRITICAL: Must use make_shared because InjectionManager inherits
        // enable_shared_from_this. Stack allocation causes shared_from_this() to
        // throw bad_weak_ptr in WMI callback, which silently prevents ALL process
        // injection.
        const int64_t injectorInitStartUs = Log_GetQpcUs();
        injector = std::make_shared<InjectionManager>(injectorState.config);
        LogInfo("[StartupPerf] Injection manager construction took %.3f ms",
                static_cast<double>(Log_GetQpcUs() - injectorInitStartUs) / 1000.0);
        configureInjector(injector);
        LogInfo("[Inject] Injection manager initialized");
    } else {
        LogInfo("[Inject] Injection manager SKIPPED (capture_method=%s, no overlay whitelist targets)",
                currentConfig.captureMethod.c_str());
    }

    LogInfo("[Inject] Process started (PID: %lu)", GetCurrentProcessId());
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    // Main loop
    while (g_Running) {
        // Check for IPC commands
        ProcessCommand cmd;
        if (ipc.PollCommand(cmd)) {
            switch (cmd) {
                case ProcessCommand::Shutdown:
                    LogInfo("[Inject] Shutdown command received");
                    g_Running = false;
                    ipc.SendResponse(ProcessResponse::Ack);
                    break;
                case ProcessCommand::StartRecording:
                    // Only reset start time if not already recording to avoid resetting the
                    // overlay timer on duplicate commands (e.g. if IPC state was desynced).
                    if (!pSharedMem->runtimeState.isRecording.load()) {
                        pSharedMem->runtimeState.recordingStartTime.store(GetTickCount64());
                    }
                    pSharedMem->runtimeState.isRecording.store(true);
                    // Set command flag for media process to poll (use atomic store)
                    pSharedMem->runtimeState.cmdStartRecording.store(true, std::memory_order_release);
                    ipc.SendResponse(ProcessResponse::Ack);
                    break;
                case ProcessCommand::StopRecording:
                    pSharedMem->runtimeState.isRecording.store(false);
                    pSharedMem->runtimeState.recordingStartTime.store(0);
                    // Set command flag for media process to poll (use atomic store)
                    pSharedMem->runtimeState.cmdStopRecording.store(true, std::memory_order_release);
                    ipc.SendResponse(ProcessResponse::Ack);
                    break;
                case ProcessCommand::Ping:
                    ipc.SendResponse(ProcessResponse::Pong);
                    break;
                case ProcessCommand::ReloadConfig: {
                    AppConfig reloadedConfig;
                    LoadConfig(configPath, reloadedConfig);

                    InjectorConfigState newInjectorState = BuildInjectorConfigState(reloadedConfig);
                    const bool shouldRescan =
                        ShouldRescanForConfigChange(currentConfig, injectorState, reloadedConfig, newInjectorState);

                    currentConfig = reloadedConfig;
                    injectorState = newInjectorState;

                    if (pDiscovery) {
                        PopulateWhitelistCache(pDiscovery, currentConfig);
                    }

                    AppConfig activeConfig = ResolveActiveTargetConfig(configPath, pSharedMem, currentConfig);
                    UpdateSharedMemoryFromConfig(pSharedMem, activeConfig);

                    if (injector) {
                        injector->UpdateConfig(injectorState.config);
                        if (shouldRescan && injectorState.allowInjection) {
                            LogInfo(
                                "[Inject] Config reload changed whitelist/injection policy, rescanning running "
                                "processes");
                            injector->RescanExistingProcesses();
                        } else if (!injectorState.allowInjection) {
                            LogInfo(
                                "[Inject] Config reload disabled new injections; existing injected targets are left "
                                "alone");
                        }
                    } else if (injectorState.allowInjection) {
                        const int64_t injectorInitStartUs = Log_GetQpcUs();
                        injector = std::make_shared<InjectionManager>(injectorState.config);
                        configureInjector(injector);
                        LogInfo("[Inject] Injection manager started after config reload in %.3f ms",
                                static_cast<double>(Log_GetQpcUs() - injectorInitStartUs) / 1000.0);
                    }

                    ipc.SendResponse(ProcessResponse::Ack);
                    break;
                }
                default:
                    ipc.SendResponse(ProcessResponse::Ack);
                    break;
            }
        }

        // Monitor sourcePid for config reloads (CBT hook support)
        static uint32_t lastSourcePid = 0;
        uint32_t currentSourcePid = pSharedMem->GetSourcePid();
        if (currentSourcePid != 0 && currentSourcePid != lastSourcePid) {
            lastSourcePid = currentSourcePid;
            std::string procName = GetProcessNameFromPID(currentSourcePid);
            LogInfo(
                "[Inject] Hook detected in process: %s (PID: %d). Applying "
                "overrides...",
                procName.c_str(), currentSourcePid);

            // Reload config for this process
            AppConfig targetConfig;
            LoadConfig(configPath, targetConfig, procName);

            // Update Shared Memory
            UpdateSharedMemoryFromConfig(pSharedMem, targetConfig);
        }

        // Update injector (scan for games, inject)
        // Update injector (process pending WMI injections)
        if (injector) {
            injector->Update();
        }

        Sleep(10);  // Main loop sleep (low latency for IPC)
    }

    // Signal hook to exit
    LogInfo("[Inject] Signaling hook to exit...");
    pSharedMem->SetRequestExit(true);
    Sleep(200);  // Give hook time to unload

    // Cleanup injector (ejects all hooks)
    // Destructor handles this

    // Cleanup shared memory and handles
    LogInfo("[Inject] Cleaning up...");
    if (pShmem)
        UnmapViewOfFile(pShmem);
    if (hMapShmem)
        CloseHandle(hMapShmem);

    UnmapViewOfFile(pSharedMem);
    CloseHandle(hMapFile);

    // Cleanup discovery shared memory
    if (pDiscovery) {
        UnmapViewOfFile(pDiscovery);
    }
    if (hDiscoveryFile) {
        CloseHandle(hDiscoveryFile);
    }

    // Close limiter event handles
    if (hLimiterReleaseEvent)
        CloseHandle(hLimiterReleaseEvent);
    if (hLimiterRequestEvent)
        CloseHandle(hLimiterRequestEvent);
    if (hHookDll) {
        FreeLibrary(hHookDll);
    }

    LogInfo("[Inject] Injector shutdown complete.");
    return 0;
}
