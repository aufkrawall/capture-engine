"""Run the real, UAC-approved broker test in a removable portable fixture."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    default_installed = Path(__file__).resolve().parents[1] / "installed" / "captureengine"
    parser.add_argument("--installed", type=Path, default=default_installed)
    parser.add_argument("--diagnostics", action="store_true", help="Retain private fixture logs on failure")
    args = parser.parse_args()
    source = args.installed.resolve()
    if not (source / "captureengine_elevation_service.exe").is_file():
        parser.error("Build the elevation service first")
    # Do not copy existing recordings, logs, user configuration, or dump files.
    root = source.parents[1] / "build" / "elevation-integration"
    root.mkdir(parents=True, exist_ok=True)
    fixture = Path(tempfile.mkdtemp(prefix="runtime-", dir=root))
    for name in ("ffmpeg", "plugins", "licenses"):
        shutil.copytree(source / name, fixture / name)
    for pattern in ("*.exe", "*.dll", "*.pdb"):
        for binary in source.glob(pattern):
            shutil.copy2(binary, fixture / binary.name)
    config = (source.parents[1] / "captureengine" / "config.ini.template").read_text(encoding="utf-8")
    config = config.replace("log_level=trace", "log_level=debug" if args.diagnostics else "log_level=none")
    config = config.replace("poll_interval_ms=1000", "poll_interval_ms=250")
    (fixture / "config.ini").write_text(config, encoding="utf-8")
    print("Windows will request UAC for temporary service setup and removal.", flush=True)
    # Native test refuses an existing service, exercises ordinary tokens and the
    # sensor shared-memory path, waits for child/service exits, then removes it.
    completed = subprocess.run([str(fixture / "captureengine.exe"), "--ce-service-integration"], check=False)
    if completed.returncode:
        print(f"Integration failed: Windows error {completed.returncode}. Fixture retained at {fixture}")
        return completed.returncode
    renamed = fixture.with_name(fixture.name + "-released")
    fixture.rename(renamed)
    shutil.rmtree(renamed)
    print("PASS: sensor/ETW publication, controller loss, service teardown, folder rename/delete")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
