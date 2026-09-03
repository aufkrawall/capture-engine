#pragma once

#include <string>
#include "benchmark_manager.h"

// Generates an interactive, standalone HTML benchmark report.
// Returns the absolute file path where the report was written, or empty string on failure.
std::string SaveBenchmarkHtmlReport(const BenchmarkResults& results, const std::string& configuredOutputDir);
