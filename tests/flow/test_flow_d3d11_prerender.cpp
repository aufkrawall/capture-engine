#include "tests/flow/d3d11_af_fixture.h"

#include <cstdint>

// CE's D3D11 cpu_prerender_limit (hook/d3d11/dx11_hook_prerender.cpp) on a real WARP device: a fence
// marker after each present, a kernel wait for the marker `limit` presents back. The test makes the
// presents itself - a Flush, as the DXGI Present would - around the detour's two halves. Until
// 2026-10-18 every limit ran an event-query ring the present thread spun on.
namespace {
using ce::flow::af::AFFixture;
using ce::flow::af::FlowHook;
using ce::flow::af::Path;
using Microsoft::WRL::ComPtr;

template <class Fn>
Fn Export(const char* name) {
    return reinterpret_cast<Fn>(GetProcAddress(FlowHook(), name));
}

struct Stats {
    uint64_t path = 0;  // 1 fence, 2 query ring
    uint64_t signaled = 0;
    uint64_t completed = 0;
    uint64_t presents = 0;
    uint64_t blocked = 0;
    uint64_t armed = 0;
};

struct Prerender {
    Prerender()
        : before(Export<void (*)(ID3D11Device*, int)>("CEFlow_D3D11PrerenderBefore")),
          after(Export<void (*)()>("CEFlow_D3D11PrerenderAfter")),
          release(Export<void (*)()>("CEFlow_D3D11PrerenderRelease")),
          forceQueryRing(Export<void (*)(bool)>("CEFlow_D3D11PrerenderForceQueryRing")),
          stats(Export<uint32_t (*)(uint64_t*, uint32_t)>("CEFlow_GetD3D11PrerenderStats")) {}
    bool Ready() const {
        return before && after && release && forceQueryRing && stats;
    }
    Stats Read() const {
        uint64_t values[6] = {};
        Stats out;
        if (stats(values, 6) == 6u) {
            out = {values[0], values[1], values[2], values[3], values[4], values[5]};
        }
        return out;
    }
    // One frame as the DXGI detours run it: the limit before the present, the marker after it.
    void Frame(AFFixture& af, int limit) const {
        af.DrawWith(af.mipmapped.Get());
        before(af.device.Get(), limit);
        af.context->Flush();
        after();
    }

    void (*before)(ID3D11Device*, int);
    void (*after)();
    void (*release)();
    void (*forceQueryRing)(bool);
    uint32_t (*stats)(uint64_t*, uint32_t);
};

TEST(FlowD3D11Prerender, MarkersAfterEachPresentBoundTheQueue) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, Path::kRaw);
    ASSERT_TRUE(af.Ready());
    const Prerender prerender;
    ASSERT_TRUE(prerender.Ready());
    prerender.release();
    af.Bind(af.linear.Get());

    // The first present has no earlier marker to wait for and owes its own.
    af.DrawWith(af.mipmapped.Get());
    prerender.before(af.device.Get(), 1);
    Stats stats = prerender.Read();
    EXPECT_EQ(stats.path, 1u) << "WARP has ID3D11Fence: the fence path, not the polled query ring";
    EXPECT_EQ(stats.signaled, 0u);
    EXPECT_EQ(stats.armed, 1u);
    af.context->Flush();
    prerender.after();
    stats = prerender.Read();
    EXPECT_EQ(stats.signaled, 1u);
    EXPECT_EQ(stats.armed, 0u);

    // Every return from the limit leaves at most `limit` presents unfinished on the GPU.
    for (int limit : {1, 2, 3}) {
        for (int frame = 0; frame < 6; ++frame) {
            af.DrawWith(af.mipmapped.Get());
            prerender.before(af.device.Get(), limit);
            stats = prerender.Read();
            EXPECT_GE(stats.completed + static_cast<uint64_t>(limit), stats.signaled + 1)
                << "limit " << limit << " frame " << frame;
            af.context->Flush();
            prerender.after();
        }
    }
    stats = prerender.Read();
    EXPECT_EQ(stats.signaled, 19u) << "one marker per present";
    EXPECT_EQ(stats.presents, 19u);
    EXPECT_EQ(stats.armed, 0u);
    prerender.release();
}

TEST(FlowD3D11Prerender, LimitZeroSerializesBeforeThePresent) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, Path::kRaw);
    ASSERT_TRUE(af.Ready());
    const Prerender prerender;
    ASSERT_TRUE(prerender.Ready());
    prerender.release();
    af.Bind(af.linear.Get());

    for (int frame = 0; frame < 4; ++frame) {
        af.DrawWith(af.mipmapped.Get());
        prerender.before(af.device.Get(), 0);
        const Stats stats = prerender.Read();
        EXPECT_EQ(stats.signaled, static_cast<uint64_t>(frame + 1));
        EXPECT_GE(stats.completed, stats.signaled) << "the frame's own work finished before its present";
        EXPECT_EQ(stats.armed, 0u) << "no marker owed after the present";
        prerender.after();
    }
    EXPECT_EQ(prerender.Read().signaled, 4u);
    prerender.release();
}

TEST(FlowD3D11Prerender, NewDeviceStartsItsOwnStreamAndFallbackStillLimits) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, Path::kRaw);
    ASSERT_TRUE(af.Ready());
    const Prerender prerender;
    ASSERT_TRUE(prerender.Ready());
    prerender.release();
    af.Bind(af.linear.Get());
    for (int frame = 0; frame < 3; ++frame) {
        prerender.Frame(af, 1);
    }
    EXPECT_EQ(prerender.Read().signaled, 3u);

    // Another device: a marker owed to the old stream is dropped, the new one counts from zero.
    const HMODULE runtime = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create =
        runtime ? reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(runtime, "D3D11CreateDevice")) : nullptr;
    ASSERT_NE(create, nullptr);
    ComPtr<ID3D11Device> other;
    ComPtr<ID3D11DeviceContext> otherContext;
    ASSERT_TRUE(SUCCEEDED(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &other,
                                 nullptr, &otherContext)));
    prerender.before(af.device.Get(), 1);
    prerender.before(other.Get(), 1);
    Stats stats = prerender.Read();
    EXPECT_EQ(stats.path, 1u);
    EXPECT_EQ(stats.signaled, 0u);
    otherContext->Flush();
    prerender.after();
    EXPECT_EQ(prerender.Read().signaled, 1u);

    // Without fences the query ring still holds the limit (and is the only path that polls).
    prerender.forceQueryRing(true);
    prerender.release();
    for (int frame = 0; frame < 3; ++frame) {
        prerender.Frame(af, 1);
    }
    stats = prerender.Read();
    EXPECT_EQ(stats.path, 2u);
    EXPECT_EQ(stats.signaled, 0u) << "the ring signals no fence";
    EXPECT_EQ(stats.armed, 0u);
    EXPECT_EQ(stats.presents, 3u + 1u + 1u + 3u);
    prerender.forceQueryRing(false);
    prerender.release();
}

}  // namespace
