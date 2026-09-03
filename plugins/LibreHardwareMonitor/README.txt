CaptureEngine optional LibreHardwareMonitor sensors
==================================================

Nothing has to be installed here by hand. CaptureEngine's bridge is compiled
into captureengine.exe, and the four LibreHardwareMonitor runtime files beside
this README are installed by the build from the official v0.9.6 release, after
verifying the archive against a pinned SHA-256 digest:

  LibreHardwareMonitorLib.dll
  System.Memory.dll
  System.Numerics.Vectors.dll
  System.Runtime.CompilerServices.Unsafe.dll

Nothing else from that release is extracted or shipped. Their licenses and the
redistribution boundary are recorded in
licenses/LibreHardwareMonitor_NOTICE.txt.

What you get
------------

CPU/GPU temperature, package power and GPU fan RPM appear beside the existing
usage values; CPU/GPU core clock, GPU memory clock and GPU core voltage appear
on their own clock rows. Every metric defaults to auto in [HardwareSensors] and
can be set to off or to an exact sensor identifier.

The bridge runs as a contained child owned by CaptureEngine's dedicated
sensor-service process: the sensor service starts captureengine.exe again in a
dedicated bridge role, which hosts the .NET Framework runtime that ships with
Windows and drives LibreHardwareMonitorLib directly. No script, interpreter, or
extra binary is involved, and none of this ever runs in a game or hook DLL.
Only LibreHardwareMonitor's CPU and GPU visitors are enabled. With several GPUs,
an automatic selector follows the GPU reporting the highest GPU Core load and
retains it across equal-load samples rather than switching arbitrarily. The
selected identifiers and periodic values are recorded in CaptureEngine's debug
log without recording the plugin's filesystem path.

An automatic selector prefers an exactly named sensor, then the lowest-numbered
instance of that name, and only then falls back to the highest current reading.
Hardware that numbers its sensors ("GPU Fan 1", "GPU Fan 2") therefore resolves
to one fixed fan instead of alternating between physical fans as their speeds
cross.

CPU sensors need PawnIO and administrator rights
------------------------------------------------

GPU sensors work out of the box. CPU temperature, package power and core clock
do not: LibreHardwareMonitor 0.9.6 reads those through PawnIO, a separate
Microsoft-signed kernel driver, and CaptureEngine additionally has to be running
as administrator to open it. Without both, every one of those rails reads
exactly zero.

CaptureEngine neither bundles nor downloads that driver. When the driver is
missing and CPU sensors are requested, CaptureEngine asks once at startup
whether to install it and, if you agree, hands the job to the Windows Package
Manager under a single administrator prompt. The dialog's three answers are
install now, not now (ask again next start), and don't ask again. "Don't ask
again" is remembered per user under HKCU\Software\CaptureEngine.

The driver can also be installed or removed from a command line:

  captureengine.exe --install-pawnio
  captureengine.exe --uninstall-pawnio

Both refuse to run without elevation. Uninstalling is deliberately not offered
in the interface: PawnIO is a shared system component that LibreHardwareMonitor
and other tools may also be using, so removing it stays an explicit decision.

A metric that reads zero is reported as unavailable and omitted from the overlay
rather than shown as a real 0 C, 0 W, 0 MHz or 0 V; a stopped fan is the one
case where zero is kept, because 0 RPM is a genuine reading. The reason is
logged once per run.
