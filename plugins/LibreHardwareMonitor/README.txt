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
   CaptureEngine. CPU/GPU temperatures appear beside the existing usage values.
   Package power and GPU fan RPM are opt-in settings in that section.

The bridge enables only LibreHardwareMonitor's CPU and GPU visitors. It runs as
a contained child owned by CaptureEngine's dedicated sensor-service process,
never in a game or hook DLL. With several GPUs, an automatic selector follows
the GPU reporting the highest GPU Core load and retains it across equal-load
samples rather than switching arbitrarily. Advanced users can replace auto
with an exact sensor identifier. The selected identifiers and periodic values
are recorded in CaptureEngine's debug log without recording the plugin's
filesystem path.

Some low-level sensors require CaptureEngine itself to be started as
administrator. CaptureEngine never elevates automatically; an unavailable or
zero temperature is omitted from the overlay instead of being guessed.

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
