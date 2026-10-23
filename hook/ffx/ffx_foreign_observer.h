#pragma once

#include <windows.h>

#include <cstddef>

// Observe the native side of an already detoured FFX export, including the owner's saved original calls.
// The foreign entry and its function pointers stay owned by that component. Installation runs under InitMutex.
bool ffx_hook_ObserveForeignExports(HMODULE module, const char* moduleName);
void ffx_hook_ObserveLoadedForeignRuntimes();
bool ffx_hook_DelegatesToForeignObserver(const void* original);
void* ffx_hook_NativeExportAnchor(const void* original);
void ffx_hook_InvalidateForeignObservers(const void* image, size_t size);
void ffx_hook_ShutdownForeignObservers();

// Uncover only a CE-owned breakpoint whose saved byte is an entry jump, before disk-verified inspection.
void ffx_hook_UncoverForeignCreateEntry(void* target);
void ffx_hook_UncoverForeignConfigureEntry(void* target);
