#pragma once

#include <windows.h>
#include <string>

// Installs the unhandled exception filter to generate Minidumps
void InstallCrashHandler();

// Sets the directory where crash dumps will be written
void SetCrashDumpDirectory(const std::string& dir);

// Sets the process name for crash logging
void SetCrashProcessName(const char* name);

// Trace function for debugging the crash handler itself
void TraceCrash(const char* msg);
