#pragma once

// Whether CE may call a queue method it resolved from one D3D12 implementation (the real
// ExecuteCommandLists or Signal, taken from a D3D12Core queue vtable) with a given queue object.
//
// A layer that wraps queues with objects of its own - the D3D12 debug layer (d3d12SDKLayers.dll),
// RenderDoc, PIX, Streamline's proxies - gives them a vtable in its own module and a layout D3D12Core's
// code does not understand: D3D12Core's ExecuteCommandLists called with the debug layer's queue reads
// the wrapper as its own object (an access violation on CE's first overlay submit, every FG flow
// scenario with the debug layer on). The queue's vtable decides: a resolved method is only called on
// objects whose vtable lives in the image the method lives in; any other queue is called through its
// own vtable. Pure: the caller resolves both addresses to the base of the image containing them
// (nullptr when an address is not image-backed, e.g. a vtable copied to the heap).
namespace ce::dx12_overlay_policy {

enum class ResolvedQueueMethodFit {
    kNoMethod,            // nothing resolved: the caller's unresolved path
    kSameImplementation,  // the queue's vtable lives in the method's image: callable
    kForeignQueueObject,  // the queue's vtable lives in another image (a wrapping layer)
    kUnprovable,          // the queue or its vtable is missing, or an address is not image-backed
};

inline ResolvedQueueMethodFit ClassifyResolvedQueueMethodFit(bool methodResolved, const void* methodImage,
                                                             const void* queueVtableImage) {
    if (!methodResolved) {
        return ResolvedQueueMethodFit::kNoMethod;
    }
    if (!methodImage || !queueVtableImage) {
        return ResolvedQueueMethodFit::kUnprovable;
    }
    return methodImage == queueVtableImage ? ResolvedQueueMethodFit::kSameImplementation
                                           : ResolvedQueueMethodFit::kForeignQueueObject;
}

inline bool MayCallResolvedQueueMethod(ResolvedQueueMethodFit fit) {
    return fit == ResolvedQueueMethodFit::kSameImplementation;
}

inline const char* ResolvedQueueMethodFitName(ResolvedQueueMethodFit fit) {
    switch (fit) {
        case ResolvedQueueMethodFit::kNoMethod:
            return "no-method";
        case ResolvedQueueMethodFit::kSameImplementation:
            return "same-implementation";
        case ResolvedQueueMethodFit::kForeignQueueObject:
            return "foreign-queue-object";
        case ResolvedQueueMethodFit::kUnprovable:
            return "unprovable";
    }
    return "unknown";
}

}  // namespace ce::dx12_overlay_policy
