# CaptureEngine SDK - developer guide

`cengine.dll` is the CaptureEngine runtime: helper processes, recording, hotkeys, the desktop
overlay, screenshots, game launching with injection, settings, platform setup (sensor driver,
elevation service, autostart) and monitor enumeration. The CaptureEngine app is one client of it;
this folder lets you write another. Windows x64 only.

| File | Use |
| --- | --- |
| `include/cengine/cengine.h` | The C API (C11 and C++). Everything a client may call, documented inline. |
| `include/cengine/cengine.hpp` | Optional header-only C++17 wrapper (RAII handles, `Status`, `Event`). |
| `lib/cmake/cengine/` | CMake package: `find_package(cengine 2.0 REQUIRED)` gives the target `cengine::cengine`. |
| `lib/libcengine.dll.a` | Import library for clang/MinGW linkers. |
| `lib/cengine.lib` | Import library for MSVC `link.exe` / `lld-link`. |
| `lib/cengine.def` | The export list (the same functions as `cengine.h`). |
| `samples/headless/` | A complete headless client (`main.c` + `CMakeLists.txt`): start, record, finalize, shut down. |

## Getting started

### Deployment

The runtime runs from a complete CaptureEngine installation: `cengine.dll` starts the helper processes
from its own folder and reads `config.ini` there (`ce_runtime_desc_t.settings_path` names another
settings file). **Your executable must live in the CaptureEngine folder, next to `cengine.dll`**: game
injection and the setup actions still locate their files beside the running executable. The DLL must
match the installation it sits in; never copy it elsewhere. For development, install CaptureEngine into a
folder you can write to and put your build output there.

### CMake

```cmake
cmake_minimum_required(VERSION 3.16)
project(myclient C)
find_package(cengine 2.0 REQUIRED)  # CMAKE_PREFIX_PATH=<CaptureEngine>/sdk
add_executable(myclient main.c)
target_link_libraries(myclient PRIVATE cengine::cengine)
# cengine_PACKAGE_DIR is the CaptureEngine folder the executable has to run from.
```

The package selects `cengine.lib` for MSVC and `libcengine.dll.a` for clang/MinGW, refuses 32-bit
builds, and accepts any 2.x request up to the installed minor.

### Command line

```
clang -std=c11 -I sdk/include main.c sdk/lib/libcengine.dll.a -o myclient.exe
cl /std:c11 /I sdk\include main.c sdk\lib\cengine.lib
```

## Concepts

- **One runtime per Windows session.** `ce_runtime_create` claims the session; while another runtime
  (for example the CaptureEngine app) holds it, create returns `CE_E_BUSY`.
- **Lifecycle.** create → `CE_EVENT_RUNTIME_STATE` READY (or FAILED) → `ce_runtime_request_shutdown` or
  `ce_runtime_shutdown(budget)` → STOPPED → `ce_runtime_destroy`. Destroy is allowed only in STOPPED or
  FAILED, right after either the STOPPED event or a successful `ce_runtime_shutdown`. A new runtime can
  be created in the same process afterwards.
- **Shutdown waits for the helpers.** The runtime never terminates a helper: a recording that is still
  being written is finalized completely. `ce_runtime_shutdown` returns `CE_E_TIMEOUT` when your budget
  runs out; the handle stays valid and you may wait again.
- **Commands never block.** Every command returns at once. When it is admitted, `*out` receives a request
  id and exactly one `CE_EVENT_COMMAND_COMPLETED` with that id follows. Commands before READY are refused
  (`CE_E_INVALID_STATE`, request id 0).
- **Events are pulled**, never pushed into your code: `ce_runtime_next_event(rt, timeout_ms, &event)`, or
  wait on `ce_runtime_event_handle(rt)` (a Win32 event, signaled while events are pending) in your own
  message loop. Release every event with `ce_runtime_release_event`. Unknown event types: ignore them.
- **Threads.** Every function may be called from any thread. The runtime does its work on its own thread.
- **Memory and strings.** Strings are UTF-8 in and out. Pointers inside an event stay valid until you
  release it. Variable-size results use your buffer plus a `needed` size (`ce_settings_get`).
- **Errors.** Every function returns a `ce_status_t`; `ce_status_string` names it; the event `message`
  and the runtime log carry detail. No C++ exception crosses the API.
- **Features** opt into process-wide behavior: `CE_FEATURE_GLOBAL_HOTKEYS` (the configured hotkeys),
  `CE_FEATURE_DESKTOP_OVERLAY`, `CE_FEATURE_CRASH_HANDLER` (CaptureEngine's crash dumps for your process;
  the handler and `cengine.dll` stay for the rest of the process) and `CE_FEATURE_WATCH_SETTINGS` (apply
  edits of the settings file while running). Without them the runtime only does what you ask.
- **Helpers and crashes (API 2.1).** The helper roles (game injection, capture and encoding, log collection) run
  inside your process, on threads the runtime owns: no CaptureEngine process is needed. A crash in capture or
  encoding therefore ends your process. `CE_FEATURE_ISOLATED_HELPERS` runs them as separate `captureengine.exe`
  processes instead, so a crash ends only the helper (the CaptureEngine app does this). The settings' `[Runtime]
  isolated_helpers=on|off` overrides your choice either way; `auto`, the default, follows it. In both
  modes, recording one application's audio starts a short-lived `captureengine.exe` worker per captured
  application, and hardware sensors run in their own process. While recording the runtime raises the timer
  resolution to 1 ms (per process on Windows 11). The runtime never changes your console handlers, working set,
  DLL search order or COM security.
- **`CE_FEATURE_PROCESS_TUNING` (API 2.1)** lets the settings' process priority class and GPU scheduling
  priority apply to your process while recording. Without it your process keeps its own priorities.
- **Logs** go to `<CaptureEngine>\logs\<session>\`; `ce_runtime_log` adds your own lines, prefixed with
  your client name.

## Recipes (C)

### Start and wait until ready

```c
ce_runtime_desc_t desc;
ce_runtime_desc_init(&desc, sizeof desc, CE_API_VERSION);
desc.client_name = "myclient";
ce_runtime_t* rt = NULL;
ce_status_t s = ce_runtime_create(&desc, &rt);
if (s != CE_OK) { fprintf(stderr, "create: %s\n", ce_status_string(s)); return 1; }

const ce_event_t* ev = NULL;
int ready = 0;
while (!ready && ce_runtime_next_event(rt, 60000, &ev) == CE_OK) {
    if (ev->type == CE_EVENT_RUNTIME_STATE)
        ready = ev->data.runtime.state == CE_RUNTIME_READY ? 1 : (ev->data.runtime.state == CE_RUNTIME_FAILED ? -1 : 0);
    ce_runtime_release_event(rt, ev);
}
```

### Record, then get the file

```c
ce_request_t start = 0;
ce_recording_start(rt, CE_MODE_VIDEO, "my reason", &start);      /* or CE_MODE_AUDIO_ONLY */
/* ... COMMAND_COMPLETED{request == start}, RECORDING_STATE LIVE (data.recording.recording_id) ... */
ce_recording_stop(rt, "done", NULL);
/* ... CE_EVENT_RECORDING_FINALIZED: status, data.finalized.recording_id, data.finalized.output_path */
```

`ce_recording_toggle` applies the app's hotkey semantics (stop while starting, mode switch). A new
recording may start while the previous one is still being finalized; match FINALIZED by
`recording_id`. `RECORDING_STATE.failure` is 1 for a generic and 2 for an integrity failure.

### Screenshot

`ce_screenshot_take(rt, &request)`, then `CE_EVENT_SCREENSHOT` for that request: `status`, and
`data.screenshot.paths[0 .. path_count-1]` (the HDR variant first when one was written).

### Settings

```c
char value[64]; size_t needed = 0;
s = ce_settings_get(rt, "Video", "fps", value, sizeof value, &needed);  /* CE_E_NOT_FOUND = not in the file */

ce_settings_edit_t* edit = NULL;
ce_settings_begin(rt, &edit);
ce_settings_set(edit, "Video", "fps", "60");                 /* value NULL removes the key */
s = ce_settings_commit(edit, &request);                     /* consumes the edit on CE_OK */
if (s == CE_E_SETTINGS) { /* ce_settings_diagnostic_count/_get_diagnostic, fix, commit again */ }
if (s == CE_E_CONFLICT) { ce_settings_discard(edit); /* the file changed meanwhile: begin again */ }
```

Section and key names are those of `config.ini`. A commit is validated on your thread and applied by the
runtime: `COMMAND_COMPLETED`, then `CE_EVENT_SETTINGS_CHANGED`. External edits refused by validation arrive
as `CE_EVENT_SETTINGS_REJECTED` (with `CE_FEATURE_WATCH_SETTINGS`).

### Launch a game with injection

`ce_target_launch(rt, "\"C:\\Games\\game.exe\" -arg", &request)`: starts the program the way
CaptureEngine's `--launch` does (suspended and hooked before it runs, or a launcher passed through).

### Platform setup and monitors

```c
ce_setup_status_t st = { sizeof st };
ce_setup_get_status(rt, &st);                         /* any thread */
if (st.sensor_driver_offer)                           /* the runtime decides; you only ask the user */
    ce_setup_request(rt, CE_SETUP_SENSOR_DRIVER_INSTALL, &request);  /* may show UAC; CE_E_CANCELED if declined */

ce_monitor_info_t mons[8]; uint32_t count = 0;        /* no runtime needed */
ce_enumerate_monitors(mons, sizeof mons[0], 8, &count); /* count = total; ids usable as capture_monitor=id:... */
```

### C++

```cpp
#include <cengine/cengine.hpp>
cengine::Runtime rt;
cengine::Options options;
options.clientName = "myclient";
if (auto s = cengine::Runtime::create(options, rt); !s) return s.code();
cengine::Event ev;
while (rt.nextEvent(std::chrono::seconds(60), ev) && ev.type() != CE_EVENT_RUNTIME_STATE) {}
rt.startRecording(cengine::Mode::Video, "my reason");
```

## Compatibility and versioning

- `CE_API_VERSION` is major.minor (currently 2.1). A runtime serves clients of the same major and an
  equal or older minor; check `ce_api_version()` at startup if you load the DLL dynamically. Minor 1 added
  the `CE_FEATURE_ISOLATED_HELPERS` and `CE_FEATURE_PROCESS_TUNING` bits.
- Structs carry `struct_size`. New fields are only ever appended (with a minor bump); initialize
  descriptors with `ce_runtime_desc_init` and pass `sizeof` of your struct, never a constant.
- Status codes, event types, modes and setup actions keep their values; new ones are appended. Treat
  unknown values as "not for me".
- A function stays for the whole major version. Deprecations are marked in `cengine.h` and announced in
  the CaptureEngine changelog; removal happens only with the next major.

## Troubleshooting

| Result | Meaning |
| --- | --- |
| `CE_E_BUSY` from create | Another runtime (often the CaptureEngine app) holds this Windows session. |
| `CE_E_PACKAGE` | `captureengine.exe` is missing beside `cengine.dll`, or the folder path cannot be expressed in the Windows code page and has no 8.3 name. |
| `CE_E_VERSION` | Your header is newer than the installed runtime (or another major). |
| `CE_E_INVALID_STATE` | A command before READY or after shutdown began, or destroy before STOPPED. |
| `CE_E_TIMEOUT` from shutdown | Helpers are still finishing (for example a large recording); wait again. |

The runtime's log (`<CaptureEngine>\logs\<session>\captureengine.log`) records every refusal with its reason.

## Current limits

- Your executable has to run from the CaptureEngine folder (see Deployment).
- The engine's work runs in CaptureEngine helper processes started by `cengine.dll`. A later version
  runs it inside your process by default, with the helper processes as an option.
- No access to captured frames or audio samples: the output is files (or a live stream chosen by settings).
- Recording parameters come from the settings; a recording command takes only the mode.
