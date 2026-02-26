#pragma once
#include <cstdint>
#include <string>

struct SensorData {
    float cpuUsage;
    float ramUsage;  // GB
    float gpuUsage;
    float vramUsage;  // MB
    uint64_t vramTotal;
    uint32_t maxCoreLoad;
};

class ISensorPlugin {
public:
    virtual ~ISensorPlugin() = default;

    // Initialize the plugin
    virtual bool Initialize() = 0;

    // Poll for new data
    virtual bool Poll(SensorData& data) = 0;

    // Cleanup
    virtual void Shutdown() = 0;

    // Plugin Metadata
    virtual const char* GetName() const = 0;
    virtual const char* GetVersion() const = 0;
};

// Exported function signature to create the plugin instance
typedef ISensorPlugin* (*PFN_CreateSensorPlugin)();
