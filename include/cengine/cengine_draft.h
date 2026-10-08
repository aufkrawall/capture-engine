/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 aufkrawall
 * UNSHIPPED DRAFT: declarations only; no runtime or exports are implemented.
 * cengine_draft.h - CaptureEngine runtime library, public C API. */
#ifndef CENGINE_DRAFT_H
#define CENGINE_DRAFT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CE_BUILDING_LIBRARY)
#define CE_API __declspec(dllexport)
#else
#define CE_API __declspec(dllimport)
#endif

#define CE_API_VERSION_MAJOR 2u
#define CE_API_VERSION_MINOR 0u
#define CE_API_VERSION ((CE_API_VERSION_MAJOR << 16) | CE_API_VERSION_MINOR)

typedef int32_t ce_status_t;
#define CE_OK 0
#define CE_E_INVALID_ARGUMENT (-1)
#define CE_E_INVALID_STATE (-2)
#define CE_E_BUSY (-3)
#define CE_E_VERSION (-4)
#define CE_E_PACKAGE (-5)
#define CE_E_UNSUPPORTED (-6)
#define CE_E_TIMEOUT (-7)
#define CE_E_SETTINGS (-8)
#define CE_E_CONFLICT (-9)
#define CE_E_IPC (-10)
#define CE_E_PROCESS (-11)
#define CE_E_IO (-12)
#define CE_E_NO_MEMORY (-13)
#define CE_E_BUFFER_TOO_SMALL (-14)
#define CE_E_NOT_FOUND (-15)
#define CE_E_INTERNAL (-100)

typedef struct ce_runtime ce_runtime_t;
typedef uint64_t ce_request_t; /* 0 = none */

/* Pin the Windows x64 ABI regardless of the client's surrounding packing. */
#pragma pack(push, 8)

/* ---- Utilities (no runtime required) ------------------------------------------------ */
CE_API uint32_t ce_api_version(void);
CE_API const char* ce_version_string(void);              /* product build, static UTF-8 */
CE_API const char* ce_status_string(ce_status_t status); /* static UTF-8 */

/* ---- Runtime lifecycle ------------------------------------------------------------- */
#define CE_FEATURE_GLOBAL_HOTKEYS (1u << 0)  /* runtime registers configured hotkeys      */
#define CE_FEATURE_DESKTOP_OVERLAY (1u << 1) /* runtime may show the desktop status overlay */
#define CE_FEATURE_CRASH_HANDLER (1u << 2)   /* install CE crash handling in this process */
#define CE_FEATURE_WATCH_SETTINGS (1u << 3)  /* apply external edits of the settings file */

typedef struct ce_runtime_desc {
    uint32_t struct_size;    /* sizeof(ce_runtime_desc_t) */
    uint32_t api_version;    /* CE_API_VERSION */
    const char* package_dir; /* UTF-8; NULL = directory containing cengine.dll */
    const char* data_dir;    /* UTF-8; logs/dumps root; NULL = <package_dir>\logs */
    const char* client_name; /* UTF-8, for diagnostics; NULL allowed */
    uint32_t features;       /* CE_FEATURE_* */
    uint32_t reserved;       /* 0 */
} ce_runtime_desc_t;

CE_API ce_status_t ce_runtime_desc_init(ce_runtime_desc_t* desc);
/* Validates package and settings, claims session ownership, starts the engine thread.
 * Returns before helpers are ready; READY/FAILED arrive as CE_EVENT_RUNTIME_STATE. */
CE_API ce_status_t ce_runtime_create(const ce_runtime_desc_t* desc, ce_runtime_t** out_runtime);
/* Non-blocking. Stops any recording (finalization continues in helpers), then helpers.
 * Idempotent. Completion: CE_EVENT_RUNTIME_STATE with state STOPPED. */
CE_API ce_status_t ce_runtime_request_shutdown(ce_runtime_t* runtime);
/* request_shutdown + wait up to timeout_ms. CE_E_TIMEOUT keeps the runtime valid; retry allowed. */
CE_API ce_status_t ce_runtime_shutdown(ce_runtime_t* runtime, uint32_t timeout_ms);
/* Only in state STOPPED or FAILED; otherwise CE_E_INVALID_STATE. Frees all events/edits. */
CE_API ce_status_t ce_runtime_destroy(ce_runtime_t* runtime);

/* ---- Status (cheap snapshot, any thread) --------------------------------------------- */
#define CE_RUNTIME_STARTING 1
#define CE_RUNTIME_READY 2
#define CE_RUNTIME_STOPPING 3
#define CE_RUNTIME_STOPPED 4
#define CE_RUNTIME_FAILED 5

#define CE_RECORDING_IDLE 0
#define CE_RECORDING_STARTING 1 /* request accepted, media not live yet */
#define CE_RECORDING_LIVE 2
#define CE_RECORDING_STOPPING 3 /* stop accepted; next state IDLE. Finalization is a separate event */

#define CE_MODE_VIDEO 1
#define CE_MODE_AUDIO_ONLY 2

#define CE_HELPER_INJECT (1u << 0)
#define CE_HELPER_MEDIA (1u << 1)
#define CE_HELPER_LOGGER (1u << 2)
#define CE_HELPER_SENSORS (1u << 3)

typedef struct ce_status_info {
    uint32_t struct_size;
    int32_t runtime_state;     /* CE_RUNTIME_* */
    int32_t recording_state;   /* CE_RECORDING_* */
    int32_t recording_mode;    /* CE_MODE_* or 0 */
    uint64_t recording_id;     /* current/last recording, 0 = none */
    uint32_t helpers_ready;    /* CE_HELPER_* bits */
    uint32_t finalizing_count; /* recordings whose output is still being finalized */
    uint64_t settings_revision;
} ce_status_info_t;

CE_API ce_status_t ce_runtime_get_status(const ce_runtime_t* runtime, ce_status_info_t* out);

/* ---- Commands (non-blocking; exactly one COMMAND_COMPLETED per accepted request) ------- */
CE_API ce_status_t ce_recording_start(ce_runtime_t* rt, int32_t mode, const char* reason, ce_request_t* out);
CE_API ce_status_t ce_recording_stop(ce_runtime_t* rt, const char* reason, ce_request_t* out);
/* Engine-owned toggle policy (pending start, stop-while-starting, mode switch). */
CE_API ce_status_t ce_recording_toggle(ce_runtime_t* rt, int32_t mode, const char* reason, ce_request_t* out);
CE_API ce_status_t ce_overlay_toggle(ce_runtime_t* rt, ce_request_t* out);
CE_API ce_status_t ce_benchmark_toggle(ce_runtime_t* rt, ce_request_t* out);
CE_API ce_status_t ce_screenshot_take(ce_runtime_t* rt, ce_request_t* out);
/* Launch a program with CE injection policy (suspended inject or launcher passthrough). */
CE_API ce_status_t ce_target_launch(ce_runtime_t* rt, const char* command_line, ce_request_t* out);

/* ---- Events ------------------------------------------------------------------------- */
#define CE_EVENT_RUNTIME_STATE 1
#define CE_EVENT_COMMAND_COMPLETED 2
#define CE_EVENT_RECORDING_STATE 3
#define CE_EVENT_RECORDING_FINALIZED 4
#define CE_EVENT_SCREENSHOT 5
#define CE_EVENT_SETTINGS_CHANGED 6
#define CE_EVENT_SETTINGS_REJECTED 7 /* external file edit failed validation; previous stays */
#define CE_EVENT_HELPER 8
#define CE_EVENT_HOTKEY 9 /* informational: a runtime-owned hotkey fired */
#define CE_EVENT_EVENTS_DROPPED 10

#define CE_HELPER_STATE_READY 1
#define CE_HELPER_STATE_LOST 2
#define CE_HELPER_STATE_RECOVERED 3

#define CE_SETTINGS_SOURCE_API 1
#define CE_SETTINGS_SOURCE_FILE 2

#define CE_HOTKEY_RECORD 1
#define CE_HOTKEY_AUDIO_ONLY 2
#define CE_HOTKEY_SCREENSHOT 3
#define CE_HOTKEY_OVERLAY 4
#define CE_HOTKEY_BENCHMARK 5

typedef struct ce_event {
    uint32_t struct_size; /* runtime-allocated; >= what the client compiled against */
    int32_t type;         /* CE_EVENT_*; ignore unknown */
    uint64_t sequence;    /* strictly increasing per runtime */
    int64_t time_us;      /* runtime monotonic clock, microseconds */
    ce_request_t request; /* originating request or 0 */
    ce_status_t status;   /* outcome for COMMAND_COMPLETED / FINALIZED / SCREENSHOT */
    uint32_t reserved0;
    const char* message; /* UTF-8 diagnostic or NULL; valid until released */
    union {
        struct {
            int32_t state;
        } runtime;
        struct {
            uint64_t recording_id;
            int32_t state;
            int32_t mode;
            uint32_t failure;
        } recording;
        struct {
            uint64_t recording_id;
            const char* output_path;
        } finalized;
        struct {
            const char* const* paths;
            uint32_t path_count;
        } screenshot;
        struct {
            uint64_t revision;
            int32_t source;
        } settings;
        struct {
            uint32_t helper;
            int32_t state;
        } helper;
        struct {
            int32_t action;
        } hotkey;
        struct {
            uint64_t count;
        } dropped;
        uint8_t reserved[64];
    } data;
} ce_event_t;

/* Waits up to timeout_ms (0 = poll; UINT32_MAX rejected). CE_E_TIMEOUT when none. */
CE_API ce_status_t ce_runtime_next_event(ce_runtime_t* rt, uint32_t timeout_ms, const ce_event_t** out);
CE_API ce_status_t ce_runtime_release_event(ce_runtime_t* rt, const ce_event_t* event);
/* Manual-reset Win32 event, signaled while events are pending. Owned by the runtime;
 * valid until destroy. For MsgWaitForMultipleObjects-style client loops. */
CE_API void* ce_runtime_event_handle(ce_runtime_t* rt);

/* ---- Settings (INI document addressed by section/key) -------------------------------- */
typedef struct ce_settings_edit ce_settings_edit_t;

#define CE_DIAG_WARNING 1
#define CE_DIAG_ERROR 2

typedef struct ce_settings_diagnostic {
    uint32_t struct_size;
    int32_t severity;    /* CE_DIAG_* */
    const char* section; /* UTF-8; valid until the edit is discarded */
    const char* key;
    const char* message;
} ce_settings_diagnostic_t;

/* Copies the effective stored value (not defaults) into buf. NULL buf queries *needed. */
CE_API ce_status_t ce_settings_get(ce_runtime_t* rt, const char* section, const char* key, char* buf, size_t capacity,
                                   size_t* needed);
CE_API ce_status_t ce_settings_begin(ce_runtime_t* rt, ce_settings_edit_t** out_edit);
/* value NULL removes the key. Section "" = global section. */
CE_API ce_status_t ce_settings_set(ce_settings_edit_t* edit, const char* section, const char* key, const char* value);
/* Validates synchronously. CE_E_SETTINGS -> read diagnostics, edit stays open for fixes.
 * CE_E_CONFLICT -> discard and begin again. CE_OK -> queued; COMMAND_COMPLETED then
 * SETTINGS_CHANGED. The edit is consumed on CE_OK. */
CE_API ce_status_t ce_settings_commit(ce_settings_edit_t* edit, ce_request_t* out);
CE_API ce_status_t ce_settings_diagnostic_count(const ce_settings_edit_t* edit, uint32_t* count);
CE_API ce_status_t ce_settings_get_diagnostic(const ce_settings_edit_t* edit, uint32_t index,
                                              ce_settings_diagnostic_t* out);
CE_API void ce_settings_discard(ce_settings_edit_t* edit);

/* ---- Platform setup (M9) ------------------------------------------------------------ */
#define CE_SETUP_SENSOR_DRIVER_INSTALL 1
#define CE_SETUP_SENSOR_DRIVER_UNINSTALL 2
#define CE_SETUP_ELEVATION_SERVICE_INSTALL 3
#define CE_SETUP_ELEVATION_SERVICE_REMOVE 4
#define CE_SETUP_CLIENT_AUTOSTART_ON 5 /* registers the calling executable */
#define CE_SETUP_CLIENT_AUTOSTART_OFF 6

typedef struct ce_setup_status {
    uint32_t struct_size;
    uint32_t sensor_driver_installed; /* 0/1 */
    int32_t elevation_service_state;  /* 0 absent, 1 installed, 2 running, -1 unknown */
    uint32_t client_autostart;        /* 0/1 */
} ce_setup_status_t;

CE_API ce_status_t ce_setup_request(ce_runtime_t* rt, int32_t action, ce_request_t* out);
CE_API ce_status_t ce_setup_get_status(ce_runtime_t* rt, ce_setup_status_t* out);

/* ---- Misc (M9) ---------------------------------------------------------------------- */
typedef struct ce_monitor_info {
    char id[256];   /* value usable as capture_monitor=id:<...> */
    char name[128]; /* UTF-8 friendly name */
    int32_t left, top, right, bottom;
    uint32_t primary; /* 0/1 */
} ce_monitor_info_t;

/* item_size = sizeof(ce_monitor_info_t). Fills up to capacity, reports total in *count. */
CE_API ce_status_t ce_enumerate_monitors(ce_monitor_info_t* items, uint32_t item_size, uint32_t capacity,
                                         uint32_t* count);

#define CE_LOG_ERROR 1
#define CE_LOG_WARN 2
#define CE_LOG_INFO 3
#define CE_LOG_DEBUG 4
/* Writes one line into the runtime's log, tagged with client_name. */
CE_API ce_status_t ce_runtime_log(ce_runtime_t* rt, int32_t level, const char* message);

#pragma pack(pop)

#ifdef __cplusplus
}
#endif
#endif /* CENGINE_DRAFT_H */
