#pragma once

#include <cstdint>
#include <string>

struct BenchmarkConfig {
    uint32_t startDelaySeconds = 0;  // 0 = immediate start; 1-3600 seconds delay
    uint32_t durationSeconds = 0;    // 0 = manual stop; >0 stops automatically after N seconds
    std::string outputDir;           // empty = "benchmarks" subfolder
};
