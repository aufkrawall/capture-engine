/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 aufkrawall
 * UNSHIPPED DRAFT: the sections of the C API that are not implemented yet: settings edits (M6), platform setup
 * (M9) and monitor enumeration (M9). Everything implemented is in cengine.h, which this header includes; a
 * section moves out of this file into cengine.h when it is implemented, so no declaration exists twice.
 * cengine_draft.h - CaptureEngine runtime library, public C API. */
#ifndef CENGINE_DRAFT_H
#define CENGINE_DRAFT_H

#include "cengine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pin the Windows x64 ABI regardless of the client's surrounding packing. */
#pragma pack(push, 8)

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

#pragma pack(pop)

#ifdef __cplusplus
}
#endif
#endif /* CENGINE_DRAFT_H */
