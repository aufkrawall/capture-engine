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
    DisplayTimingService();
    ~DisplayTimingService();

    DisplayTimingService(const DisplayTimingService&) = delete;
    DisplayTimingService& operator=(const DisplayTimingService&) = delete;

    void Start();
    void UpdateTargets(const std::vector<DisplayTimingTarget>& targets);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
