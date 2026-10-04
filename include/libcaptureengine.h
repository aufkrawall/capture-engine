/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 aufkrawall
 *
 * libcaptureengine: Public C Application Binary Interface (ABI) for Capture Engine.
 * Provides opaque handles, structured configuration, and life-cycle controls
 * for embedding Capture Engine into host applications or external front-ends.
 */

#ifndef LIBCAPTUREENGINE_H
#define LIBCAPTUREENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  if defined(CE_BUILD_SHARED)
#    define CE_API __declspec(dllexport)
#  elif defined(CE_USE_SHARED)
#    define CE_API __declspec(dllimport)
#  else
#    define CE_API
#  endif
#else
#  if defined(__GNUC__) && __GNUC__ >= 4
#    define CE_API __attribute__((visibility("default")))
#  else
#    define CE_API
#  endif
#endif

#define CE_VERSION_MAJOR 0
#define CE_VERSION_MINOR 1
#define CE_VERSION_PATCH 6968

/**
 * Return status codes for libcaptureengine functions.
 */
typedef enum ce_status {
    CE_SUCCESS = 0,
    CE_ERROR_INVALID_ARGUMENT = -1,
    CE_ERROR_INVALID_STATE = -2,
    CE_ERROR_NOT_INITIALIZED = -3,
    CE_ERROR_ALREADY_INITIALIZED = -4,
    CE_ERROR_PROCESS_FAILURE = -5,
    CE_ERROR_IPC_FAILURE = -6,
    CE_ERROR_IO_FAILURE = -7,
    CE_ERROR_TIMEOUT = -8,
    CE_ERROR_UNSUPPORTED = -9,
    CE_ERROR_UNKNOWN = -100
} ce_status_t;

/**
 * Recording intent / stream mode.
 */
typedef enum ce_recording_intent {
    CE_RECORDING_INTENT_IDLE = 0,
    CE_RECORDING_INTENT_VIDEO = 1,
    CE_RECORDING_INTENT_AUDIO_ONLY = 2
} ce_recording_intent_t;

/**
 * Diagnostic log level.
 */
typedef enum ce_log_level {
    CE_LOG_LEVEL_NONE = 0,
    CE_LOG_LEVEL_ERROR = 1,
    CE_LOG_LEVEL_WARN = 2,
    CE_LOG_LEVEL_INFO = 3,
    CE_LOG_LEVEL_DEBUG = 4,
    CE_LOG_LEVEL_TRACE = 5
} ce_log_level_t;

/**
 * Opaque engine instance handle.
 */
typedef struct ce_engine ce_engine_t;

/**
 * Engine configuration descriptor.
 */
typedef struct ce_engine_config {
    uint32_t struct_size;          /**< Set to sizeof(ce_engine_config_t) for ABI compatibility */
    const char* config_file_path;  /**< Optional path to config.ini (NULL for default) */
    const char* log_directory;     /**< Optional directory for log files (NULL for default) */
    ce_log_level_t log_level;      /**< Minimum logging severity level */
    bool enable_hotkeys;           /**< Whether to register global hotkeys */
    bool enable_system_tray;       /**< Whether to display system tray icon */
    bool start_minimized;          /**< Whether to begin in background / tray */
} ce_engine_config_t;

/**
 * Real-time recording telemetry statistics.
 */
typedef struct ce_recording_stats {
    uint32_t struct_size;          /**< Set to sizeof(ce_recording_stats_t) */
    uint64_t recorded_frames;      /**< Total video frames recorded */
    uint64_t duplicate_frames;     /**< Total duplicate frames emitted */
    uint64_t dropped_frames;       /**< Total dropped frames */
    double duration_seconds;       /**< Elapsed duration of active recording */
    double average_fps;            /**< Measured average encoding rate */
    uint64_t written_bytes;        /**< Output file size in bytes */
} ce_recording_stats_t;

/**
 * Initialize a configuration descriptor to sensible defaults.
 *
 * @param config Pointer to the descriptor to initialize.
 * @return CE_SUCCESS on success, error code otherwise.
 */
CE_API ce_status_t ce_engine_config_init_default(ce_engine_config_t* config);

/**
 * Create a new Capture Engine instance.
 *
 * @param config Pointer to configuration descriptor, or NULL for defaults.
 * @param out_engine Output pointer to receive the created engine handle.
 * @return CE_SUCCESS on success, error code otherwise.
 */
CE_API ce_status_t ce_engine_create(const ce_engine_config_t* config, ce_engine_t** out_engine);

/**
 * Destroy a Capture Engine instance and release all associated resources.
 *
 * @param engine The engine handle to destroy.
 * @return CE_SUCCESS on success, error code otherwise.
 */
CE_API ce_status_t ce_engine_destroy(ce_engine_t* engine);

/**
 * Start recording with the requested intent (Video or Audio-Only).
 *
 * @param engine The engine handle.
 * @param intent The recording stream mode.
 * @param reason Human-readable initiator reason for logging/diagnostics (optional).
 * @return CE_SUCCESS on success, error code otherwise.
 */
CE_API ce_status_t ce_engine_start_recording(ce_engine_t* engine, ce_recording_intent_t intent, const char* reason);

/**
 * Stop active recording.
 *
 * @param engine The engine handle.
 * @param reason Human-readable termination reason for logging/diagnostics (optional).
 * @return CE_SUCCESS on success, error code otherwise.
 */
CE_API ce_status_t ce_engine_stop_recording(ce_engine_t* engine, const char* reason);

/**
 * Toggle recording between active and stopped state.
 *
 * @param engine The engine handle.
 * @return CE_SUCCESS on success, error code otherwise.
 */
CE_API ce_status_t ce_engine_toggle_recording(ce_engine_t* engine);

/**
 * Toggle audio-only recording between active and stopped state.
 *
 * @param engine The engine handle.
 * @return CE_SUCCESS on success, error code otherwise.
 */
CE_API ce_status_t ce_engine_toggle_audio_only(ce_engine_t* engine);

/**
 * Toggle in-game overlay display.
 *
 * @param engine The engine handle.
 * @return CE_SUCCESS on success, error code otherwise.
 */
CE_API ce_status_t ce_engine_toggle_overlay(ce_engine_t* engine);

/**
 * Toggle benchmark / metrics logging session.
 *
 * @param engine The engine handle.
 * @return CE_SUCCESS on success, error code otherwise.
 */
CE_API ce_status_t ce_engine_toggle_benchmark(ce_engine_t* engine);

/**
 * Capture a screenshot of the active capture target.
 *
 * @param engine The engine handle.
 * @return CE_SUCCESS on success, error code otherwise.
 */
CE_API ce_status_t ce_engine_take_screenshot(ce_engine_t* engine);

/**
 * Query whether recording is currently active.
 *
 * @param engine The engine handle.
 * @param out_is_recording Output boolean receiving true if active, false otherwise.
 * @return CE_SUCCESS on success, error code otherwise.
 */
CE_API ce_status_t ce_engine_is_recording(const ce_engine_t* engine, bool* out_is_recording);

/**
 * Query current recording statistics.
 *
 * @param engine The engine handle.
 * @param out_stats Output statistics descriptor to populate.
 * @return CE_SUCCESS on success, error code otherwise.
 */
CE_API ce_status_t ce_engine_get_recording_stats(const ce_engine_t* engine, ce_recording_stats_t* out_stats);

/**
 * Process pending engine events and message pump callbacks.
 *
 * @param engine The engine handle.
 * @param timeout_ms Maximum time to wait in milliseconds (0 for non-blocking poll).
 * @return CE_SUCCESS on success, error code otherwise.
 */
CE_API ce_status_t ce_engine_poll_events(ce_engine_t* engine, uint32_t timeout_ms);

/**
 * Retrieve a human-readable description of a status code.
 *
 * @param status The status code.
 * @return Static null-terminated string describing the status.
 */
CE_API const char* ce_status_to_string(ce_status_t status);

/**
 * Retrieve the library version string (e.g. "0.1.6968").
 *
 * @return Static null-terminated version string.
 */
CE_API const char* ce_version_string(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBCAPTUREENGINE_H */
