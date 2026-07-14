# Process IPC And Restricted Children

Last cross-checked: 2026-07-15 (restricted handle-list spawning and private versioned process channels)

Primary sources:
- `common/restricted_child_process.{h,cpp}`
- `common/process_ipc.{h,cpp}`
- `captureengine/{main,inject_main,media_main,limiter_main}.cpp`
- `tests/test_process_ipc.cpp`

## Summary

Controller-to-inject/media/limiter commands use a private channel created for each spawned child. The controller creates a unique connected duplex named-pipe pair, marks only the child endpoint inheritable, and launches through `RestrictedChildProcess` with `STARTUPINFOEX` plus `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`. Production children no longer reconnect to a public fixed pipe name. A broken channel is terminal for that child instance and the controller performs a clean respawn.

The pipe security descriptor grants access only to SYSTEM and the current user. The child verifies that the inherited pipe's server PID is the controller. The controller accepts startup only when the versioned handshake contains the exact spawned child PID, expected mode, and per-channel cryptographically generated 128-bit nonce.

## Message Contract

Protocol 2 has an exact 52-byte packed header and at most 256 payload bytes. Validation covers magic, version, header size, exact read and total sizes, message kind, opcode, sender mode, sender PID, sequence, nonce, payload termination, and opcode-specific payload shape. Short, oversized, stale, unknown, mismatched, and malformed messages are rejected. Invalid-message logging is rate limited.

The private pipe name is only a transient rendezvous used while the controller already holds the connected endpoint; it is not a stable production API. The child receives the endpoint handle, controller PID, and nonce on its command line and strictly parses their complete values before using the channel.

## Child Launcher Invariants

- Only handles listed in the attribute list are inherited.
- Invalid handles, attribute-list setup failure, or process creation failure abort the spawn and close owned resources.
- The same launcher is used by normal process IPC and the process-loopback helper boundary.
- Internal controller/child binaries are shipped atomically; protocol compatibility with independently upgraded binaries is not supported.

## Tests And Diagnostics

`tests/test_process_ipc.cpp` covers exact message validation, valid commands/responses, bad kind/opcode/mode/PID/nonce/sequence/size/payload, strict startup arguments, private-pipe source invariants, and restricted handle-list spawning. Runtime diagnostics identify rejected messages and channel disconnects without logging the nonce.

## Open Questions / Stale-risk

- Logger and sensor process behavior is intentionally outside the private command-channel set unless they gain the same controller command contract.
- Real anti-malware/injection-heavy runtime testing remains useful because inherited-handle launch behavior can be affected by third-party process instrumentation.
