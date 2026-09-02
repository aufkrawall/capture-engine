CaptureEngine optional LibreHardwareMonitor bridge
==================================================

CaptureEngine ships this MIT-licensed bridge script but does not bundle
LibreHardwareMonitor or its dependency DLLs.

Setup (tested with LibreHardwareMonitor 0.9.6):

1. Download LibreHardwareMonitor.zip from the official v0.9.6 release:
   https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases/tag/v0.9.6
2. From that one release, copy these four matching files into this folder:

   LibreHardwareMonitorLib.dll
   System.Memory.dll
   System.Numerics.Vectors.dll
   System.Runtime.CompilerServices.Unsafe.dll

   No other release files are used by this CPU/GPU-only bridge; the
   LibreHardwareMonitor GUI executable is not required.
3. Leave [HardwareSensors] enabled=auto in config.ini and restart
   CaptureEngine. CPU/GPU temperature, package power and GPU fan RPM appear
   beside the existing usage values; CPU/GPU core clock, GPU memory clock and
   GPU core voltage appear on their own clock rows. Every metric defaults to
   auto and can be set to off or to an exact sensor identifier.

The bridge enables only LibreHardwareMonitor's CPU and GPU visitors. It runs as
a contained child owned by CaptureEngine's dedicated sensor-service process,
never in a game or hook DLL. With several GPUs, an automatic selector follows
the GPU reporting the highest GPU Core load and retains it across equal-load
samples rather than switching arbitrarily. Advanced users can replace auto
with an exact sensor identifier. The selected identifiers and periodic values
are recorded in CaptureEngine's debug log without recording the plugin's
filesystem path.

An automatic selector prefers an exactly named sensor, then the lowest-numbered
instance of that name, and only then falls back to the highest current reading.
Hardware that numbers its sensors ("GPU Fan 1", "GPU Fan 2") therefore resolves
to one fixed fan instead of alternating between physical fans as their speeds
cross.

Some low-level sensors require CaptureEngine itself to be started as
administrator: without elevation LibreHardwareMonitor cannot open its kernel
driver, and the CPU temperature, package power and core clock rails then read
exactly zero. CaptureEngine never elevates automatically. A metric that reads
zero is reported as unavailable and omitted from the overlay rather than shown
as a real 0 C, 0 W, 0 MHz or 0 V; a stopped fan is the one case where zero is
kept, because 0 RPM is a genuine reading. The reason is logged once per run.

Licensing
---------

LibreHardwareMonitor is licensed under MPL-2.0. Its upstream source, license,
and third-party notices are here:

https://github.com/LibreHardwareMonitor/LibreHardwareMonitor
https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/blob/v0.9.6/LICENSE
https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/blob/v0.9.6/THIRD-PARTY-NOTICES.txt

The three System.* dependency DLLs retain their own licenses. Keep the files and
the versioned upstream license and notices together if you redistribute a
combined installation. These optional DLLs execute with the sensor service's
privileges, so obtain them only from the official project release. CaptureEngine
release packaging deliberately includes only this README and the first-party
bridge script from the plugins folder; it excludes every locally added DLL and
notice file.

See licenses/LibreHardwareMonitor_NOTICE.txt for CaptureEngine's complete
redistribution boundary and upstream references.
