# CaptureEngine SDK

`cengine.dll` is the CaptureEngine runtime: the helper processes, recording, hotkeys, the desktop
overlay, screenshots, game launching with injection, the settings, platform setup (sensor driver,
elevation service, autostart) and monitor enumeration. The CaptureEngine app is one client of it; this
folder lets you write another.

| File | Use |
| --- | --- |
| `include/cengine/cengine.h` | The C API (C11 and C++). Everything a client may call. |
| `include/cengine/cengine.hpp` | Optional header-only C++17 wrapper over the C API. |
| `lib/libcengine.dll.a` | Import library for clang/MinGW linkers. |
| `lib/cengine.lib` | Import library for MSVC `link.exe` / `lld-link`. |
| `lib/cengine.def` | The export list (the same functions as `cengine.h`). |
| `samples/headless/main.c` | A complete headless client: start, record, finalize, shut down. |

Windows x64 only. Build a client with, for example,
`clang -std=c11 -I sdk/include main.c sdk/lib/libcengine.dll.a -o myclient.exe`.

## Deployment

The runtime runs from a complete CaptureEngine installation: `cengine.dll` starts
`captureengine.exe` from its own folder for its helper processes and reads `config.ini` there
(`ce_runtime_desc_t.settings_path` names another settings file). Put your executable into the
CaptureEngine folder, next to `cengine.dll`: game injection and the setup actions still locate their
files beside the running executable. The DLL version must match the installation it sits in; do not
copy it elsewhere.

## Contract in brief

- **One runtime per Windows session.** `ce_runtime_create` claims the session; while another
  runtime (for example the CaptureEngine app) holds it, create returns `CE_E_BUSY`.
- **Lifecycle.** create (STARTING) → `CE_EVENT_RUNTIME_STATE` READY or FAILED →
  `ce_runtime_request_shutdown` / `ce_runtime_shutdown` → STOPPED → `ce_runtime_destroy`. Destroy is
  only allowed in STOPPED or FAILED. A runtime can be created again in the same process after destroy.
- **Shutdown waits for the helpers.** The runtime never terminates a helper; a recording that is still
  being written is finalized completely. `ce_runtime_shutdown` returns `CE_E_TIMEOUT` when your
  budget runs out; the handle stays valid and you may wait again.
- **Commands never block.** Every command returns at once with a request id; exactly one
  `CE_EVENT_COMMAND_COMPLETED` follows for every accepted request. Commands before READY are refused.
- **Events.** Read them with `ce_runtime_next_event` (or wait on `ce_runtime_event_handle`) and
  release each with `ce_runtime_release_event`. A finished recording arrives as
  `CE_EVENT_RECORDING_FINALIZED` with its status and output path.
- **Threads.** Every function may be called from any thread. The runtime does its work on its own
  thread and never calls back into the client.
- **Strings** are UTF-8, in and out. Pointers in an event stay valid until the event is released.
- **Errors** are `ce_status_t` codes; `ce_status_string` names them. No C++ exception crosses the API.
- **Versioning.** `CE_API_VERSION` is major.minor; a runtime serves clients of the same major and an
  equal or older minor. Structs carry `struct_size`; initialize descriptors with
  `ce_runtime_desc_init`.

## Features

`ce_runtime_desc_t.features` opts into process-wide behavior: `CE_FEATURE_GLOBAL_HOTKEYS` (the
configured hotkeys), `CE_FEATURE_DESKTOP_OVERLAY` (the desktop status overlay),
`CE_FEATURE_CRASH_HANDLER` (CaptureEngine's crash dumps for your process; the handler stays installed
and `cengine.dll` stays loaded for the rest of the process) and
`CE_FEATURE_WATCH_SETTINGS` (apply edits of the settings file while running). Without them the runtime
only does what you ask for.
