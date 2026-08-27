#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace RemixHook {

// Arm the public RTX Remix API lookup before a bridge/client can cache the
// interface initializer returned by GetProcAddress.
void RegisterDynamicHooks();

// Patch already-loaded static importers after the verified Remix d3d9 module
// is available. Safe to call from every periodic hook-install pass.
void Install();

void OnModuleLoaded(HMODULE module, const char* moduleNameOrPath);
void OnModuleUnloaded(const void* moduleBase, size_t moduleSizeBytes, const char* moduleBaseName);

// RTX Remix schedules generated frames before NGX evaluates them. If its
// internal option moves away from CE's configured factor, restore the upstream
// schedule rather than merely changing NGX's diagnostic parameter object.
void ReassertFrameGenerationScheduleFromNgx(const char* parameterName, uint32_t observedGeneratedFrames,
                                            uint32_t configuredGeneratedFrames);

}  // namespace RemixHook
