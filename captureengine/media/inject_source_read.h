#pragma once

#include <d3d11_4.h>

#include <cstdint>

#include "common/capture/frame_queue.h"
#include "common/capture/inject_source_read_release.h"

namespace ce::media { struct FrameSubmissionResultV1; }

// Completion token of the encoder's GPU reads of one inject source slot: an
// owned fence plus its signaled value, or an event query with value zero
// (MediaEngine_TakeInjectSourceReadToken).
class D3D11SourceReadToken final {
public:
    D3D11SourceReadToken() = default;
    using ContextLock = void (*)();
    D3D11SourceReadToken(IUnknown* adoptedMarker, uint64_t value, ContextLock lock, ContextLock unlock) noexcept
        : value_(value), lock_(lock), unlock_(unlock) {
        if (value != 0) {
            fence_ = static_cast<ID3D11Fence*>(adoptedMarker);
        } else {
            query_ = static_cast<ID3D11Query*>(adoptedMarker);
            ID3D11Device* device = nullptr;
            query_->GetDevice(&device);
            ID3D11Device3* device3 = nullptr;
            if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device3)))) {
                device3->GetImmediateContext3(&context_);
                device3->Release();
            }
            device->Release();
        }
    }
    D3D11SourceReadToken(const D3D11SourceReadToken&) = delete;
    D3D11SourceReadToken& operator=(const D3D11SourceReadToken&) = delete;
    D3D11SourceReadToken(D3D11SourceReadToken&& other) noexcept
        : fence_(other.fence_), query_(other.query_), context_(other.context_), value_(other.value_),
          lock_(other.lock_), unlock_(other.unlock_) {
        other.fence_ = nullptr;
        other.query_ = nullptr;
        other.context_ = nullptr;
        other.value_ = 0;
    }
    D3D11SourceReadToken& operator=(D3D11SourceReadToken&& other) noexcept {
        if (this != &other) {
            if (fence_)
                fence_->Release();
            if (query_)
                query_->Release();
            if (context_)
                context_->Release();
            fence_ = other.fence_;
            query_ = other.query_;
            context_ = other.context_;
            value_ = other.value_;
            lock_ = other.lock_;
            unlock_ = other.unlock_;
            other.fence_ = nullptr;
            other.query_ = nullptr;
            other.context_ = nullptr;
            other.value_ = 0;
        }
        return *this;
    }
    ~D3D11SourceReadToken() {
        if (fence_)
            fence_->Release();
        if (query_)
            query_->Release();
        if (context_)
            context_->Release();
    }

    // A removed device reports UINT64_MAX, which also completes: no read can
    // execute on it any more.
    bool Completed() const {
        if (fence_)
            return fence_->GetCompletedValue() >= value_;
        if (!query_ || !context_)
            return false;
        lock_();
        BOOL completed = FALSE;
        const HRESULT hr = context_->GetData(query_, &completed, sizeof(completed), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        unlock_();
        if (hr == S_OK)
            return completed != FALSE;
        if (FAILED(hr)) {
            ID3D11Device* device = nullptr;
            query_->GetDevice(&device);
            const bool removed = FAILED(device->GetDeviceRemovedReason());
            device->Release();
            return removed;
        }
        return false;
    }
    bool ArmEvent(HANDLE event) const {
        if (fence_ && SUCCEEDED(fence_->SetEventOnCompletion(value_, event)))
            return true;
        // Flush1 places an event after the query (or failed fence-event arm) on
        // the same context. This preserves event-driven release even on drivers
        // without fences, with no CPU wait and no texture copy.
        ID3D11DeviceContext3* context = context_;
        if (!context && fence_) {
            ID3D11Device* device = nullptr;
            fence_->GetDevice(&device);
            ID3D11Device3* device3 = nullptr;
            if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device3)))) {
                device3->GetImmediateContext3(&context);
                device3->Release();
            }
            device->Release();
        } else if (context) {
            context->AddRef();
        }
        if (!context)
            return false;
        lock_();
        context->Flush1(D3D11_CONTEXT_TYPE_ALL, event);
        unlock_();
        context->Release();
        return true;
    }
    uint64_t Value() const { return value_; }

private:
    ID3D11Fence* fence_ = nullptr;
    ID3D11Query* query_ = nullptr;
    ID3D11DeviceContext3* context_ = nullptr;
    uint64_t value_ = 0;
    ContextLock lock_ = nullptr;
    ContextLock unlock_ = nullptr;
};

using InjectSourceReadReleaseQueue = ce::SourceReadReleaseQueue<D3D11SourceReadToken>;

extern InjectSourceReadReleaseQueue media_main_g_InjectSourceReadReleases;

// Call right after every inject submission (fresh or catch-up). When the
// encoder still has GPU reads of the frame's slot in flight, the frame's ring
// lease moves into media_main_g_InjectSourceReadReleases, leaving the frame
// without a lease, so the existing release calls on it become no-ops. A frame
// retained for retry keeps its lease.
void DeferInjectLeaseUntilSourceRead(QueuedFrame& frame, const ce::media::FrameSubmissionResultV1& submission,
                                     const char* site);

// Releases leases whose reads finished; cheap when nothing is pending.
void ReleaseCompletedInjectSourceReads();

// One summary line per window with activity, plus the recording totals.
void LogInjectSourceReadWindow(int64_t qpcFrequency);
void LogInjectSourceReadSummary(int64_t qpcFrequency);
