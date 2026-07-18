# Process IPC And Restricted Children

Last cross-checked: 2026-07-18 (restricted process channels, accept-before-finalize media stops, private same-image process-loopback workers, GUI launch-feedback suppression, and exact shared-memory ABI/build publication/isolation)

Primary sources:
- `common/restricted_child_process.{h,cpp}`
- `common/process_ipc.{h,cpp}`
- `common/shared_defs.h`
- `captureengine/{main,inject_main,media_main,limiter_main}.cpp`
- `captureengine/{ipc,logger_service,sensor_service}.cpp`
- `hook/common/ipc_client.cpp`
- `tests/test_process_ipc.cpp`
- `tests/{test_shared_runtime_state,test_logger_service_policy}.cpp`

## Summary

Controller-to-inject/media/limiter commands use a private channel created for each spawned child. The controller creates a unique connected duplex named-pipe pair, marks only the child endpoint inheritable, and launches through `RestrictedChildProcess` with `STARTUPINFOEX` plus `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`. Production children no longer reconnect to a public fixed pipe name. A broken channel is terminal for that child instance and the controller performs a clean respawn.

An ordinary video or audio-only stop is an authenticated media command, not an intentional broken pipe. Media clears every hook-facing recording flag and any older shared stop/start command, acknowledges acceptance before encoder/mux finalization, then finalizes and exits as a disposable child. The controller can release its endpoint immediately without blocking tray/hotkey work on trailer writing or the post-mux probe. It asks inject to publish the shared-memory stop only when media did not accept the private command. Therefore `media channel failed during peek` remains evidence of a real channel failure, not normal stop noise, and an inject fallback cannot leave a stale `cmdStopRecording` for the next media child.

The pipe security descriptor grants access only to SYSTEM and the current user. The child verifies that the inherited pipe's server PID is the controller. The controller accepts startup only when the versioned handshake contains the exact spawned child PID, expected mode, and per-channel cryptographically generated 128-bit nonce.

The separate high-volume inject shared-memory transport is also an exact internal ABI, not a backward-compatible prefix protocol. ABI 36 uses version-isolated main/discovery mapping names and publishes magic only after construction and full header/config initialization. Consumers must validate exact version, `sizeof(SharedMemoryLayout)`, and a compiled fingerprint of major/nested sizes and offsets before dereferencing any payload.

CaptureEngine is linked as a Windows GUI-subsystem image even when the same executable runs a windowless internal role. Every restricted internal spawn therefore sets `STARTF_FORCEOFFFEEDBACK`; `CREATE_NO_WINDOW` alone does not suppress GUI process-start cursor feedback. This covers ordinary media/logger/sensor/limiter/inject children and disposable process-loopback workers, while explicit user game/launcher starts retain normal Windows feedback. The controller also clears any external launch feedback at its earliest controller-only dispatch and creates its hidden tray owner before Vulkan registration or child startup can extend the busy cursor. The hook's hidden same-image dump helper uses the same feedback opt-out.

## Shared-Memory Contract

- Owners use placement construction so every interprocess atomic has begun its C++ lifetime; zeroing raw atomic storage is forbidden.
- `magic` is the publication barrier and is stored last. The header also carries exact version, size, and ABI fingerprint. Main mappings are exclusive; an existing PID/version mapping is a startup failure rather than memory to overwrite.
- The fixed discovery mapping is version-isolated and carries `BUILD_NUMBER`. Every consumer requires the exact published build; this keeps older same-ABI hooks/layers dormant when multiple installation paths coexist. A live advertised inject PID blocks a second owner; a mapping retained by a client after its owner exited is withdrawn and safely reused. Shutdown withdraws discovery before closing the main mapping.
- Controller, media, limiter, sensor, logger, pseudo-overlay, screenshot, hook, and Vulkan-layer entry points validate the same header. A mismatched binary fails closed and logs its observed/expected header instead of reading shifted telemetry, frame, or log fields.
- Incremental compile signatures include project dependency-header contents as well as source/compiler/flags. This prevents a checkout/restore with older header mtimes from reusing objects compiled against another shared layout.

## Shared Rings And Session Logs

- Shared log producers reserve slots with a CAS on the monotonic write index, write the bounded slot, then release-publish its committed flag. The host logger stops at the first uncommitted reservation and clears slots after append.
- Logger output uses the immutable per-session `DiscoveryInfo.logsPath`; executable-relative `logs` is only a no-discovery fallback. The filename prefix is copied into owned storage and restricted to a single safe basename, so every accepted IPC log stays inside the corresponding session ID directory.
- Integration summaries default to the latest corresponding session directory rather than the global log root. An explicit `--results-json` path remains an intentional caller override.
- The frame-ring producer window must remain at most `FRAME_RING_SIZE`. Media refuses an inject start/reset with a corrupt window and publishes a recording-integrity failure instead of silently creating a zero-frame recording.
- Vulkan layer-owned and encoder-owned producer pools use all 16 shared texture lease slots. Four slots can deadlock when the media scheduler retains five or more frames for measured loopback A/V delay and fence headroom: every texture remains leased while the consumer waits for one additional frame. The integration runner therefore requires final nonzero encoder output statistics; overlay/performance samples alone cannot prove recording success.

## Message Contract

Protocol 2 has an exact 52-byte packed header and at most 256 payload bytes. Validation covers magic, version, header size, exact read and total sizes, message kind, opcode, sender mode, sender PID, sequence, nonce, payload termination, and opcode-specific payload shape. Short, oversized, stale, unknown, mismatched, and malformed messages are rejected. Invalid-message logging is rate limited.

`StopRecording` accepts `Ack` as an acceptance response as well as the legacy completion response. The disposable media child intentionally uses `Ack` before finalization; completion is proven by its finalization logs/process exit rather than by holding the controller command call open.

The private pipe name is only a transient rendezvous used while the controller already holds the connected endpoint; it is not a stable production API. The child receives the endpoint handle, controller PID, and nonce on its command line and strictly parses their complete values before using the channel.

## Child Launcher Invariants

- Only handles listed in the attribute list are inherited.
- Invalid handles, attribute-list setup failure, or process creation failure abort the spawn and close owned resources.
- The same launcher is used by normal process IPC and the private disposable `captureengine.exe --process-loopback-worker` boundary; the latter inherits exactly its mapping, packet event, and stop event.
- Restricted internal children are noninteractive GUI-subsystem processes and must launch with `STARTF_FORCEOFFFEEDBACK`. Do not substitute `CREATE_NO_WINDOW`, which Windows ignores for GUI applications, and do not apply this policy to an explicitly user-launched game or launcher.
- Only Controller mode may reset the cursor to an arrow. Worker/private modes must not change the user's current cursor; their parent launcher suppresses feedback instead.
- Internal controller/child binaries are shipped atomically; protocol compatibility with independently upgraded binaries is not supported.

## Tests And Diagnostics

`tests/test_process_ipc.cpp` covers exact message validation, valid commands/responses, bad kind/opcode/mode/PID/nonce/sequence/size/payload, strict startup arguments, private-pipe source invariants, restricted handle-list spawning, accept-before-finalize media stop ordering and fallback placement, delivery of the launch-feedback opt-out to a real child probe, and earliest Controller-only cursor/tray ordering. `tests/test_crash_handler.cpp` locks the hidden dump-helper feedback opt-out. Shared-memory tests cover explicit ABI publication/rejection, version-isolated names, wrap-safe frame windows, session log directory selection, and filename containment. Runtime diagnostics identify rejected headers/messages, channel disconnects, corrupt frame indices, early/periodic Vulkan frame publication, and rate-limited ring-full events without logging secrets.

The focused `ProcessIPCTest.*:AudioCaptureSourceTest.*:ConfigTest.*` gate passed 89 tests. Incremental product build `0.1.5085` passed both hook architectures, CaptureEngine/MediaEngine, packaging, both Vulkan layers, import closure, PE mitigations, and PDB checks. The exact-build no-build gate passed all 1,728 native tests in 132 suites and all five Python tool self-tests.

## Open Questions / Stale-risk

- Logger and sensor process behavior remains outside the private pipe-command set, but both are strict consumers of the shared-memory ABI.
- Real anti-malware/injection-heavy runtime testing remains useful because inherited-handle launch behavior can be affected by third-party process instrumentation.
- CaptureEngine cannot prevent feedback that an external shell applies before `WinMain` begins; it minimizes that interval with the earliest Controller-only cursor reset and hidden UI owner. Fresh ordinary startup/application-audio churn observation remains the runtime confirmation boundary.
- ABI 36 intentionally requires a fresh target process after installation; an already injected older DLL cannot hot-upgrade its compiled mapping name/layout/build identity. Fresh ordinary-account Vulkan session `20260717_152124` confirmed exact-build discovery, session logging, 16-texture publication, overlay rendering, and 534 encoded output frames after the 2026-07-17 fix.
