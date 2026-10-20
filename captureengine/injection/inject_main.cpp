// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#include <atomic>
#include <cstring>
#include <new>
#include <string>
#include "captureengine/roles/role_entries.h"
#include "common/config/config.h"
#include "common/overlay/inject_overlay_policy.h"
#include "common/logging/logging.h"
#include "common/platform/ansi_path.h"
#include "common/platform/runtime_process_identity.h"
#include "common/ipc/process_ipc.h"
#include "common/ipc/role_context.h"
#include "common/ipc/shared_defs.h"
#include "captureengine/app/host_metrics.h"
#include "inject_config.h"
#include "inject_config_publication.h"
#include "injection.h"
#include "inject_lifecycle.h"

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
    ClearPublicationTarget();
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

// A stop request from the role's host (DR-38): a thread host sets the event, and in a helper process the process
// host's console handler does.
static bool StopRequested(HANDLE stopEvent) {
    return stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0;
}

namespace ce::role {

// The inject role (DR-38). It runs in a helper process from the process host, or on a runtime thread inside a
// client. Everything it touches resolves from the context: the package directory, the config path and the command
// channel. The process-wide effects are reserved for a process that owns itself (DR-39).
int RunInjectRole(RoleContext& context, const AppConfig& config) {
    // Threads the role starts inherit this sink (DR-40). A helper process passes none and writes the process log.
    ce::log::SinkScope roleLog(context.logSink);
    if (!context.channel) {
        LogError("[Inject] No command channel; the inject role cannot run");
        return 1;
    }
    if (context.packageDir.empty()) {
        LogError("[Inject] No package directory; refusing to run the inject role");
        return 1;
    }
    RoleChannel& ipc = *context.channel;
    // In a client the runtime is the host, and nothing else registers it: the process host's IPC does so for a
    // helper. Re-registering the same host is a no-op.
    if (!context.ownsProcess)
        ce::runtime_identity::RegisterHost(context.controllerPid, context.roleHostPath);

    AppConfig currentConfig = config;
    InjectorConfigState injectorState = BuildInjectorConfigState(currentConfig);
    const std::string& configPath = context.configPath;
    // Package-relative paths are published as active-code-page text: the hook reads them as ANSI.
    const std::string packageDir = ce::ansi_path::CompatiblePath(context.packageDir);

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
        pDiscovery->SetProfileTargetPid(0);
        pDiscovery->SetBuildNumber(GetCurrentBuildNumber());
        pDiscovery->SetAbiSignature(0);
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
    std::string logsDir = packageDir + "\\logs";
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

    // Copy graphics overrides and the overlay config (the publication helper
    // owns the overlay-config seqlock write, so there is one writer for it).
    SetPublicationBaseConfig(configPath, currentConfig, packageDir);
    PublishResolvedConfig(pSharedMem, "startup");

    InjectLifecycleControl injectLifecycle;
    injectLifecycle.Initialize();

    pSharedMem->structSize.store(sizeof(SharedMemoryLayout), std::memory_order_relaxed);
    pSharedMem->abiSignature.store(SHARED_MEMORY_ABI_SIGNATURE, std::memory_order_relaxed);
    pSharedMem->SetVersion(SHARED_MEMORY_VERSION);
    pSharedMem->SetMagic(SHARED_MEMORY_MAGIC);

    // Publish discovery only after ValidateSharedMemory() can succeed. This
    // prevents consumers from observing a partially initialized frame/log ring.
    // The signature is what consumers gate on, so it must be in place before the
    // magic makes the mapping visible to them.
    pDiscovery->SetBuildNumber(GetCurrentBuildNumber());
    pDiscovery->SetAbiSignature(SHARED_MEMORY_ABI_SIGNATURE);
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
    const InjectionEnvironment injectionEnvironment{context.packageDir, context.ownsProcess};
    std::shared_ptr<InjectionManager> injector;
    auto configureInjector = [&](const std::shared_ptr<InjectionManager>& manager) {
        if (!manager) {
            return;
        }

        manager->SetNgxOtaModeQuery([pSharedMem]() -> uint8_t {
            return pSharedMem ? pSharedMem->graphicsConfig.ngxOtaMode : kNgxOtaModeDefault;
        });

        manager->SetOnInjectCallback([&](DWORD targetPid, const std::string& processName) {
            LogInfo("[Inject] Reloading config for target: %s", processName.c_str());

            // Publish the exact configured parent before remote LoadLibrary.
            // A split renderer can create its Vulkan child while injection is
            // still pending, before sourcePid proves the hook connection.
            pDiscovery->SetProfileTargetPid(targetPid);
            LogInfo("[Inject] Published profile target identity: %s (PID: %lu)", processName.c_str(),
                    static_cast<unsigned long>(targetPid));

            // Suppress the controller-side layered pseudo overlay immediately
            // while the injected overlay handoff settles.
            SetInjectOverlayRuntimeFlag(pSharedMem, kCaptureRuntimeFlagInjectOverlayPending, true,
                                        "configureInjector:onInject");

            // Update shared memory with this target's profile applied
            PublishResolvedConfigForTarget(pSharedMem, processName, "injector:onInject");
        });
        manager->StartMonitoring();
    };

    if (injectorState.allowInjection) {
        // Keep controller ownership explicit. Delayed workers are owned and
        // joined by InjectionManager; they no longer extend the manager's own
        // lifetime through a shared_ptr cycle.
        const int64_t injectorInitStartUs = Log_GetQpcUs();
        injector = std::make_shared<InjectionManager>(injectorState.config, injectionEnvironment);
        configureInjector(injector);
        LogInfo("[StartupPerf] Injection manager construction and monitoring took %.3f ms",
                static_cast<double>(Log_GetQpcUs() - injectorInitStartUs) / 1000.0);
        LogInfo("[Inject] Injection manager initialized");
    } else {
        LogInfo("[Inject] Injection manager SKIPPED (capture_method=%s, no whitelist targets)",
                currentConfig.captureMethod.c_str());
    }

    LogInfo("[Inject] Process started (PID: %lu)", GetCurrentProcessId());
    if (context.ownsProcess) {
        // Trims the whole process's working set, so only the process that owns itself may do it (DR-39).
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    }

    // Main loop. A command wakes it through the controller's doorbell (ProcessIPCServer::WaitForCommand)
    // and is read at once: the controller waits for its ack on its main thread, and until 2026-10-18 this
    // loop only looked every 250 ms (100 ms with a game), which froze tray and hotkeys on config reloads.
    bool commandRang = false;
    DWORD lastIpcPoll = 0;
    DWORD lastInjectorUpdate = 0;
    uint32_t lastSourcePid = 0;
    DWORD lastIdentityWarningTick = 0;
    uint64_t lastReportedRefusal = 0;
    bool running = !StopRequested(context.stopEvent);
    while (running) {
        // Check for IPC commands (throttled when idle)
        ProcessCommand cmd;
        DWORD ipcNow = GetTickCount();
        bool hasGame = pSharedMem && pSharedMem->GetSourcePid() != 0;
        if (commandRang || hasGame || (ipcNow - lastIpcPoll) >= 250) {
            lastIpcPoll = ipcNow;
            if (ipc.PollCommand(cmd)) {
                switch (cmd) {
                    case ProcessCommand::Shutdown:
                        LogInfo("[Inject] Shutdown command received");
                        running = false;
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
                        //
                        // The toggle arms an override next to the config instead
                        // of editing the config, and republishes the active
                        // target's resolved config. Flipping the loaded base
                        // config and republishing that used to strip the running
                        // target's profile, so one hotkey press dropped its
                        // graphics, DLSS and UE5 overrides mid-session.
                        const bool overlayVisible = TogglePublishedOverlayVisibility(pSharedMem);
                        LogInfo("[Inject] Overlay %s via controller hotkey",
                                overlayVisible ? "enabled" : "disabled");
                        ipc.SendResponse(ProcessResponse::Ack);
                        break;
                    }
                    case ProcessCommand::ToggleBenchmark: {
                        if (pSharedMem) {
                            const uint64_t seq =
                                pSharedMem->benchmark.toggleSeq.fetch_add(1, std::memory_order_release) + 1;
                            LogInfo("[Inject] Benchmark toggle signaled via controller hotkey (seq=%llu)",
                                    static_cast<unsigned long long>(seq));
                        }
                        ipc.SendResponse(ProcessResponse::Ack);
                        break;
                    }
                    case ProcessCommand::ReloadConfig: {
                        // The controller waits for this ack on its main thread
                        // with a 1 s window and treats an overrun as a dead
                        // child, so the handler's cost is a contract, not a
                        // detail: exceeding it respawns this process and drops
                        // the injected hook's host out from under a running
                        // game. Keep the duration visible.
                        const int64_t reloadStartUs = Log_GetQpcUs();
                        AppConfig reloadedConfig;
                        LoadConfig(configPath, reloadedConfig);

                        InjectorConfigState newInjectorState = BuildInjectorConfigState(reloadedConfig);
                        const bool shouldRescan =
                            ShouldRescanForConfigChange(currentConfig, injectorState, reloadedConfig, newInjectorState);

                        currentConfig = reloadedConfig;
                        // The log level is process-wide, so only the process that owns itself sets it (DR-39).
                        if (context.ownsProcess)
                            Log_SetLevel(currentConfig.logLevel);
                        injectorState = newInjectorState;

                        if (pDiscovery) {
                            PopulateWhitelistCache(pDiscovery, currentConfig);
                        }

                        SetPublicationBaseConfig(configPath, currentConfig, packageDir);
                        PublishResolvedConfig(pSharedMem, "command:reload-config");

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
                            injector = std::make_shared<InjectionManager>(injectorState.config, injectionEnvironment);
                            configureInjector(injector);
                            LogInfo("[Inject] Injection manager started after config reload in %.3f ms",
                                    static_cast<double>(Log_GetQpcUs() - injectorInitStartUs) / 1000.0);
                        }

                        const double reloadMs = static_cast<double>(Log_GetQpcUs() - reloadStartUs) / 1000.0;
                        if (reloadMs >= 500.0) {
                            LogWarn(
                                "[Inject] Config reload took %.1f ms, close to the controller's 1000 ms ack "
                                "window; an overrun makes the controller respawn this process and blacks out "
                                "the injected overlay",
                                reloadMs);
                        } else {
                            LogDebug("[Inject] Config reload handled in %.1f ms", reloadMs);
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
            running = false;
            break;
        }
        // A Shutdown command ends the role here: the wait at the bottom of the loop must not hold it for one more poll.
        if (!running)
            break;

        // Record which runtime-override mechanism the hook refused.
        //
        // Every reason CE publishes concerns ONE mechanism: the `sl.*` plugin
        // set behind `streamline_dll_path`. The NGX runtimes behind
        // `dlss_sr_dll_path` / `dlss_fg_dll_path` / `dlss_rr_dll_path` are a
        // separate, generation-independent override that is unaffected by any
        // of it, and in session 20260918_221342 all three loaded from the
        // configured folder while the sl.* set was refused. Saying "the
        // configured override did not apply" there was simply wrong, and it is
        // the reason this is worded per mechanism now.
        //
        // INFO, not WARN: refusing to build a version-mixed Streamline stack is
        // CE behaving correctly. It is worth recording because the user cannot
        // otherwise tell which plugin set the game ended up with, but it is not
        // a fault and must not read like one. Reported once per publication -
        // the hook collapses the echoes into one first-refusal-wins value, so a
        // change here is a genuinely new decision.
        {
            const uint32_t refusalSourcePid = pSharedMem->GetSourcePid();
            const uint32_t reason = pSharedMem->runtimeOverrideStatus.ReadReasonForProcess(refusalSourcePid);
            const uint64_t refusalKey =
                (static_cast<uint64_t>(refusalSourcePid) << 32) | static_cast<uint64_t>(reason);
            if (reason != kRuntimeOverrideRefusalNone && refusalKey != lastReportedRefusal) {
                lastReportedRefusal = refusalKey;
                LogInfo(
                    "[Inject] streamline_dll_path sl.* plugin set not substituted in %s (PID: %lu): %s. The game "
                    "keeps its own coherent sl.* set; the dlss_*_dll_path NGX runtimes are a separate override and "
                    "are unaffected",
                    GetProcessNameFromPID(refusalSourcePid).c_str(), static_cast<unsigned long>(refusalSourcePid),
                    RuntimeOverrideRefusalText(reason));
            }
        }

        // Monitor sourcePid for config reloads (CBT hook support)
        const uint32_t profileTargetPid = pDiscovery ? pDiscovery->GetProfileTargetPid() : 0;
        if (profileTargetPid != 0 && !IsProcessAlive(profileTargetPid) &&
            pDiscovery->ClearProfileTargetPid(profileTargetPid)) {
            LogInfo("[Inject] Profile target process exited (PID: %lu); cleared discovery identity",
                    static_cast<unsigned long>(profileTargetPid));
        }
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

                // The hook is live now, so hide the controller-side layered pseudo
                // overlay immediately before the regular injector state poll runs.
                SetInjectOverlayRuntimeFlag(pSharedMem, kCaptureRuntimeFlagInjectOverlayPending, true,
                                            "sourcePid:hook-detected");

                // Update shared memory with this process' profile applied
                PublishResolvedConfigForTarget(pSharedMem, procName, "sourcePid:hook-detected");
            }
        }

        // Update injector (scan for games, inject)
        // Update injector (process pending WMI injections)
        DWORD now = GetTickCount();
        if (injector && (lastInjectorUpdate == 0 || (now - lastInjectorUpdate) >= 500)) {
            injector->Update();
            lastInjectorUpdate = now;

            // Track inject overlay state for pseudo-overlay suppression.
            const bool hasPending = injector->HasPendingInjections();
            const bool hasActive = injector->HasActiveInjections();
            SetInjectOverlayRuntimeState(pSharedMem, hasPending, hasActive, "injector:update");
        }

        // The wait is bounded by its timeout, so a stop request is seen within that bound. The stop event is not one of
        // the channel's wake events, and a thread host sets it without ringing the channel (RoleThreadRuns::RequestStop).
        // Waking on it would need a stop handle in RoleChannel::WaitForCommand, which the pipe and in-memory channels
        // both implement.
        commandRang = ipc.WaitForCommand(injector && pSharedMem && pSharedMem->GetSourcePid() != 0 ? 100 : 250);
        if (StopRequested(context.stopEvent))
            running = false;
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
    // The prewarm worker reads config.ini and the publication state, so it has
    // to be joined before either can go away.
    StopPublicationWarmup();
    // Withdraw discovery before closing the main mapping so new consumers
    // cannot observe a valid advertisement for a disappearing session.
    if (pDiscovery) {
        pDiscovery->SetMagic(0);
        pDiscovery->SetInjectPid(0);
        pDiscovery->SetProfileTargetPid(0);
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

    LogInfo("[Inject] Injector shutdown complete.");
    return 0;
}

}  // namespace ce::role
