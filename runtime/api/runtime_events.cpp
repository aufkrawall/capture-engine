// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// The events of the C ABI (llm-wiki/library/03 "Events"): pass-throughs to the runtime's bounded event queue. The
// queue owns every record; a client holds one only until ce_runtime_release_event.
#include "include/cengine/cengine.h"

#include "runtime/api/handle_registry.h"
#include "runtime/api/runtime_handle.h"
#include "runtime/core/event_queue.h"

extern "C" {

ce_status_t ce_runtime_next_event(ce_runtime_t* rt, uint32_t timeout_ms, const ce_event_t** out) {
    if (out)
        *out = nullptr;
    return ce::capi::RunGuarded("ce_runtime_next_event", [&]() -> ce_status_t {
        ce_runtime* runtime = ce::capi::ResolveRuntime(rt, "ce_runtime_next_event");
        if (!runtime || !out)
            return CE_E_INVALID_ARGUMENT;
        // The queue waits on its own condition variable, bounded by timeout_ms; UINT32_MAX is refused there.
        return runtime->engine->Events().Next(timeout_ms, out);
    });
}

ce_status_t ce_runtime_release_event(ce_runtime_t* rt, const ce_event_t* event) {
    return ce::capi::RunGuarded("ce_runtime_release_event", [&]() -> ce_status_t {
        ce_runtime* runtime = ce::capi::ResolveRuntime(rt, "ce_runtime_release_event");
        if (!runtime)
            return CE_E_INVALID_ARGUMENT;
        return runtime->engine->Events().Release(event);
    });
}

void* ce_runtime_event_handle(ce_runtime_t* rt) {
    ce_runtime* runtime = ce::capi::ResolveRuntime(rt, "ce_runtime_event_handle");
    return runtime ? runtime->engine->Events().Handle() : nullptr;
}

}  // extern "C"
