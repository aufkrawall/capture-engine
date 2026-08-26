#pragma once

#include <windows.h>

#include <string>

struct AppConfig;
struct DiscoveryInfo;
struct SharedMemoryLayout;

std::string GetProcessNameFromPID(DWORD pid);
void ClearPublicationTarget();
void SetPublicationBaseConfig(const std::string& configPath, const AppConfig& baseConfig);
void PublishResolvedConfig(SharedMemoryLayout* sharedMemory, const char* reason);
void PublishResolvedConfigForTarget(SharedMemoryLayout* sharedMemory, const std::string& targetProcessName,
                                    const char* reason);
bool TogglePublishedOverlayVisibility(SharedMemoryLayout* sharedMemory);
void PopulateWhitelistCache(DiscoveryInfo* discovery, const AppConfig& config);
