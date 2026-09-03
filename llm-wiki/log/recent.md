# llm-wiki Log

### 2026-09-03 - CaptureEngine now installs the sensor library, and offers the PawnIO driver

Two follow-ups to the native bridge. Neither adds a shipped script.

**The four LibreHardwareMonitor files are no longer user-supplied.** `tools/build/build_lhm_plugin.py` fetches the
official v0.9.6 `LibreHardwareMonitor.zip` at build time and installs the CPU/GPU closure. The verification is the
point, not the download: pinned HTTPS URL, pinned SHA-256 (the digest GitHub publishes for that exact asset) and
pinned byte size, both checked while streaming; extraction limited to four hard-coded base names with destinations
built from that constant list rather than from archive member paths, so a `../` entry cannot escape; duplicate
candidates, oversized members and non-PE payloads refused; and the three Microsoft dependencies re-checked for an
Authenticode certificate after extraction. `LibreHardwareMonitorLib.dll` is unsigned upstream, which is exactly why
the archive digest is not optional. A verification failure is fatal; an unreachable network is not, because the
feature is optional and degrades to native usage telemetry. `installed-files.json` records per-file digests so a
tampered or stale install is replaced on the next build. `tools/tests/test_lhm_plugin.py` covers all of it, including
the traversal case.

Licensing checked before shipping: MPL-2.0 permits binary redistribution provided the Source Code Form stays
available under the MPL and recipients are told how to get it (section 3.2), and it is file-level copyleft, so
CaptureEngine's own MIT sources are unaffected. The notice file now carries that statement plus the tagged source
URL, and the packaging allowlist grew to the four files. Stale-risk: the upstream MPL-2.0 and MIT license *texts* are
referenced by URL rather than included under `licenses/`, which should be closed before a public release.

**PawnIO turned out to be a second requirement nobody had documented.** LibreHardwareMonitor 0.9.6 has dropped
WinRing0 - the assembly embeds PawnIO bytecode modules only, and `PawnIO.sys` is a separate WHQL-signed install. So
shipping the library ships no kernel driver at all, which is what made bundling defensible in the first place. But it
also means the README's "run as administrator" was only half the story: this machine *has* PawnIO installed and still
read every CPU rail as exactly zero without elevation, so both are required.

`captureengine/pawnio_setup.cpp` detects the driver by its service key and, when CPU metrics are requested and it is
missing, offers installation once. CaptureEngine neither bundles nor downloads the driver - it delegates to the
Windows Package Manager (`namazso.PawnIO`) under a single UAC prompt, falling back to opening the project page. Three
deliberate choices:
- The prompt runs on its own thread. The controller loop dispatches WM_HOTKEY itself, so a modal dialog on that
  thread would swallow hotkeys for as long as it stayed open.
- `--install-pawnio` / `--uninstall-pawnio` are elevated roles of the product executable, not shipped scripts. A
  script beside the executable that installs a kernel driver under elevation is a local privilege-escalation vector
  for anyone who can write that file - strictly worse than the PowerShell bridge just removed.
- Uninstall is command-line only. PawnIO is a shared system component LibreHardwareMonitor and other tools may also
  be using, so CaptureEngine never removes it on its own initiative.

Three answers, only the last persisted (per user, `HKCU\Software\CaptureEngine`). The TaskDialog needs the v6 common
controls, so the manifest gained that dependency, with a `MessageBox` fallback if it is unavailable. Stale-risk: the
prompt itself is unverified on hardware - this machine already has the driver, so the dialog and the elevated install
path have never been exercised end to end.

### 2026-09-03 - LibreHardwareMonitor bridge is native; the PowerShell script is gone

`plugins/LibreHardwareMonitor/CaptureEngine.LibreHardwareMonitor.ps1` was the only interpreted, user-editable
executable file CaptureEngine shipped. It is deleted. The bridge is now a role of `captureengine.exe`
(`--sensor-bridge`), launched by the dedicated sensor service exactly as before: suspended, in a kill-on-close job,
with an explicit inherited-handle list, NUL stdin/stderr, the same bounded tab-delimited stdout protocol, and the same
random named shutdown event. Process isolation, the parser, the freshness rules and the fuzz target are unchanged.

How the native bridge reaches a managed library with no managed code of our own:
- Host the in-box .NET Framework 4 runtime: `mscoree!CLRCreateInstance` -> `ICLRMetaHost::GetRuntime("v4.0.30319")` ->
  `ICLRRuntimeInfo::GetInterface(CLSID_CorRuntimeHost)` -> `Start` -> `GetDefaultDomain`. `metahost.h` is absent from
  the MinGW headers, so the two hosting interfaces are hand-declared with only the slots used.
- Late binding by name through `IDispatch` does **not** work here, in either direction, and this was measured rather
  than assumed: the CLR returns `E_NOTIMPL` from `GetIDsOfNames` on `_AppDomain`/`_Type`/`_Assembly` (their dispatch is
  typelib-backed and `mscorlib.tlb` is not registered by default), and LibreHardwareMonitor's concrete hardware classes
  are `internal`, so their CCWs answer `QueryInterface(IID_IDispatch)` with `E_NOINTERFACE`. `LibreHardwareMonitorLib`
  itself has no `[assembly: ComVisible(false)]`, so `Computer` *does* have a working AutoDispatch `IDispatch` - relying
  on that would have worked for the root object only and broken on every hardware node.
- The mechanism that does work universally is `_Object::GetType` (slot 10) followed by `_Type::InvokeMember_3`
  (slot 57), which is plain `System.Type.InvokeMember` reflection and ignores COM visibility entirely. Four frozen
  mscorlib vtables carry everything: `_AppDomain` 37/38 (`CreateInstance`/`CreateInstanceFrom`), `IObjectHandle` 3
  (`Unwrap`), `_Object` 7/10, `_Type` 57. Slot numbers were read out of `mscorlib.tlb` with a `LoadTypeLibEx` dumper,
  not guessed.
- `IComputer.Hardware` is `IList<IHardware>`; the runtime refuses to marshal a constructed generic type to a COM
  interface pointer ("Generic types cannot be marshaled to COM interface pointers", HRESULT 0x80131509), so the root
  list can never cross the boundary. The bridge binds the public `HardwareAdded`/`HardwareRemoved` events to two
  `System.Collections.Queue` objects with `Delegate.CreateDelegate` (relaxed binding lets an `IHardware`-taking
  delegate bind to `Queue.Enqueue(object)`; the `CreateDelegate(Type, object, string)` overload rejects it, the
  `MethodInfo` overload accepts it) and drains them each poll. This is also strictly better than re-reading a list,
  because hot-plugged hardware arrives and leaves through the same path. Everything below the roots -
  `IHardware.SubHardware`, `IHardware.Sensors` - is a plain array and marshals as a SAFEARRAY.
- `SensorType`/`HardwareType` ordinals come from the loaded assembly via `Enum.GetNames`/`Enum.Parse`; nothing is
  hardcoded, and any `HardwareType` whose name starts with `Gpu` counts as a GPU.

Side benefits taken while porting:
- Sensor selection moved into `captureengine/sensor_selection_policy.h` as dependency-free logic and finally has
  direct unit coverage (`tests/test_sensor_selection_policy.cpp`, 12 cases): preferred-name ranking, lowest-numbered
  instance, sticky previous identifier, highest-value last resort, zero-is-unreadable except the fan, active-GPU tie
  handling, and the identifier grammar. As a PowerShell script this logic had none.
- The nine-metric wire order, maxima and zero-rejection rule are declared once in that header and read by both the
  emitter and `sensor_plugin.cpp`'s parser, so they cannot drift; a `static_assert` pins the snapshot member order.
- Dropping PowerShell also drops ExecutionPolicy, AMSI script scanning, and a plaintext script next to the executable
  that anyone could edit.

Verified non-elevated on the Ryzen 5700X / NVIDIA machine: `CE_LHM_READY 0.9.6.0`, three samples with real GPU
temperature, package power, fan RPM, core clock, memory clock and voltage, `gpu_fan` pinned to `/gpu-nvidia/0/fan/1`,
every CPU rail reported unavailable rather than zero (cross-checked against a direct LibreHardwareMonitor read: all
CPU rails really are 0/null without the kernel driver), and exit code 0 through the shutdown event. Stale-risk: the
overlay-side rendering of these values has not been re-checked in a game since the port.
