#pragma once

#include <windows.h>

#include <string>

struct AppConfig;
struct DiscoveryInfo;
struct SharedMemoryLayout;

std::string GetProcessNameFromPID(DWORD pid);
void ClearPublicationTarget();
// `packageDir` (active code page) is the role's package directory; the benchmark folder resolves from it.
void SetPublicationBaseConfig(const std::string& configPath, const AppConfig& baseConfig,
                              const std::string& packageDir);
// Joins the background profile-prewarm worker SetPublicationBaseConfig starts.
// Safe to call without a prior SetPublicationBaseConfig, and idempotent. The next
// SetPublicationBaseConfig (a later run of the role in this process) starts a new one.
void StopPublicationWarmup();
// Whether the prewarm worker exists (tests).
bool PublicationWarmupRunningForTesting();
void PublishResolvedConfig(SharedMemoryLayout* sharedMemory, const char* reason);
void PublishResolvedConfigForTarget(SharedMemoryLayout* sharedMemory, const std::string& targetProcessName,
                                    const char* reason);
bool TogglePublishedOverlayVisibility(SharedMemoryLayout* sharedMemory);
void PopulateWhitelistCache(DiscoveryInfo* discovery, const AppConfig& config);
