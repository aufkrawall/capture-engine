#include "source_read_markers.h"

#include <d3d11_4.h>

#include <mutex>
#include <vector>

namespace ce::media {

void ReleaseSourceReadMarker(SourceReadMarker& marker) {
    if (marker.query) {
        marker.query->Release();
        marker.query = nullptr;
    }
    if (marker.context) {
        marker.context->Release();
        marker.context = nullptr;
    }
}

namespace {

bool DeviceRemoved(ID3D11DeviceContext* context) {
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    if (!device)
        return true;
    const bool removed = FAILED(device->GetDeviceRemovedReason());
    device->Release();
    return removed;
}

}  // namespace

size_t PollSourceReadMarkers(std::deque<SourceReadMarker>& markers, SourceReadSequencer& sequencer) {
    size_t completed = 0;
    uint64_t through = 0;
    while (!markers.empty()) {
        SourceReadMarker& front = markers.front();
        bool done = false;
        if (front.query && front.context) {
            BOOL signaled = FALSE;
            const HRESULT hr =
                front.context->GetData(front.query, &signaled, sizeof(signaled), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            done = (hr == S_OK && signaled) || (FAILED(hr) && DeviceRemoved(front.context));
        } else {
            done = true;  // nothing was recorded for it, so nothing can still read
        }
        if (!done)
            break;
        through = front.sequence;
        ReleaseSourceReadMarker(front);
        markers.pop_front();
        ++completed;
    }
    if (through != 0)
        sequencer.CompleteThrough(through);
    return completed;
}

bool FinishSourceReadMarkers(std::deque<SourceReadMarker>& markers, SourceReadSequencer& sequencer,
                             DWORD timeoutMs, std::recursive_mutex& deviceLock) {
    std::unique_lock<std::recursive_mutex> lock(deviceLock);
    PollSourceReadMarkers(markers, sequencer);
    // Markers of one context complete in order, so one fence behind the newest
    // marker of each context covers all of them.
    std::vector<ID3D11Fence*> fences;
    std::vector<HANDLE> events;
    ID3D11DeviceContext* signaledContext = nullptr;
    for (auto it = markers.rbegin(); it != markers.rend(); ++it) {
        if (!it->context || it->context == signaledContext)
            continue;
        signaledContext = it->context;
        ID3D11Device* device = nullptr;
        it->context->GetDevice(&device);
        ID3D11Device5* device5 = nullptr;
        ID3D11DeviceContext4* context4 = nullptr;
        ID3D11Fence* fence = nullptr;
        if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5))) &&
            SUCCEEDED(it->context->QueryInterface(IID_PPV_ARGS(&context4))) &&
            SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) &&
            SUCCEEDED(context4->Signal(fence, 1))) {
            context4->Flush();
            HANDLE fenceEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (fenceEvent && SUCCEEDED(fence->SetEventOnCompletion(1, fenceEvent)))
                events.push_back(fenceEvent);
            else if (fenceEvent)
                CloseHandle(fenceEvent);
            fences.push_back(fence);
            fence = nullptr;
        }
        if (fence)
            fence->Release();
        if (context4)
            context4->Release();
        if (device5)
            device5->Release();
        if (device)
            device->Release();
    }
    lock.unlock();

    if (!events.empty())
        WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), TRUE, timeoutMs);
    for (HANDLE fenceEvent : events)
        CloseHandle(fenceEvent);

    lock.lock();
    PollSourceReadMarkers(markers, sequencer);
    const bool finished = markers.empty();
    for (ID3D11Fence* fence : fences)
        fence->Release();
    lock.unlock();
    return finished;
}

}  // namespace ce::media
