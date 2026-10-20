#pragma once

#include <string>

struct AppConfig;
struct SharedMemoryLayout;

// Publishes `config` into shared memory. `packageDir` (active code page, no trailing separator) is where the
// default and relative benchmark folders resolve (DR-38).
void UpdateSharedMemoryFromConfig(SharedMemoryLayout* sharedMemory, const AppConfig& config,
                                  const std::string& packageDir);
// The executable's own package directory: for a package host and the flow harness only.
void UpdateSharedMemoryFromConfig(SharedMemoryLayout* sharedMemory, const AppConfig& config);
