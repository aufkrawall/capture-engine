#pragma once

#include <cstdint>
#include <memory>
#include <vector>

struct SharedDisplayTiming;

struct DisplayTimingTarget {
    uint32_t sourcePid = 0;
    uint32_t rendererPid = 0;
    SharedDisplayTiming* output = nullptr;
};

class DisplayTimingService {
public:
    // recordDisplayStalls: [Diagnostics] display_stall_trace - with trace logging, the elevation service records the
    // display pipeline and the service seals stall episodes and swapchain switches into the log folder.
    explicit DisplayTimingService(bool recordDisplayStalls);
    ~DisplayTimingService();

    DisplayTimingService(const DisplayTimingService&) = delete;
    DisplayTimingService& operator=(const DisplayTimingService&) = delete;

    void Start();
    void UpdateTargets(const std::vector<DisplayTimingTarget>& targets);
    bool NeedsRestart() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
