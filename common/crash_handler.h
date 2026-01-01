#pragma once

#include <string>
#include <windows.h>


// Installs the unhandled exception filter to generate Minidumps
void InstallCrashHandler();

// Sets the directory where crash dumps will be written
void SetCrashDumpDirectory(const std::string &dir);
