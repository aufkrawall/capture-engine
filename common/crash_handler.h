#pragma once

#include <windows.h>
#include <string>

// Installs the unhandled exception filter to generate Minidumps
void InstallCrashHandler();

// Sets the directory where crash dumps will be written
void SetCrashDumpDirectory(const std::string& dir);
