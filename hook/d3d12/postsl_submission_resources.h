#pragma once

#include "postsl_lifecycle.h"

namespace ce::dx12 {
// Private statically-bound adapter: production Queue is ID3D12CommandQueue.
// Each reference is acquired under the existing command-queue mutex, then
// lives through synchronization, recording and submit, including early exits.
template<class Queue>
class PostSLSubmissionResources {
public:
    ~PostSLSubmissionResources() { Release(); }
    PostSLSubmissionResources() = default;
    PostSLSubmissionResources(const PostSLSubmissionResources&) = delete;
    PostSLSubmissionResources& operator=(const PostSLSubmissionResources&) = delete;

    void RetainSelection(Queue* selected) { Replace(selected_, selected); }
    void RetainWrapper(Queue* wrapper) { Replace(wrapper_, wrapper); }
    void Release() {
        Replace(selected_, nullptr);
        Replace(wrapper_, nullptr);
    }
    template<class Render>
    bool RenderTransaction(PostSLLifecycle& owner, uint32_t admissionEpoch,
                           std::recursive_mutex& resourceMutex, Render&& render) {
        // Normal rendering, resize and handoff already own this resource mutex.
        // Admission is checked after acquiring it, so cancelled waiters touch no retired state.
        std::lock_guard<std::recursive_mutex> resources(resourceMutex);
        return RenderTransaction(owner, admissionEpoch, std::forward<Render>(render));
    }
    template<class Render>
    bool RenderTransaction(PostSLLifecycle& owner, uint32_t admissionEpoch, Render&& render) {
        return owner.RenderTransaction(admissionEpoch, [&](uint32_t epoch) {
            ReleaseScope release{*this};
            std::forward<Render>(render)(epoch);
        });
    }
private:
    struct ReleaseScope {
        PostSLSubmissionResources& resources;
        ~ReleaseScope() { resources.Release(); }
    };
    static void Replace(Queue*& retained, Queue* next) {
        if (retained == next) return;
        if (next) next->AddRef();
        Queue* previous = retained;
        retained = next;
        if (previous) previous->Release();
    }
    Queue* selected_ = nullptr;
    Queue* wrapper_ = nullptr;
};
}  // namespace ce::dx12
