# 03 - Public API v2 (`include/cengine/cengine.h`)

This is the outside-in contract. Implement the runtime to satisfy it. It is not a wrapper around
today's controller.

## Design principles

1. **Small and deep.** About 35 functions cover lifecycle, commands, events, status, settings,
   setup and utilities. All policy (toggle semantics, retries, child recovery, finalization, profile
   resolution) stays inside.
2. **Asynchronous commands, one completion each.** Every accepted command returns a request id and
   produces exactly one `CE_EVENT_COMMAND_COMPLETED` with the same id. Domain events (recording state
   and so on) come in addition.
3. **No callbacks.** The client pulls events (`ce_runtime_next_event`) or waits on a Win32 event handle.
   That removes reentry, client-thread affinity, and the risk of notices reaching a destroyed client.
4. **Honest capability.** No stubs. A function is in the shipped header only when it works; until
   then it stays out of the header (don't publish one that returns `CE_E_UNSUPPORTED`).
5. **C only at the boundary.** Fixed-width integers (no enums inside structs), UTF-8 strings,
   `struct_size` on every caller-allocated struct, runtime-owned memory for everything the runtime
   returns. No STL, no FFmpeg/SDK/COM types, no mutable config, no mappings, no locks.
6. **One runtime per Windows session.** Returned as `CE_E_BUSY`, never "undefined".
7. **x64 only** in v2.0.

## Versioning

- `CE_API_VERSION = (major << 16) | minor`. Clients pass the version they compiled against in
  `ce_runtime_desc_t.api_version`. Different major: `CE_E_VERSION`. Client minor greater than the
  runtime's: `CE_E_VERSION` (the client needs newer features). Lower minor: fine.
- Minor bumps are additive only: new functions, new event types, new feature bits, fields appended to
  structs (guarded by `struct_size`).
- Clients must ignore unknown event types and unknown status bits.
- `ce_api_version()` and `ce_version_string()` work without a runtime.

## Error model

| Code | Value | Meaning |
| --- | --- | --- |
| `CE_OK` | 0 | Success / accepted |
| `CE_E_INVALID_ARGUMENT` | -1 | Null/invalid pointer, bad enum, bad `struct_size`, stale handle |
| `CE_E_INVALID_STATE` | -2 | Not allowed in the current runtime/recording state (e.g. command before READY) |
| `CE_E_BUSY` | -3 | Another runtime owns this process or Windows session |
| `CE_E_VERSION` | -4 | API version incompatible |
| `CE_E_PACKAGE` | -5 | Runtime package incomplete or mismatched (missing role host, mediaengine, hooks; build identity mismatch) |
| `CE_E_UNSUPPORTED` | -6 | Capability unavailable on this system (e.g. sensor driver absent); never used for "not implemented" |
| `CE_E_TIMEOUT` | -7 | Bounded wait elapsed (no event; shutdown not finished) |
| `CE_E_SETTINGS` | -8 | Settings validation failed; diagnostics available |
| `CE_E_CONFLICT` | -9 | Settings changed since the edit began (e.g. user edited the INI) |
| `CE_E_IPC` | -10 | Helper did not accept or acknowledge |
| `CE_E_PROCESS` | -11 | Helper process failed to start, crashed, or was lost |
| `CE_E_IO` | -12 | File system failure (settings write, screenshot save) |
| `CE_E_NO_MEMORY` | -13 | Allocation failed |
| `CE_E_BUFFER_TOO_SMALL` | -14 | Caller buffer too small; required size reported |
| `CE_E_NOT_FOUND` | -15 | Key/section not present |
| `CE_E_INTERNAL` | -100 | Unexpected internal failure (logged with context) |

Synchronous return codes only describe **admission**: argument checks, state checks, queueing. The
outcome arrives in `CE_EVENT_COMMAND_COMPLETED.status`.

## Header draft

```c
/* SPDX-License-Identifier: MIT
 * cengine.h - CaptureEngine runtime library, public C API. */
#ifndef CENGINE_H
#define CENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CE_BUILDING_LIBRARY)
#  define CE_API __declspec(dllexport)
#else
#  define CE_API __declspec(dllimport)
#endif

#define CE_API_VERSION_MAJOR 2u
#define CE_API_VERSION_MINOR 0u
#define CE_API_VERSION ((CE_API_VERSION_MAJOR << 16) | CE_API_VERSION_MINOR)

typedef int32_t ce_status_t;
#define CE_OK                   0
#define CE_E_INVALID_ARGUMENT  (-1)
#define CE_E_INVALID_STATE     (-2)
#define CE_E_BUSY              (-3)
#define CE_E_VERSION           (-4)
#define CE_E_PACKAGE           (-5)
#define CE_E_UNSUPPORTED       (-6)
#define CE_E_TIMEOUT           (-7)
#define CE_E_SETTINGS          (-8)
#define CE_E_CONFLICT          (-9)
#define CE_E_IPC               (-10)
#define CE_E_PROCESS           (-11)
#define CE_E_IO                (-12)
#define CE_E_NO_MEMORY         (-13)
#define CE_E_BUFFER_TOO_SMALL  (-14)
#define CE_E_NOT_FOUND         (-15)
#define CE_E_INTERNAL          (-100)

typedef struct ce_runtime ce_runtime_t;
typedef uint64_t ce_request_t;          /* 0 = none */

/* Pin the Windows x64 ABI regardless of the client's surrounding packing. */
#pragma pack(push, 8)

/* ---- Utilities (no runtime required) ------------------------------------------------ */
CE_API uint32_t    ce_api_version(void);
CE_API const char* ce_version_string(void);              /* product build, static UTF-8 */
CE_API const char* ce_status_string(ce_status_t status); /* static UTF-8 */

/* ---- Runtime lifecycle ------------------------------------------------------------- */
#define CE_FEATURE_GLOBAL_HOTKEYS     (1u << 0) /* runtime registers configured hotkeys      */
#define CE_FEATURE_DESKTOP_OVERLAY    (1u << 1) /* runtime may show the desktop status overlay */
#define CE_FEATURE_CRASH_HANDLER      (1u << 2) /* install CE crash handling in this process */
#define CE_FEATURE_WATCH_SETTINGS     (1u << 3) /* apply external edits of the settings file */

typedef struct ce_runtime_desc {
    uint32_t    struct_size;   /* sizeof(ce_runtime_desc_t) */
    uint32_t    api_version;   /* CE_API_VERSION */
    const char* package_dir;   /* UTF-8; NULL = directory containing cengine.dll */
    const char* data_dir;      /* UTF-8; logs/dumps root; NULL = <package_dir>\logs */
    const char* client_name;   /* UTF-8, for diagnostics; NULL allowed */
    uint32_t    features;      /* CE_FEATURE_* */
    uint32_t    reserved;      /* 0 */
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
#define CE_RUNTIME_READY    2
#define CE_RUNTIME_STOPPING 3
#define CE_RUNTIME_STOPPED  4
#define CE_RUNTIME_FAILED   5

#define CE_RECORDING_IDLE     0
#define CE_RECORDING_STARTING 1   /* request accepted, media not live yet */
#define CE_RECORDING_LIVE     2
#define CE_RECORDING_STOPPING 3   /* stop accepted; next state IDLE. Finalization is a separate event */

#define CE_MODE_VIDEO      1
#define CE_MODE_AUDIO_ONLY 2

#define CE_HELPER_INJECT  (1u << 0)
#define CE_HELPER_MEDIA   (1u << 1)
#define CE_HELPER_LOGGER  (1u << 2)
#define CE_HELPER_SENSORS (1u << 3)

typedef struct ce_status_info {
    uint32_t struct_size;
    int32_t  runtime_state;     /* CE_RUNTIME_* */
    int32_t  recording_state;   /* CE_RECORDING_* */
    int32_t  recording_mode;    /* CE_MODE_* or 0 */
    uint64_t recording_id;      /* current/last recording, 0 = none */
    uint32_t helpers_ready;     /* CE_HELPER_* bits */
    uint32_t finalizing_count;  /* recordings whose output is still being finalized */
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
#define CE_EVENT_RUNTIME_STATE       1
#define CE_EVENT_COMMAND_COMPLETED   2
#define CE_EVENT_RECORDING_STATE     3
#define CE_EVENT_RECORDING_FINALIZED 4
#define CE_EVENT_SCREENSHOT          5
#define CE_EVENT_SETTINGS_CHANGED    6
#define CE_EVENT_SETTINGS_REJECTED   7   /* external file edit failed validation; previous stays */
#define CE_EVENT_HELPER              8
#define CE_EVENT_HOTKEY              9   /* informational: a runtime-owned hotkey fired */
#define CE_EVENT_EVENTS_DROPPED      10

#define CE_HELPER_STATE_READY     1
#define CE_HELPER_STATE_LOST      2
#define CE_HELPER_STATE_RECOVERED 3

#define CE_SETTINGS_SOURCE_API  1
#define CE_SETTINGS_SOURCE_FILE 2

#define CE_HOTKEY_RECORD     1
#define CE_HOTKEY_AUDIO_ONLY 2
#define CE_HOTKEY_SCREENSHOT 3
#define CE_HOTKEY_OVERLAY    4
#define CE_HOTKEY_BENCHMARK  5

typedef struct ce_event {
    uint32_t     struct_size;   /* runtime-allocated; >= what the client compiled against */
    int32_t      type;          /* CE_EVENT_*; ignore unknown */
    uint64_t     sequence;      /* strictly increasing per runtime */
    int64_t      time_us;       /* runtime monotonic clock, microseconds */
    ce_request_t request;       /* originating request or 0 */
    ce_status_t  status;        /* outcome for COMMAND_COMPLETED / FINALIZED / SCREENSHOT */
    uint32_t     reserved0;
    const char*  message;       /* UTF-8 diagnostic or NULL; valid until released */
    union {
        struct { int32_t state; } runtime;
        struct { uint64_t recording_id; int32_t state; int32_t mode; uint32_t failure; } recording;
        struct { uint64_t recording_id; const char* output_path; } finalized;
        struct { const char* const* paths; uint32_t path_count; } screenshot;
        struct { uint64_t revision; int32_t source; } settings;
        struct { uint32_t helper; int32_t state; } helper;
        struct { int32_t action; } hotkey;
        struct { uint64_t count; } dropped;
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
#define CE_DIAG_ERROR   2

typedef struct ce_settings_diagnostic {
    uint32_t    struct_size;
    int32_t     severity;      /* CE_DIAG_* */
    const char* section;       /* UTF-8; valid until the edit is discarded */
    const char* key;
    const char* message;
} ce_settings_diagnostic_t;

/* Copies the effective stored value (not defaults) into buf. NULL buf queries *needed. */
CE_API ce_status_t ce_settings_get(ce_runtime_t* rt, const char* section, const char* key,
                                   char* buf, size_t capacity, size_t* needed);
CE_API ce_status_t ce_settings_begin(ce_runtime_t* rt, ce_settings_edit_t** out_edit);
/* value NULL removes the key. Section "" = global section. */
CE_API ce_status_t ce_settings_set(ce_settings_edit_t* edit, const char* section, const char* key,
                                   const char* value);
/* Validates synchronously. CE_E_SETTINGS -> read diagnostics, edit stays open for fixes.
 * CE_E_CONFLICT -> discard and begin again. CE_OK -> queued; COMMAND_COMPLETED then
 * SETTINGS_CHANGED. The edit is consumed on CE_OK. */
CE_API ce_status_t ce_settings_commit(ce_settings_edit_t* edit, ce_request_t* out);
CE_API ce_status_t ce_settings_diagnostic_count(const ce_settings_edit_t* edit, uint32_t* count);
CE_API ce_status_t ce_settings_get_diagnostic(const ce_settings_edit_t* edit, uint32_t index,
                                              ce_settings_diagnostic_t* out);
CE_API void        ce_settings_discard(ce_settings_edit_t* edit);

/* ---- Platform setup (M9) ------------------------------------------------------------ */
#define CE_SETUP_SENSOR_DRIVER_INSTALL     1
#define CE_SETUP_SENSOR_DRIVER_UNINSTALL   2
#define CE_SETUP_ELEVATION_SERVICE_INSTALL 3
#define CE_SETUP_ELEVATION_SERVICE_REMOVE  4
#define CE_SETUP_CLIENT_AUTOSTART_ON       5   /* registers the calling executable */
#define CE_SETUP_CLIENT_AUTOSTART_OFF      6

typedef struct ce_setup_status {
    uint32_t struct_size;
    uint32_t sensor_driver_installed;  /* 0/1 */
    int32_t  elevation_service_state;  /* 0 absent, 1 installed, 2 running, -1 unknown */
    uint32_t client_autostart;         /* 0/1 */
} ce_setup_status_t;

CE_API ce_status_t ce_setup_request(ce_runtime_t* rt, int32_t action, ce_request_t* out);
CE_API ce_status_t ce_setup_get_status(ce_runtime_t* rt, ce_setup_status_t* out);

/* ---- Misc (M9) ---------------------------------------------------------------------- */
typedef struct ce_monitor_info {
    char    id[256];      /* value usable as capture_monitor=id:<...> */
    char    name[128];    /* UTF-8 friendly name */
    int32_t left, top, right, bottom;
    uint32_t primary;     /* 0/1 */
} ce_monitor_info_t;

/* item_size = sizeof(ce_monitor_info_t). Fills up to capacity, reports total in *count. */
CE_API ce_status_t ce_enumerate_monitors(ce_monitor_info_t* items, uint32_t item_size,
                                         uint32_t capacity, uint32_t* count);

#define CE_LOG_ERROR 1
#define CE_LOG_WARN  2
#define CE_LOG_INFO  3
#define CE_LOG_DEBUG 4
/* Writes one line into the runtime's log, tagged with client_name. */
CE_API ce_status_t ce_runtime_log(ce_runtime_t* rt, int32_t level, const char* message);

#pragma pack(pop)

#ifdef __cplusplus
}
#endif
#endif /* CENGINE_H */
```

Notes on the draft:
- Public layouts use Windows x64 packing/alignment with an 8-byte maximum; the header saves and
  restores the client's surrounding packing. M2 tests both ordinary and nondefault ambient packing.
- `ce_status_info_t` and `ce_setup_status_t` are caller-allocated, so the client sets `struct_size`
  and the runtime fills only the fields it knows (min of both sizes).
- `ce_event_t` is runtime-allocated. The client checks `struct_size` before reading appended fields.
- Monitors use `item_size` because an array can't carry a per-item `struct_size` cleanly.
- Header sections marked (M6)/(M9) are added to the shipped header only when that milestone lands.
  M2 commits the full draft as `cengine_draft.h` for ABI tests; the shipped header grows by milestone.

## Semantics in detail

### Lifecycle

```
create ──► STARTING ──(inject helper ready, desktop/hotkey components started)──► READY
              │                                                                    │
              └──(package/settings/helper failure)──► FAILED                       │
READY ──request_shutdown──► STOPPING ──(recording stopped, helpers shut down)──► STOPPED
FAILED/STOPPED ──destroy──► (handle invalid)
```

- `create` does synchronously: argument/version checks, package validation (`CE_E_PACKAGE`),
  settings load (an unreadable file falls back to defaults or the template, exactly as today), the
  session claim (`CE_E_BUSY`), and the engine thread start. Everything slow (helper spawn up to 10 s)
  happens on the engine thread.
- READY requires the inject helper to be ready. A logger or sensor failure is not fatal: it produces a
  `CE_EVENT_HELPER` and READY is still reached (today's behavior, `CompleteControllerStartup`).
- Commands in STARTING return `CE_E_INVALID_STATE`. Settings get/begin/commit work in STARTING.
- Shutdown order and budgets are today's, see 04 §core. When the shutdown budget is exceeded,
  helpers that are still finalizing stay owned. `ce_runtime_shutdown` returns `CE_E_TIMEOUT` and the
  handle stays valid. Today's forced-termination fallback is applied only by the **frontend** policy
  (it calls `shutdown` again with a final budget), never silently by the library.
- `destroy` frees memory, releases the session claim, joins threads and unpins the module.

### Recording

- `recording_id` = `RecordingSnapshot::request` (controller request identity). It is monotonically
  increasing per runtime. The diagnostic `g_RecordingId` (`r0001` ...) appears only in log messages.
- State events: IDLE→STARTING (accepted), STARTING→LIVE (media observed live), STARTING/LIVE→STOPPING
  (stop accepted), STOPPING→IDLE (child stop acknowledged/retired), any→IDLE with `failure != 0`
  (failure reconciliation).
- `CE_EVENT_RECORDING_FINALIZED` fires once per recording whose media helper finished finalizing,
  with status and output path. Finalization can overlap the next recording's STARTING (retired media
  finalizer, verified behavior of `HostChildrenSession`). Implementing it needs a media → runtime
  result channel (M6; see 04 §recording and 08 Q3).
- Toggle semantics are exactly `RecordingSession::Toggle`. Clients never reimplement them.
- Live streaming is selected by settings (`IsLiveStreamTarget(video.outputDir)`, verified). There is
  no separate API mode.

### Events

- Bounded queue (default 1024). On overflow, the newest STATE event of each type is kept (coalesced)
  and the dropped notices are counted. The next delivered event is `CE_EVENT_EVENTS_DROPPED` with
  the count. COMMAND_COMPLETED and FINALIZED are never dropped: their memory is reserved at admission,
  and a command is rejected with `CE_E_NO_MEMORY` when no reservation is possible.
- The engine thread never blocks on the client. A client that never reads only loses coalescible
  events.
- `release_event` is required. Unreleased events count against the queue budget and are freed at
  destroy.

### Settings

- Section/key names are those of `config.ini` (and `config.ini.template`). Profile sections use the
  same section names as in the file. Values are text, exactly as written to the INI.
- `begin` captures the base revision and the file identity. `commit`:
  1. applies the edits to a copy of the current document text, preserving comments, order and
     unknown lines;
  2. parses the candidate with the production loader in diagnostic mode (unknown key = warning,
     out-of-range/clamped = warning, unparsable = error);
  3. rejects on any error;
  4. checks the identity again (`CE_E_CONFLICT` if the file changed);
  5. queues publication on the engine thread, which writes a temp file in the same directory and
     atomically replaces the original, records the new identity as a self-write, publishes the new
     snapshot, fans out to components, and broadcasts `ReloadConfig` to helpers.
- With `CE_FEATURE_WATCH_SETTINGS`, external edits are applied as today (coherent replacement and
  debounce in `ConfigurationState`). Invalid external edits produce `CE_EVENT_SETTINGS_REJECTED` and
  keep the previous snapshot.
- The settings file is `<package_dir>\config.ini` in v2.0. A configurable path is deferred until the
  hook can follow it (08 Q1). The app's existing `--config=` keeps its current behavior.

### Strings and memory

- All strings are UTF-8 in both directions. Paths are converted to UTF-16 inside, and the runtime
  never uses ANSI APIs for client-supplied paths. Existing ANSI/8.3 handling stays inside the
  runtime/helpers.
- Input strings are copied before an accepting call returns. Request output pointers may be NULL;
  accepted commands still receive an id and exactly one completion. Failed output-handle/event calls
  clear the corresponding output pointer; failed admissions set a supplied request id to zero.
- Pointers inside events are valid until `ce_runtime_release_event`. Pointers inside diagnostics
  are valid until `ce_settings_discard` or a successful commit.
- Handles are validated against a live registry, so a stale handle returns `CE_E_INVALID_ARGUMENT`
  instead of crashing. Calling `destroy` concurrently with other calls on the same handle is a client
  error (documented), not supported.

### Forbidden contexts

- Not callable from `DllMain` or from TLS callbacks.
- Not callable from a process that is a CaptureEngine helper or an injected game (the hook DLL
  doesn't link it).

## C++ wrapper (`include/cengine/cengine.hpp`, header-only)

Purpose: RAII and type safety for C++ clients, **the frontend included** (dogfooding). It calls
only the C ABI. It has no exceptions across the boundary, and STL types appear only in client-side
code.

Wrapper operations return `Status` and output objects so timeout, missing settings and other errors
remain distinguishable. Events/edits keep runtime ownership alive; explicit destroy rejects outstanding
wrapper objects. Cleanup is bounded, never force-terminates helpers, and may leave a C runtime retained
after timeout: call shutdown explicitly and retain the wrapper to inspect errors and retry. Client-side
string/vector allocation may throw; no C++ exception crosses the DLL boundary. String views containing
embedded NULs are rejected rather than truncated. Empty event `raw()` returns NULL.

```cpp
namespace cengine {
class Status { /* wraps ce_status_t; ok(), code(), message() via ce_status_string */ };

class Event {  // move-only; releases on destruction
public:
    int type() const; uint64_t sequence() const; ce_request_t request() const; Status status() const;
    const ce_event_t* raw() const;
};

class SettingsEdit {  // move-only; discards on destruction unless committed
public:
    Status set(std::string_view section, std::string_view key, std::optional<std::string_view> value);
    Status commit(ce_request_t* request = nullptr);
    Status diagnostics(std::vector<Diagnostic>& out) const;
};

class Runtime {  // move-only
public:
    static Status create(const Options&, Runtime& out);
    ~Runtime();                                   // shutdown(kDefaultBudget) then destroy; never throws
    Status shutdown(std::chrono::milliseconds budget);
    StatusInfo status() const;
    Status startRecording(Mode, std::string_view reason, ce_request_t* = nullptr);
    Status stopRecording(std::string_view reason, ce_request_t* = nullptr);
    Status toggleRecording(Mode, std::string_view reason, ce_request_t* = nullptr);
    Status toggleOverlay(ce_request_t* = nullptr);
    Status toggleBenchmark(ce_request_t* = nullptr);
    Status takeScreenshot(ce_request_t* = nullptr);
    Status launch(std::string_view commandLine, ce_request_t* = nullptr);
    Status nextEvent(std::chrono::milliseconds timeout, Event& out);
    HANDLE eventHandle() const;
    Status editSettings(SettingsEdit& out);
    Status setting(std::string_view section, std::string_view key, std::string& out) const;
    Status log(LogLevel, std::string_view message);
};
}  // namespace cengine
```

## Mapping from v1 (`include/libcaptureengine.h`)

| v1 | v2 | Note |
| --- | --- | --- |
| `ce_engine_config_init_default` / `ce_engine_create` (attach to controller) | `ce_runtime_desc_init` / `ce_runtime_create` (owns runtime) | v1 attach semantics dropped |
| `ce_engine_destroy` (detach, runtime continues) | `ce_runtime_shutdown` + `ce_runtime_destroy` | |
| `enable_hotkeys`, `enable_system_tray`, `start_minimized` | `CE_FEATURE_GLOBAL_HOTKEYS`; tray is frontend-only; no minimized concept | |
| `ce_engine_start/stop/toggle_recording`, `toggle_audio_only` | `ce_recording_start/stop/toggle(mode)` | |
| `ce_engine_toggle_overlay/benchmark`, `take_screenshot` | same names under `ce_overlay_/ce_benchmark_/ce_screenshot_` | async with completion |
| `ce_engine_is_recording` | `ce_runtime_get_status` | state, not just a bool |
| `ce_engine_get_recording_stats` (unsupported) | removed until M11 implements it | |
| `ce_engine_poll_events` (pumps controller messages) | `ce_runtime_next_event` / `ce_runtime_event_handle` | client loop no longer pumps engine messages |
| `log_level`, `log_directory`, `config_file_path` | settings (`log_level` key), `data_dir`, fixed package config | |

v1 has no external consumer, so it is deleted in M4 in the same commit that moves the frontend to
v2. There is no compatibility shim.

## Minimal headless client (must compile against shipped artifacts only; M8)

```c
#include <cengine/cengine.h>
#include <stdio.h>

int main(void) {
    ce_runtime_desc_t desc; ce_runtime_desc_init(&desc);
    desc.client_name = "headless-sample";
    ce_runtime_t* rt = NULL;
    ce_status_t s = ce_runtime_create(&desc, &rt);
    if (s != CE_OK) { fprintf(stderr, "create: %s\n", ce_status_string(s)); return 1; }

    int ready = 0, done = 0; ce_request_t start = 0, stop = 0;
    while (!done) {
        const ce_event_t* ev = NULL;
        if (ce_runtime_next_event(rt, 1000, &ev) != CE_OK) continue;
        switch (ev->type) {
        case CE_EVENT_RUNTIME_STATE:
            if (ev->data.runtime.state == CE_RUNTIME_READY && !ready) {
                ready = 1; ce_recording_start(rt, CE_MODE_VIDEO, "sample", &start);
            } else if (ev->data.runtime.state == CE_RUNTIME_FAILED) done = 1;
            break;
        case CE_EVENT_RECORDING_STATE:
            if (ev->data.recording.state == CE_RECORDING_LIVE) ce_recording_stop(rt, "sample", &stop);
            break;
        case CE_EVENT_RECORDING_FINALIZED:
            printf("finalized: %s\n", ev->data.finalized.output_path ? ev->data.finalized.output_path : "?");
            done = 1; break;
        }
        ce_runtime_release_event(rt, ev);
    }
    if (ce_runtime_shutdown(rt, 30000) == CE_OK) ce_runtime_destroy(rt);
    return 0;
}
```
