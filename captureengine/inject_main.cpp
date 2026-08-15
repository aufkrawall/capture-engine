// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <new>
#include "../common/config.h"
#include "../common/crash_handler.h"
#include "../common/inject_overlay_policy.h"
#include "../common/logging.h"
#include "../common/process_identity.h"
#include "../common/process_ipc.h"
#include "../common/shared_defs.h"
#include "host_metrics.h"
#include "inject_config.h"
#include "injection.h"
#include "inject_lifecycle.h"

namespace fs = std::filesystem;

static std::atomic<bool> g_Running{true};

static const char* InjectOverlayRuntimeFlagName(CaptureRuntimeFlags flag) {
    switch (flag) {
        case kCaptureRuntimeFlagInjectOverlayActive:
            return "InjectOverlayActive";
        case kCaptureRuntimeFlagInjectOverlayPending:
            return "InjectOverlayPending";
        default:
            return "Unknown";
    }
}

static void SetInjectOverlayRuntimeFlag(SharedMemoryLayout* sharedMemory, CaptureRuntimeFlags flag, bool value,
                                        const char* source) {
    if (!sharedMemory) {
        return;
    }

    const bool previous = sharedMemory->runtimeState.HasRuntimeFlag(flag);
    sharedMemory->runtimeState.SetRuntimeFlag(flag, value);
    if (previous != value) {
        LogInfo("[InjectHandoff] flag=%s prev=%d next=%d source=%s", InjectOverlayRuntimeFlagName(flag),
                previous ? 1 : 0, value ? 1 : 0, source ? source : "unknown");
    }
}

static void SetInjectOverlayRuntimeState(SharedMemoryLayout* sharedMemory, bool pending, bool active,
                                         const char* source) {
    SetInjectOverlayRuntimeFlag(sharedMemory, kCaptureRuntimeFlagInjectOverlayPending, pending, source);
    SetInjectOverlayRuntimeFlag(sharedMemory, kCaptureRuntimeFlagInjectOverlayActive, active, source);
}

static bool IsProcessAlive(uint32_t processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
        return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD exitCode = 0;
    const bool alive = GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

static void ClearStaleHookSourceState(SharedMemoryLayout* sharedMemory) {
    if (!sharedMemory)
        return;
    sharedMemory->SetSourcePid(0);
    sharedMemory->SetLuidSourcePid(0);
    sharedMemory->runtimeState.screenshotRequestId.store(0, std::memory_order_release);
    sharedMemory->runtimeState.screenshotCompletedRequestId.store(0, std::memory_order_release);
    sharedMemory->runtimeState.screenshotStatus.store(static_cast<uint32_t>(ScreenshotRequestStatus::Idle),
                                                      std::memory_order_release);
    sharedMemory->runtimeState.screenshotError.store(ERROR_SUCCESS, std::memory_order_relaxed);
    sharedMemory->runtimeState.screenshotPayloadKind.store(static_cast<uint32_t>(ScreenshotPayloadKind::None),
                                                           std::memory_order_relaxed);
}

// Console control handler for graceful cleanup
BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT ||
        ctrlType == CTRL_LOGOFF_EVENT || ctrlType == CTRL_SHUTDOWN_EVENT) {
        LogInfo("[Inject] Console event %lu received, shutting down...", ctrlType);
        g_Running = false;
        return TRUE;
    }
    return FALSE;
}

// Helper: Get process name from PID
std::string GetProcessNameFromPID(DWORD pid) {
    const ce::process::ProcessIdentityResult identity = ce::process::QueryProcessIdentity(pid);
    if (!identity) {
        static std::atomic<uint32_t> failureLogs{0};
        if (failureLogs.fetch_add(1, std::memory_order_relaxed) < 16) {
            LogDebug("[Identity] Limited process-name query failed (pid=%lu error=%lu)",
                     static_cast<unsigned long>(pid), identity.error);
        }
    }
    return identity.imageName;
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
    if (!processName.empty()) {
        LoadConfig(configPath, activeConfig, processName);
    }

    return activeConfig;
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
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            memcpy(p, lower.c_str(), len);
            p += len;
            *p++ = '\0';
        }
    };

    size_t gameCount = 0;
    size_t overlayCount = 0;

    for (const auto& entry : config.gameWhitelist) {
        addName(entry.pattern);
        gameCount++;
        if (IsTraceLoggingEnabled(config.logLevel)) {
            LogInfo("[Inject] Added game to whitelist cache: %s", entry.pattern.c_str());
        }
    }
    for (const auto& entry : config.overlayWhitelist) {
        addName(entry.pattern);
        overlayCount++;
        if (IsTraceLoggingEnabled(config.logLevel)) {
            LogInfo("[Inject] Added overlay target to whitelist cache: %s", entry.pattern.c_str());
        }
    }

    // Always whitelist our test processes too
    if (IsTraceLoggingEnabled(config.logLevel)) {
        addName("dx12_test.exe");
        addName("dx11_test.exe");
        addName("vulkan_test.exe");
    }

    *p = '\0';  // Double null terminator
    LogInfo("[Inject] Whitelist cache prepared: games=%zu overlayTargets=%zu traceExtras=%d", gameCount, overlayCount,
            IsTraceLoggingEnabled(config.logLevel) ? 1 : 0);
}

int InjectProcessMain(const AppConfig& config) {
    // Install crash handler for this process
    InstallCrashHandler();
    Log_SetLevel(config.logLevel);

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
    const DWORD sharedMemoryCreateError = GetLastError();

    if (hMapFile == NULL) {
        LogError("[Inject] Failed to create shared memory: %lu", GetLastError());
        return 1;
    }
    if (sharedMemoryCreateError == ERROR_ALREADY_EXISTS) {
        LogError("[Inject] Refusing pre-existing shared memory '%ls'; a stale process still owns this session name",
                 sharedMemName);
        CloseHandle(hMapFile);
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

    // Construct every atomic member before use. Keep magic unpublished until
    // all fields and auxiliary mappings are ready.
    new (pSharedMem) SharedMemoryLayout();

    // Discovery shared memory
    HANDLE hDiscoveryFile =
        CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(DiscoveryInfo), SHARED_MEM_DISCOVERY);
    const DWORD discoveryCreateError = GetLastError();
    DiscoveryInfo* pDiscovery = nullptr;
    if (!hDiscoveryFile) {
        LogError("[Inject] Failed to create discovery mapping: %lu", GetLastError());
        UnmapViewOfFile(pSharedMem);
        CloseHandle(hMapFile);
        return 1;
    }
    pDiscovery = (DiscoveryInfo*)MapViewOfFile(hDiscoveryFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DiscoveryInfo));
    if (!pDiscovery) {
        LogError("[Inject] Failed to map discovery memory: %lu", GetLastError());
        CloseHandle(hDiscoveryFile);
        UnmapViewOfFile(pSharedMem);
        CloseHandle(hMapFile);
        return 1;
    }
    if (discoveryCreateError == ERROR_ALREADY_EXISTS) {
        const uint32_t existingMagic = pDiscovery->GetMagic();
        const uint32_t existingPid = pDiscovery->GetInjectPid();
        bool existingOwnerRunning = false;
        if (existingMagic == DISCOVERY_MAGIC && existingPid != 0) {
            HANDLE existingProcess = OpenProcess(SYNCHRONIZE, FALSE, existingPid);
            if (existingProcess) {
                existingOwnerRunning = WaitForSingleObject(existingProcess, 0) == WAIT_TIMEOUT;
                CloseHandle(existingProcess);
            } else {
                const DWORD openError = GetLastError();
                // ERROR_INVALID_PARAMETER is OpenProcess' documented result for
                // a PID that no longer exists. Other failures cannot prove that
                // the advertised owner is stale.
                existingOwnerRunning = openError != ERROR_INVALID_PARAMETER;
            }
        }
        if (existingOwnerRunning) {
            LogError("[Inject] Refusing discovery mapping owned by active inject PID %u", existingPid);
            UnmapViewOfFile(pDiscovery);
            CloseHandle(hDiscoveryFile);
            UnmapViewOfFile(pSharedMem);
            CloseHandle(hMapFile);
            return 1;
        }
        pDiscovery->SetMagic(0);
        pDiscovery->SetInjectPid(0);
        pDiscovery->SetBuildNumber(GetCurrentBuildNumber());
        memset(pDiscovery->processWhitelist, 0, sizeof(pDiscovery->processWhitelist));
        memset(pDiscovery->logsPath, 0, sizeof(pDiscovery->logsPath));
        LogInfo("[Inject] Reusing stale discovery mapping retained by another process (previous PID %u)",
                existingPid);
    } else {
        new (pDiscovery) DiscoveryInfo();
    }
    PopulateWhitelistCache(pDiscovery, currentConfig);
    // Set logs path for hook DLL and Vulkan layer to use (session-specific).
    // Discovery magic remains zero until the main layout is fully published.
    std::string logsDir = baseDir + "\\logs";
    if (!g_SessionDirName.empty())
        logsDir += "\\" + g_SessionDirName;
    strncpy(pDiscovery->logsPath, logsDir.c_str(), sizeof(pDiscovery->logsPath) - 1);
    pDiscovery->logsPath[sizeof(pDiscovery->logsPath) - 1] = '\0';

    // Initialize shared memory
    pSharedMem->SetHostPID(GetCurrentProcessId());
    pSharedMem->SetDebugLogging(IsDebugLoggingEnabled(currentConfig.logLevel));
    pSharedMem->SetLogLevel(currentConfig.logLevel);

    // Copy log path
    std::string logPath = currentConfig.logFilePath;
    strncpy(pSharedMem->logFilePath, logPath.c_str(), sizeof(pSharedMem->logFilePath) - 1);
    pSharedMem->logFilePath[sizeof(pSharedMem->logFilePath) - 1] = '\0';

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

    InjectLifecycleControl injectLifecycle;
    injectLifecycle.Initialize();

    pSharedMem->structSize.store(sizeof(SharedMemoryLayout), std::memory_order_relaxed);
    pSharedMem->abiSignature.store(SHARED_MEMORY_ABI_SIGNATURE, std::memory_order_relaxed);
    pSharedMem->SetVersion(SHARED_MEMORY_VERSION);
    pSharedMem->SetMagic(SHARED_MEMORY_MAGIC);

    // Publish discovery only after ValidateSharedMemory() can succeed. This
    // prevents consumers from observing a partially initialized frame/log ring.
    pDiscovery->SetBuildNumber(GetCurrentBuildNumber());
    pDiscovery->SetInjectPid(GetCurrentProcessId());
    pDiscovery->SetMagic(DISCOVERY_MAGIC);
    LogInfo("[Inject] Shared memory initialized: version=%u size=%zu abi=0x%08X mapping=%ls", SHARED_MEMORY_VERSION,
            sizeof(SharedMemoryLayout), SHARED_MEMORY_ABI_SIGNATURE, sharedMemName);
    LogInfo("[Inject] Published discovery memory with whitelist cache (logsPath=%s)", logsDir.c_str());

    // Injection and video acquisition are independent. Explicit screen-grab
    // methods keep the full game whitelist active for overlays and graphics
    // overrides; hook-side video copies are gated separately by the media path.
    if (injectorState.allowInjection && IsScreenGrabCaptureMethod(currentConfig.captureMethod)) {
        LogInfo(
            "[Inject] Video method %s keeps injection active: fullTargets=%zu overlayOnlyTargets=%zu "
            "(hook video copies follow the active media path)",
            currentConfig.captureMethod.c_str(), currentConfig.gameWhitelist.size(),
            currentConfig.overlayWhitelist.size());
    }
    std::shared_ptr<InjectionManager> injector;
    auto configureInjector = [&](const std::shared_ptr<InjectionManager>& manager) {
        if (!manager) {
            return;
        }

        manager->SetOnInjectCallback([&](const std::string& processName) {
            LogInfo("[Inject] Reloading config for target: %s", processName.c_str());

            // Suppress the controller-side layered pseudo overlay immediately
            // while the injected overlay handoff settles.
            SetInjectOverlayRuntimeFlag(pSharedMem, kCaptureRuntimeFlagInjectOverlayPending, true,
                                        "configureInjector:onInject");

            // Load fresh config with process-specific overrides
            AppConfig targetConfig;
            LoadConfig(configPath, targetConfig, processName);

            // Update Shared Memory with new values
            UpdateSharedMemoryFromConfig(pSharedMem, targetConfig);
        });
    };

    if (injectorState.allowInjection) {
        // Keep controller ownership explicit. Delayed workers are owned and
        // joined by InjectionManager; they no longer extend the manager's own
        // lifetime through a shared_ptr cycle.
        const int64_t injectorInitStartUs = Log_GetQpcUs();
        injector = std::make_shared<InjectionManager>(injectorState.config);
        LogInfo("[StartupPerf] Injection manager construction took %.3f ms",
                static_cast<double>(Log_GetQpcUs() - injectorInitStartUs) / 1000.0);
        configureInjector(injector);
        LogInfo("[Inject] Injection manager initialized");
    } else {
        LogInfo("[Inject] Injection manager SKIPPED (capture_method=%s, no whitelist targets)",
                currentConfig.captureMethod.c_str());
    }

    LogInfo("[Inject] Process started (PID: %lu)", GetCurrentProcessId());
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    // Main loop
    while (g_Running) {
        // Check for IPC commands (throttled when idle)
        ProcessCommand cmd;
        static DWORD lastIpcPoll = 0;
        DWORD ipcNow = GetTickCount();
        bool hasGame = pSharedMem && pSharedMem->GetSourcePid() != 0;
        if (hasGame || (ipcNow - lastIpcPoll) >= 250) {
            lastIpcPoll = ipcNow;
            if (ipc.PollCommand(cmd)) {
                switch (cmd) {
                    case ProcessCommand::Shutdown:
                        LogInfo("[Inject] Shutdown command received");
                        g_Running = false;
                        ipc.SendResponse(ProcessResponse::Ack);
                        break;
                    case ProcessCommand::StartRecording:
                        // Media owns the active video-path decision. Clear any flag
                        // left by an interrupted prior session before publishing the
                        // new raw recording request; inject mode will re-arm it during
                        // StartRecording after resolving the actual path.
                        if (!pSharedMem->runtimeState.captureRequested.load(std::memory_order_acquire)) {
                            if (pSharedMem->runtimeState.HasRuntimeFlag(
                                    kCaptureRuntimeFlagInjectVideoCaptureRequested)) {
                                LogWarn("[Inject] Clearing stale inject-video publication flag before recording start");
                            }
                            pSharedMem->runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested,
                                                                    false);
                        }
                        if (!pSharedMem->runtimeState.captureRequested.exchange(true, std::memory_order_acq_rel)) {
                            pSharedMem->runtimeState.isRecording.store(false, std::memory_order_release);
                            pSharedMem->runtimeState.recordingStartTime.store(0, std::memory_order_release);
                        }
                        // The controller normally publishes this before readiness waits. Preserve
                        // that exact intent, but recover a missing publication for direct/legacy
                        // command senders using the already-published audio-only bit.
                        if (pSharedMem->runtimeState.GetRecordingStartIntent() == RecordingStartIntent::Idle) {
                            const bool audioOnly =
                                pSharedMem->runtimeState.audioOnly.load(std::memory_order_acquire);
                            pSharedMem->runtimeState.SetRecordingStartIntent(
                                audioOnly ? RecordingStartIntent::AudioOnly : RecordingStartIntent::Video);
                        }
                        // Set command flag for media process to poll (use atomic store)
                        // Note: audioOnly flag is managed by the controller — do NOT clear
                        // it here — the controller may have set it for audio-only recording.
                        pSharedMem->runtimeState.cmdStartRecording.store(true, std::memory_order_release);
                        ipc.SendResponse(ProcessResponse::Ack);
                        break;
                    case ProcessCommand::StopRecording:
                        pSharedMem->runtimeState.SetRecordingStartIntent(RecordingStartIntent::Idle);
                        pSharedMem->runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested, false);
                        pSharedMem->runtimeState.captureRequested.store(false, std::memory_order_release);
                        pSharedMem->runtimeState.isRecording.store(false, std::memory_order_release);
                        pSharedMem->runtimeState.recordingStartTime.store(0, std::memory_order_release);
                        // Set command flag for media process to poll (use atomic store)
                        pSharedMem->runtimeState.cmdStopRecording.store(true, std::memory_order_release);
                        ipc.SendResponse(ProcessResponse::Ack);
                        break;
                    case ProcessCommand::Ping:
                        ipc.SendResponse(ProcessResponse::Pong);
                        break;
                    case ProcessCommand::ToggleOverlay: {
                        // Runtime overlay visibility override. Only this process
                        // writes overlayConfig, so the seqlock stays single-writer;
                        // the controller forwards the hotkey intent over IPC.
                        currentConfig.overlay.showOverlay = !currentConfig.overlay.showOverlay;
                        UpdateSharedMemoryFromConfig(pSharedMem, currentConfig);
                        LogInfo("[Inject] Overlay %s via controller hotkey",
                                currentConfig.overlay.showOverlay ? "enabled" : "disabled");
                        ipc.SendResponse(ProcessResponse::Ack);
                        break;
                    }
                    case ProcessCommand::ReloadConfig: {
                        AppConfig reloadedConfig;
                        LoadConfig(configPath, reloadedConfig);

                        InjectorConfigState newInjectorState = BuildInjectorConfigState(reloadedConfig);
                        const bool shouldRescan =
                            ShouldRescanForConfigChange(currentConfig, injectorState, reloadedConfig, newInjectorState);

                        currentConfig = reloadedConfig;
                        Log_SetLevel(currentConfig.logLevel);
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
                                    "[Inject] Config reload disabled new injections; existing injected targets are "
                                    "left "
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
        }
        if (ipc.HasFatalDisconnect()) {
            LogWarn("[Inject] Controller IPC disconnected; exiting for a clean respawn");
            g_Running = false;
            break;
        }

        // Monitor sourcePid for config reloads (CBT hook support)
        static uint32_t lastSourcePid = 0;
        static DWORD lastIdentityWarningTick = 0;
        uint32_t currentSourcePid = pSharedMem->GetSourcePid();
        // The hook lives inside the source process and dies with it, so the
        // hook-owned identity can only be cleared from here. A stale PID must
        // not keep host consumers (desktop screenshots, media, sensors)
        // treating a dead hook as an active source.
        if (currentSourcePid != 0 && !IsProcessAlive(currentSourcePid)) {
            LogInfo("[Inject] Hook source process exited (PID: %lu); clearing stale hook source identity",
                    static_cast<unsigned long>(currentSourcePid));
            ClearStaleHookSourceState(pSharedMem);
            lastSourcePid = 0;
            currentSourcePid = 0;
        }
        if (currentSourcePid != 0 && currentSourcePid != lastSourcePid) {
            std::string procName = GetProcessNameFromPID(currentSourcePid);
            if (procName.empty()) {
                const DWORD warningNow = GetTickCount();
                if (lastIdentityWarningTick == 0 || warningNow - lastIdentityWarningTick >= 5000) {
                    LogWarn("[Inject] Hook source identity unavailable (PID: %u); retaining current configuration",
                            currentSourcePid);
                    lastIdentityWarningTick = warningNow;
                }
            } else {
                lastSourcePid = currentSourcePid;
                LogInfo(
                    "[Inject] Hook detected in process: %s (PID: %d). Applying "
                    "overrides...",
                    procName.c_str(), currentSourcePid);

                // Reload config for this process
                AppConfig targetConfig;
                LoadConfig(configPath, targetConfig, procName);

                // The hook is live now, so hide the controller-side layered pseudo
                // overlay immediately before the regular injector state poll runs.
                SetInjectOverlayRuntimeFlag(pSharedMem, kCaptureRuntimeFlagInjectOverlayPending, true,
                                            "sourcePid:hook-detected");

                // Update Shared Memory
                UpdateSharedMemoryFromConfig(pSharedMem, targetConfig);
            }
        }

        // Update injector (scan for games, inject)
        // Update injector (process pending WMI injections)
        static DWORD lastInjectorUpdate = 0;
        DWORD now = GetTickCount();
        if (injector && (lastInjectorUpdate == 0 || (now - lastInjectorUpdate) >= 500)) {
            injector->Update();
            lastInjectorUpdate = now;

            // Track inject overlay state for pseudo-overlay suppression.
            const bool hasPending = injector->HasPendingInjections();
            const bool hasActive = injector->HasActiveInjections();
            SetInjectOverlayRuntimeState(pSharedMem, hasPending, hasActive, "injector:update");
        }

        if (injector && pSharedMem && pSharedMem->GetSourcePid() != 0) {
            Sleep(100);
        } else {
            Sleep(250);
        }
    }

    // Withdraw readiness, then wake every resident hook/layer. Deactivation is
    // cooperative because foreign chains and game wrappers can retain hook addresses.
    LogInfo("[InjectLifecycle] Requesting cooperative hook deactivation...");
    SetInjectOverlayRuntimeState(pSharedMem, false, false, "injector:shutdown");
    pSharedMem->SetRequestExit(true);
    injectLifecycle.SignalHostStopping();

    // Destroy injector explicitly before other cleanup so WMI teardown
    // (CancelAsyncCall + drain) completes while COM is still initialized.
    LogInfo("[Inject] Cleaning up...");
    injector.reset();
    // Withdraw discovery before closing the main mapping so new consumers
    // cannot observe a valid advertisement for a disappearing session.
    if (pDiscovery) {
        pDiscovery->SetMagic(0);
        pDiscovery->SetInjectPid(0);
    }
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
