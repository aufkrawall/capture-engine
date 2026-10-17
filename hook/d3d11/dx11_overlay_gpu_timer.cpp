// GPU timing of CE's DX11 overlay draw (dx11_overlay_gpu_timer.h). Present-thread only, like the
// rest of the DX11 overlay state.

#include "dx11_overlay_gpu_timer.h"

#include "hook/runtime/hook_common.h"

namespace ce::dx11_overlay_gpu {

namespace {

struct QuerySet {
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* begin = nullptr;
    ID3D11Query* end = nullptr;
    bool inFlight = false;
};

// Four frames in flight before a frame goes untimed: results normally arrive within one or two.
constexpr int kSets = 4;

struct Timer {
    ID3D11Device* device = nullptr;  // an address only: identifies the device the queries belong to
    QuerySet sets[kSets];
    int next = 0;
    int open = -1;  // the set between BeginOverlay and EndOverlay
    int32_t latestUs = 0;
    bool createFailed = false;
};

Timer g_timer;

void ReleaseSet(QuerySet& set, bool releaseObjects) {
    for (ID3D11Query** query : {&set.disjoint, &set.begin, &set.end}) {
        if (*query && releaseObjects) {
            (*query)->Release();
        }
        *query = nullptr;
    }
    set.inFlight = false;
}

bool CreateSets(ID3D11Device* device) {
    D3D11_QUERY_DESC disjoint = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC stamp = {D3D11_QUERY_TIMESTAMP, 0};
    for (QuerySet& set : g_timer.sets) {
        if (FAILED(device->CreateQuery(&disjoint, &set.disjoint)) || FAILED(device->CreateQuery(&stamp, &set.begin)) ||
            FAILED(device->CreateQuery(&stamp, &set.end))) {
            return false;
        }
    }
    return true;
}

}  // namespace

void BeginOverlay(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context) {
        return;
    }
    if (device != g_timer.device) {
        Release(true);
        g_timer.device = device;
        g_timer.createFailed = !CreateSets(device);
        if (g_timer.createFailed) {
            HookLog(
                "DX11: overlay GPU timer unavailable (timestamp queries could not be created); "
                "overlay_gpu_us stays 0");
            Release(true);
            g_timer.device = device;
            g_timer.createFailed = true;
        }
    }
    if (g_timer.createFailed) {
        return;
    }
    QuerySet& set = g_timer.sets[g_timer.next];
    if (set.inFlight) {
        return;  // never wait for the GPU: this frame goes untimed
    }
    context->Begin(set.disjoint);
    context->End(set.begin);
    g_timer.open = g_timer.next;
    g_timer.next = (g_timer.next + 1) % kSets;
}

void EndOverlay(ID3D11DeviceContext* context) {
    if (!context || g_timer.open < 0) {
        return;
    }
    QuerySet& set = g_timer.sets[g_timer.open];
    context->End(set.end);
    context->End(set.disjoint);
    set.inFlight = true;
    g_timer.open = -1;
}

int32_t PollLatestUs(ID3D11DeviceContext* context) {
    if (!context || g_timer.createFailed) {
        return g_timer.latestUs;
    }
    // Oldest first, so the newest finished result wins.
    for (int age = 0; age < kSets; ++age) {
        QuerySet& set = g_timer.sets[(g_timer.next + age) % kSets];
        if (!set.inFlight) {
            continue;
        }
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
        UINT64 begin = 0;
        UINT64 end = 0;
        constexpr UINT kNoFlush = D3D11_ASYNC_GETDATA_DONOTFLUSH;
        if (context->GetData(set.disjoint, &disjoint, sizeof(disjoint), kNoFlush) != S_OK ||
            context->GetData(set.begin, &begin, sizeof(begin), kNoFlush) != S_OK ||
            context->GetData(set.end, &end, sizeof(end), kNoFlush) != S_OK) {
            continue;
        }
        set.inFlight = false;
        if (!disjoint.Disjoint && disjoint.Frequency != 0 && end >= begin) {
            g_timer.latestUs = static_cast<int32_t>((end - begin) * 1000000ull / disjoint.Frequency);
        }
    }
    return g_timer.latestUs;
}

void Release(bool releaseObjects) {
    for (QuerySet& set : g_timer.sets) {
        ReleaseSet(set, releaseObjects);
    }
    g_timer.device = nullptr;
    g_timer.next = 0;
    g_timer.open = -1;
    g_timer.latestUs = 0;
    g_timer.createFailed = false;
}

#ifdef CE_FLOW_TEST
// The timer around the test's own draws, as DrawDX11Overlay runs it around the overlay's.
extern "C" __declspec(dllexport) void CEFlow_DX11OverlayGpuBegin(ID3D11Device* device, ID3D11DeviceContext* context) {
    BeginOverlay(device, context);
}
extern "C" __declspec(dllexport) void CEFlow_DX11OverlayGpuEnd(ID3D11DeviceContext* context) {
    EndOverlay(context);
}
// The newest result, and how many query sets are still in flight.
extern "C" __declspec(dllexport) int32_t CEFlow_DX11OverlayGpuPoll(ID3D11DeviceContext* context, int* inFlight) {
    const int32_t latest = PollLatestUs(context);
    if (inFlight) {
        *inFlight = 0;
        for (const QuerySet& set : g_timer.sets) {
            *inFlight += set.inFlight ? 1 : 0;
        }
    }
    return latest;
}
extern "C" __declspec(dllexport) void CEFlow_DX11OverlayGpuRelease() {
    Release(true);
}
#endif

}  // namespace ce::dx11_overlay_gpu
