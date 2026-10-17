#include "tests/flow/d3d11_af_fixture.h"

#include <cstdint>

// The DX11 overlay's GPU timer (hook/d3d11/dx11_overlay_gpu_timer.h), which fills the perf CSV's
// overlay_gpu_us, run around draws on a real WARP context. It must never wait for the GPU: a
// frame whose query set is still in flight goes untimed, results are read without a flush, and
// they arrive once the GPU got the work.
namespace {
using ce::flow::af::AFFixture;
using ce::flow::af::FlowHook;
using ce::flow::af::Path;

template <class Fn>
Fn Export(const char* name) {
    return reinterpret_cast<Fn>(GetProcAddress(FlowHook(), name));
}

TEST(FlowDX11OverlayGpuTimer, TimesDrawsWithoutWaiting) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, Path::kRaw);
    ASSERT_TRUE(af.Ready());
    const auto begin = Export<void (*)(ID3D11Device*, ID3D11DeviceContext*)>("CEFlow_DX11OverlayGpuBegin");
    const auto end = Export<void (*)(ID3D11DeviceContext*)>("CEFlow_DX11OverlayGpuEnd");
    const auto poll = Export<int32_t (*)(ID3D11DeviceContext*, int*)>("CEFlow_DX11OverlayGpuPoll");
    const auto release = Export<void (*)()>("CEFlow_DX11OverlayGpuRelease");
    ASSERT_TRUE(begin && end && poll && release);

    af.Bind(af.linear.Get());
    int inFlight = 0;
    EXPECT_EQ(poll(af.context.Get(), &inFlight), 0) << "no result before the first timed draw";
    // More frames than query sets, none flushed: the timer keeps at most four in flight and never
    // blocks on them.
    for (int frame = 0; frame < 8; ++frame) {
        begin(af.device.Get(), af.context.Get());
        af.DrawWith(af.mipmapped.Get());
        end(af.context.Get());
        poll(af.context.Get(), &inFlight);
        EXPECT_LE(inFlight, 4) << frame;
    }
    // Once the GPU has finished the work, every set resolves at the next poll. The test, not the
    // timer, waits for that: an event query issued after the last timed draw.
    Microsoft::WRL::ComPtr<ID3D11Query> done;
    const D3D11_QUERY_DESC event = {D3D11_QUERY_EVENT, 0};
    ASSERT_TRUE(SUCCEEDED(af.device->CreateQuery(&event, &done)));
    af.context->End(done.Get());
    BOOL finished = FALSE;
    while (af.context->GetData(done.Get(), &finished, sizeof(finished), 0) != S_OK) {}
    EXPECT_GE(poll(af.context.Get(), &inFlight), 0);
    EXPECT_EQ(inFlight, 0) << "finished query sets must resolve";

    // A release forgets the device: the next frame creates new queries.
    release();
    EXPECT_EQ(poll(af.context.Get(), &inFlight), 0);
    EXPECT_EQ(inFlight, 0);
    begin(af.device.Get(), af.context.Get());
    af.DrawWith(af.mipmapped.Get());
    end(af.context.Get());
    poll(af.context.Get(), &inFlight);
    EXPECT_LE(inFlight, 1);
    release();
}

}  // namespace
