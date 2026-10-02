#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

// Diagnostics for a failing IDXGISwapChain::ResizeBuffers. DXGI rejects a
// resize with DXGI_ERROR_INVALID_CALL while anyone still references a back
// buffer, and the rejection names no holder. Talos Reawakened died of exactly
// that while CE was recording (logs/20260926_083506) and nothing on record
// said who held what. Probing each buffer right before and after the call
// turns the next occurrence into evidence.
//
// The probe takes a reference with GetBuffer and releases it: the count
// Release returns is what everybody else still holds, DXGI's own references
// included. That baseline is why successful resizes are logged too.

namespace ce::resize_reference_probe {

inline constexpr UINT kMaxProbedBuffers = 16;

struct BackBufferReferences {
    UINT probed = 0;
    ULONG heldByOthers[kMaxProbedBuffers] = {};
};

// `bufferIid` selects the API: a D3D12 chain refuses an ID3D11 interface and the
// other way round, so a probe for one API is empty on the other's chains.
template <typename BufferType, typename SwapChain>
BackBufferReferences Probe(SwapChain* swapChain, UINT bufferCount, REFIID bufferIid) {
    BackBufferReferences references;
    if (!swapChain) {
        return references;
    }
    const UINT count = bufferCount < kMaxProbedBuffers ? bufferCount : kMaxProbedBuffers;
    for (UINT i = 0; i < count; ++i) {
        BufferType* buffer = nullptr;
        if (FAILED(swapChain->GetBuffer(i, bufferIid, reinterpret_cast<void**>(&buffer))) || !buffer) {
            break;
        }
        references.heldByOthers[i] = buffer->Release();
        ++references.probed;
    }
    return references;
}

// References everybody else holds on a buffer the caller already owns one
// reference to (the capture's own GetBuffer), without changing the count.
template <typename BufferType>
ULONG HeldByOthersBesidesCaller(BufferType* buffer) {
    if (!buffer) {
        return 0;
    }
    buffer->AddRef();
    const ULONG total = buffer->Release();
    return total > 0 ? total - 1 : 0;
}

// Per-stage evidence from the capture copy itself (logs/20260926_090625 showed
// three extra references per back buffer only while capture ran, and no CE code
// keeps one). The first copies of a generation are logged as a baseline; later
// ones only when a stage leaves more references than the copy found, which
// names the call that takes them.
struct CaptureStageReferences {
    ULONG entry = 0;
    ULONG recorded = 0;
    ULONG executed = 0;
    ULONG signaled = 0;
};

inline constexpr UINT kBaselineCopiesPerGeneration = 3;

inline bool StagesAddedReferences(const CaptureStageReferences& stages) {
    return stages.recorded > stages.entry || stages.executed > stages.entry || stages.signaled > stages.entry;
}

inline bool ShouldLogCaptureStageReferences(UINT copiesBeforeThisOne, const CaptureStageReferences& stages,
                                            uint32_t anomalyLogIndex) {
    if (copiesBeforeThisOne < kBaselineCopiesPerGeneration) {
        return true;
    }
    if (!StagesAddedReferences(stages)) {
        return false;
    }
    return anomalyLogIndex < 16 || (anomalyLogIndex % 256) == 0;
}

// "[2,2,3]" - one entry per probed buffer; "[]" when nothing could be probed.
inline void Format(const BackBufferReferences& references, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }
    size_t used = 0;
    auto append = [&](const char* text) {
        const int written = snprintf(out + used, outSize - used, "%s", text);
        if (written > 0) {
            used += static_cast<size_t>(written) < outSize - used ? static_cast<size_t>(written) : outSize - used - 1;
        }
    };
    out[0] = '\0';
    append("[");
    for (UINT i = 0; i < references.probed && i < kMaxProbedBuffers; ++i) {
        char entry[16];
        snprintf(entry, sizeof(entry), i == 0 ? "%lu" : ",%lu", static_cast<unsigned long>(references.heldByOthers[i]));
        append(entry);
    }
    append("]");
}

}  // namespace ce::resize_reference_probe
