#pragma once

struct AppConfig;
struct SharedMemoryLayout;

void UpdateSharedMemoryFromConfig(SharedMemoryLayout* sharedMemory, const AppConfig& config);
