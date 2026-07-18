#pragma once

#include <optional>

// Dispatches the private process-loopback worker command before normal
// CaptureEngine startup. A present result is the worker process exit code.
std::optional<int> TryRunProcessLoopbackWorkerHost();
