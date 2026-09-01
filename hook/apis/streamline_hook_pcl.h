#pragma once

#include <cstddef>

bool MaybeHookPCLSetMarker(void*& function, bool fallbackToReturnedWrapper);
bool IsPCLSetMarkerHookReady();
void LogPCLFeatureLookupOutcome(void* originalTarget, void* returnedTarget, bool hookReady);
void LogPCLProactiveFeatureHookGap(void* target);
bool InvalidatePCLFeatureHookForModule(const void* moduleBase, size_t moduleSizeBytes,
                                       const char* moduleBaseName);
void ResetPCLLatencyCapture();
bool TryResolvePCLFeatureHook(bool proactiveScan = false);
