#pragma once
#include <vector>
#include <mutex>
#include <d3d11.h>

namespace ce {

// Thread-safe queue for deferring COM object release to a background thread
// Prevents micro-stutters on the render thread caused by heavy resource destruction
class DeferredReleaseQueue {
    std::vector<IUnknown*> queue;
    std::mutex mutex;

public:
    // Add an object to the release queue.
    // The queue takes ownership of ONE reference count.
    // Ensure you called AddRef() before queuing if you want to keep using it elsewhere,
    // or simply pass the pointer you want to release.
    void Queue(IUnknown* p) {
        if (!p) return;
        std::lock_guard<std::mutex> lock(mutex);
        queue.push_back(p);
    }

    // Process all pending releases. Call this from a background thread (e.g. HookThread).
    void Process() {
        std::vector<IUnknown*> toRelease;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (queue.empty()) return;
            toRelease.swap(queue);
        }
        
        for (auto* p : toRelease) {
            if (p) p->Release();
        }
    }
    
    // Clear queue without releasing (e.g. on process exit if runtime is gone)
    // Usually standard Process() is safer, but this handles fast shutdown.
    void Clear() {
        std::lock_guard<std::mutex> lock(mutex);
        queue.clear(); 
        // Note: Leaks references, but avoids crashing if D3D device is already destroyed
    }
};

} // namespace ce
