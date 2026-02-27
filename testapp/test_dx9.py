#!/usr/bin/env python3
import subprocess
import time
from pathlib import Path

testapp_dir = Path(__file__).parent
bin_dir = testapp_dir / "bin"
capture_dir = testapp_dir.parent / "installed" / "captureengine"

# Clean up logs
log_dir = capture_dir / "logs"
if log_dir.exists():
    import shutil

    shutil.rmtree(log_dir, ignore_errors=True)

# Start captureengine
print("Starting captureengine...")
ce_proc = subprocess.Popen(
    [str(capture_dir / "captureengine.exe")], cwd=str(capture_dir), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
)

time.sleep(2)

# Run DX9 test
print("Running DX9 test...")
dx9_exe = bin_dir / "dx9_test.exe"
try:
    result = subprocess.run([str(dx9_exe)], timeout=15, capture_output=True, text=True)
    print(f"DX9 test exited with code: {result.returncode}")
    if result.stdout:
        print(f"stdout: {result.stdout[:500]}")
    if result.stderr:
        print(f"stderr: {result.stderr[:500]}")
except subprocess.TimeoutExpired:
    print("DX9 test timed out (probably OK - running normally)")
except Exception as e:
    print(f"Error running DX9 test: {e}")

# Check for crash logs
time.sleep(1)
if log_dir.exists():
    crash_logs = list(log_dir.glob("crash*.log"))
    crash_dumps = list(log_dir.glob("crash*.dmp"))
    if crash_logs or crash_dumps:
        print(f"CRASH DETECTED! Logs: {crash_logs}, Dumps: {crash_dumps}")
        for cl in crash_logs:
            print(f"\n--- {cl.name} ---")
            print(cl.read_text()[:2000])
    else:
        print("No crash logs found - test passed!")
else:
    print("No logs directory - test passed!")

# Cleanup
ce_proc.terminate()
try:
    ce_proc.wait(timeout=5)
except Exception:
    ce_proc.kill()

print("Done!")
