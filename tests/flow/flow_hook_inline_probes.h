#pragma once

#include "hook/hooking/inline_hook.h"

// The inline-hook engine driven directly, for targets a flow test owns (its own functions, a fake runtime it loads
// and unloads). Exists only in the isolated flow DLL.

namespace {
void PublishFlowProbeTrampoline(void* trampoline, void* context) {
    *static_cast<void**>(context) = trampoline;
}
}  // namespace

extern "C" __declspec(dllexport) void* CEFlow_InstallDeepHook(void* target, void* wrapper,
                                                               unsigned integerArgumentCount) {
    void* trampoline = nullptr;
    InlineHook::InstallDeepHookPublished(target, wrapper, PublishFlowProbeTrampoline, &trampoline, 0,
                                         ce::hook_patch::UnstableSnapshotPolicy::kRefuse, integerArgumentCount);
    return trampoline;
}

extern "C" __declspec(dllexport) bool CEFlow_RemoveDeepHook(void* target) {
    return InlineHook::RemoveDeepHook(target);
}
