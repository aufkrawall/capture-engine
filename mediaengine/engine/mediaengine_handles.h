#pragma once

// The handle table of mediaengine.dll (DR-42): maps each MediaEngineHandle to the pipeline it names, and gives every
// thread the log route (DR-40) of the pipeline it is working for.

#include <memory>
#include <utility>

#include "mediaengine/engine/mediaengine_abi.h"

class MediaEngineInstance;
struct PipelineSlot;

// Where the DLL's log lines of one pipeline go. A value rather than the instance, so a thread that outlives its
// pipeline's teardown never dereferences freed memory.
struct MediaEngineLogRoute {
    MediaEngineLogFn callback = nullptr;
    void* context = nullptr;
};

// The route the calling thread logs through; empty when it works for no pipeline (process sink).
MediaEngineLogRoute CurrentMediaEngineLogRoute();

// Installs `route` on the calling thread until destroyed; nested scopes restore the outer route.
class MediaEngineLogRouteScope {
public:
    explicit MediaEngineLogRouteScope(MediaEngineLogRoute route);
    ~MediaEngineLogRouteScope();
    MediaEngineLogRouteScope(const MediaEngineLogRouteScope&) = delete;
    MediaEngineLogRouteScope& operator=(const MediaEngineLogRouteScope&) = delete;

private:
    MediaEngineLogRoute previous_;
};

// Pins the pipeline a handle names for the length of one call (RAII) and routes the calling thread's log lines to it.
// Empty when the handle is zero, unknown or destroyed: that is logged once per process and the call takes its
// no-pipeline path. Pinning is lock-free, and MediaEngineRetire waits for every pin to go before it releases the pipeline.
class MediaEngineLease {
public:
    explicit MediaEngineLease(MediaEngineHandle handle);
    ~MediaEngineLease();
    MediaEngineLease(const MediaEngineLease&) = delete;
    MediaEngineLease& operator=(const MediaEngineLease&) = delete;

    explicit operator bool() const { return instance_ != nullptr; }
    MediaEngineInstance* get() const { return instance_; }
    MediaEngineInstance* operator->() const { return instance_; }
    MediaEngineInstance& operator*() const { return *instance_; }

private:
    MediaEngineInstance* instance_ = nullptr;
    PipelineSlot* slot_ = nullptr;
    MediaEngineLogRoute previousRoute_;
};

// Holds the pipeline on the calling thread across calls, for the device lock that MediaEngine_LockD3D11 takes and
// MediaEngine_UnlockD3D11 releases. Retire waits for a hold like it waits for a lease. Null when the handle names no
// live pipeline (logged once); the hold ends with ReleasePipelineHold.
MediaEngineInstance* HoldPipeline(MediaEngineHandle handle);
// The pipeline the calling thread holds under `handle` (its most recent hold), null when it holds none.
MediaEngineInstance* HeldPipeline(MediaEngineHandle handle);
void ReleasePipelineHold(MediaEngineHandle handle);

// Publishes `instance` under a new handle; zero when all handle slots are in use (logged).
MediaEngineHandle RegisterMediaEngineInstance(std::unique_ptr<MediaEngineInstance> instance);

// Ends the handle: later calls on it are refused, and the calls already pinning the pipeline are waited for. Returns the
// pipeline for the caller to tear down, or null when the handle is zero, unknown or already retired. The calling thread
// must not hold a lease or a hold on the handle itself.
std::unique_ptr<MediaEngineInstance> RetireMediaEngineInstance(MediaEngineHandle handle);

// Wraps a callable that a pipeline runs on another thread so that thread logs through the route the calling thread has
// now. Used for every thread a pipeline starts (DR-40).
template <typename Body>
auto WithInheritedLogRoute(Body&& body) {
    return [route = CurrentMediaEngineLogRoute(), body = std::forward<Body>(body)]() mutable -> decltype(auto) {
        MediaEngineLogRouteScope scope(route);
        return body();
    };
}
