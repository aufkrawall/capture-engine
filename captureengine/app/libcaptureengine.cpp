#include "include/libcaptureengine.h"

#include "captureengine/app/main_internal.h"
#include "captureengine/media/screenshot.h"

#include <cstring>
#include <new>

struct ce_engine {
    ce_engine_config_t config;
    bool initialized = false;
};

extern "C" {

ce_status_t ce_engine_config_init_default(ce_engine_config_t* config) {
    if (!config) {
        return CE_ERROR_INVALID_ARGUMENT;
    }
    std::memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(ce_engine_config_t);
    config->log_level = CE_LOG_LEVEL_INFO;
    config->enable_hotkeys = true;
    config->enable_system_tray = true;
    config->start_minimized = false;
    return CE_SUCCESS;
}

ce_status_t ce_engine_create(const ce_engine_config_t* config, ce_engine_t** out_engine) {
    if (!out_engine) {
        return CE_ERROR_INVALID_ARGUMENT;
    }
    auto* engine = new (std::nothrow) ce_engine();
    if (!engine) {
        return CE_ERROR_PROCESS_FAILURE;
    }
    if (config) {
        if (config->struct_size != sizeof(ce_engine_config_t)) {
            delete engine;
            return CE_ERROR_INVALID_ARGUMENT;
        }
        engine->config = *config;
    } else {
        ce_engine_config_init_default(&engine->config);
    }
    engine->initialized = true;
    *out_engine = engine;
    return CE_SUCCESS;
}

ce_status_t ce_engine_destroy(ce_engine_t* engine) {
    if (!engine) {
        return CE_ERROR_INVALID_ARGUMENT;
    }
    delete engine;
    return CE_SUCCESS;
}

ce_status_t ce_engine_start_recording(ce_engine_t* engine, ce_recording_intent_t intent, const char* reason) {
    (void)engine;
    if (intent == CE_RECORDING_INTENT_IDLE) {
        return CE_ERROR_INVALID_ARGUMENT;
    }
    if (main_g_RecordingStartIntent.load(std::memory_order_acquire) != RecordingStartIntent::Idle) {
        return CE_ERROR_INVALID_STATE;
    }
    if (intent == CE_RECORDING_INTENT_AUDIO_ONLY) {
        ToggleAudioOnlyRecording();
    } else {
        ToggleRecording();
    }
    return CE_SUCCESS;
}

ce_status_t ce_engine_stop_recording(ce_engine_t* engine, const char* reason) {
    (void)engine;
    if (main_g_RecordingStartIntent.load(std::memory_order_acquire) == RecordingStartIntent::Idle) {
        return CE_SUCCESS;
    }
    const char* stopReason = (reason && reason[0] != '\0') ? reason : "ce_engine_stop_recording";
    RequestRecordingStopAndReleaseMedia(stopReason, 5000);
    PublishRecordingStartIntent(RecordingStartIntent::Idle, stopReason);
    return CE_SUCCESS;
}

ce_status_t ce_engine_toggle_recording(ce_engine_t* engine) {
    (void)engine;
    ToggleRecording();
    return CE_SUCCESS;
}

ce_status_t ce_engine_toggle_audio_only(ce_engine_t* engine) {
    (void)engine;
    ToggleAudioOnlyRecording();
    return CE_SUCCESS;
}

ce_status_t ce_engine_toggle_overlay(ce_engine_t* engine) {
    (void)engine;
    ToggleOverlay();
    return CE_SUCCESS;
}

ce_status_t ce_engine_toggle_benchmark(ce_engine_t* engine) {
    (void)engine;
    ToggleBenchmark();
    return CE_SUCCESS;
}

ce_status_t ce_engine_take_screenshot(ce_engine_t* engine) {
    (void)engine;
    if (main_g_PseudoOverlay) {
        main_g_PseudoOverlay->BeginScreenshotCapture();
    }
    const bool screenshotSaved = TakeScreenshot(main_g_Config.screenshotDir, main_g_Config.screenshotColorSpace);
    if (main_g_PseudoOverlay) {
        main_g_PseudoOverlay->EndScreenshotCapture();
        main_g_PseudoOverlay->ShowScreenshotNotification(screenshotSaved);
    }
    // Show the same result in the inject overlay (hooked game).
    HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
    if (hDisc) {
        auto* pDisc = static_cast<DiscoveryInfo*>(MapViewOfFile(hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo)));
        if (pDisc) {
            if (ValidateDiscoveryInfo(pDisc)) {
                const uint32_t injPid = pDisc->GetInjectPid();
                if (injPid != 0) {
                    wchar_t shmName[64];
                    GenerateSharedMemName(shmName, 64, injPid);
                    HANDLE hShm = OpenFileMappingW(FILE_MAP_WRITE | FILE_MAP_READ, FALSE, shmName);
                    if (hShm) {
                        auto* pShm = static_cast<SharedMemoryLayout*>(
                            MapViewOfFile(hShm, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, sizeof(SharedMemoryLayout)));
                        if (pShm && ValidateSharedMemory(pShm)) {
                            const OverlayNotificationType notification = screenshotSaved
                                                                             ? OverlayNotificationType::ScreenshotSaved
                                                                             : OverlayNotificationType::ScreenshotFailed;
                            pShm->runtimeState.notificationType.store(static_cast<uint32_t>(notification),
                                                                      std::memory_order_release);
                            pShm->runtimeState.notificationExpiry.store(GetTickCount64() + 2000ULL,
                                                                        std::memory_order_release);
                        } else if (pShm) {
                            LogError("[Controller] Screenshot notification rejected incompatible inject shared memory ABI");
                        }
                        if (pShm) {
                            UnmapViewOfFile(pShm);
                        }
                        CloseHandle(hShm);
                    }
                }
            }
            UnmapViewOfFile(pDisc);
        }
        CloseHandle(hDisc);
    }
    return screenshotSaved ? CE_SUCCESS : CE_ERROR_IO_FAILURE;
}

ce_status_t ce_engine_is_recording(const ce_engine_t* engine, bool* out_is_recording) {
    (void)engine;
    if (!out_is_recording) {
        return CE_ERROR_INVALID_ARGUMENT;
    }
    *out_is_recording = (main_g_RecordingStartIntent.load(std::memory_order_acquire) != RecordingStartIntent::Idle);
    return CE_SUCCESS;
}

ce_status_t ce_engine_get_recording_stats(const ce_engine_t* engine, ce_recording_stats_t* out_stats) {
    (void)engine;
    if (!out_stats) {
        return CE_ERROR_INVALID_ARGUMENT;
    }
    std::memset(out_stats, 0, sizeof(*out_stats));
    out_stats->struct_size = sizeof(ce_recording_stats_t);
    return CE_SUCCESS;
}

ce_status_t ce_engine_poll_events(ce_engine_t* engine, uint32_t timeout_ms) {
    (void)engine;
    MSG msg = {};
    if (timeout_ms == 0) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    } else {
        const DWORD startTick = GetTickCount();
        while ((GetTickCount() - startTick) < timeout_ms) {
            if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            } else {
                Sleep(1);
            }
        }
    }
    return CE_SUCCESS;
}

const char* ce_status_to_string(ce_status_t status) {
    switch (status) {
        case CE_SUCCESS:
            return "Success";
        case CE_ERROR_INVALID_ARGUMENT:
            return "Invalid argument";
        case CE_ERROR_INVALID_STATE:
            return "Invalid state";
        case CE_ERROR_NOT_INITIALIZED:
            return "Not initialized";
        case CE_ERROR_ALREADY_INITIALIZED:
            return "Already initialized";
        case CE_ERROR_PROCESS_FAILURE:
            return "Process failure";
        case CE_ERROR_IPC_FAILURE:
            return "IPC failure";
        case CE_ERROR_IO_FAILURE:
            return "I/O failure";
        case CE_ERROR_TIMEOUT:
            return "Timeout";
        case CE_ERROR_UNSUPPORTED:
            return "Unsupported";
        case CE_ERROR_UNKNOWN:
        default:
            return "Unknown error";
    }
}

const char* ce_version_string(void) {
    return "0.1.6968";
}

}  // extern "C"
