#include "tests/flow/flow_test_support.h"
#include "tests/flow/translation_frontend.h"

// The translated-output owner caches its decision only while it covers the frontend's
// Release. Another CE hook in that slot must neither block it for good (DXUP session
// 20261016_224237: CE's diagnostic swapchain attribution took DXUP's Release, and every
// Present re-ran the ownership check plus a refused vtable patch) nor cost a patch
// attempt per Present.
namespace {
using namespace ce::flow;
using namespace ce::flow::translation;

struct TranslatedFrontend {
    ComPtr<IDXGISwapChain4> native;
    ComPtr<ID3D11Device> device;
    ComPtr<ApiFrontend> frontend;
};

bool SetUpFrontend(FlowGame& game, TranslatedFrontend& out) {
    if (!game.CreateDeviceAndSwapchain() || !game.RenderFrame()) return false;
    if (FAILED(game.RetainUnderlyingGameSwapchain().As(&out.native))) return false;
    out.device = CreateFrontendDevice();
    if (!out.device) return false;
    out.frontend.Attach(new ApiFrontend(out.native.Get(), out.device.Get()));
    game.ReleasePresentVTableHooks();
    return game.InstallPresentVTableHooks(out.frontend.Get());
}

template <typename Fn>
Fn Export(const char* name) {
    return reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"capture_hook_x64.dll"), name));
}

TEST(FlowTranslationLifetime, DiagnosticAttributionLeavesATranslatorFrontendToTheOwner) {
    FlowGame game(CurrentTestName());
    TranslatedFrontend chain;
    ASSERT_TRUE(SetUpFrontend(game, chain)) << game.Error();
    const auto attribute = Export<void (*)(IDXGISwapChain*)>("CEFlow_TryInstallSwapchainAttribution");
    const auto owns = Export<bool (*)(IDXGISwapChain*)>("CEFlow_HasTranslationOwner");
    const auto advance = Export<CEFlow_AdvanceClock_t>("CEFlow_AdvanceClock");
    ASSERT_NE(attribute, nullptr);
    ASSERT_NE(owns, nullptr);
    ASSERT_NE(advance, nullptr);

    // A wrapped chain from a translator's D3D11CreateDeviceAndSwapChain is the translator's
    // class; its AddRef/Release slots must stay the translator's own.
    void** vtable = *reinterpret_cast<void***>(chain.frontend.Get());
    void* const addRef = vtable[1];
    void* const release = vtable[2];
    attribute(chain.frontend.Get());
    EXPECT_EQ(vtable[1], addRef);
    EXPECT_EQ(vtable[2], release);

    advance(FlowGame::kFrameIntervalMicroseconds);
    ASSERT_TRUE(SUCCEEDED(PresentFrontend(chain.frontend.Get(), false)));
    const unsigned queries = chain.frontend->deviceQueries;
    EXPECT_EQ(queries, 2u);
    for (unsigned frame = 0; frame < 6; ++frame) {
        advance(FlowGame::kFrameIntervalMicroseconds);
        ASSERT_TRUE(SUCCEEDED(PresentFrontend(chain.frontend.Get(), frame % 2 != 0)));
        EXPECT_TRUE(owns(chain.frontend.Get()));
    }
    EXPECT_EQ(chain.frontend->deviceQueries, queries) << "the cached decision must serve every later Present";
    ExpectNoDebugLayerErrors();
}

TEST(FlowTranslationLifetime, RefusedReleaseSlotIsRetriedOnlyAfterItChanges) {
    FlowGame game(CurrentTestName());
    TranslatedFrontend chain;
    ASSERT_TRUE(SetUpFrontend(game, chain)) << game.Error();
    const auto occupy = Export<bool (*)(IDXGISwapChain*, bool)>("CEFlow_OccupyFrontendRelease");
    const auto attempts = Export<uint64_t (*)()>("CEFlow_TranslationReleasePatchAttempts");
    const auto owns = Export<bool (*)(IDXGISwapChain*)>("CEFlow_HasTranslationOwner");
    const auto advance = Export<CEFlow_AdvanceClock_t>("CEFlow_AdvanceClock");
    ASSERT_NE(occupy, nullptr);
    ASSERT_NE(attempts, nullptr);
    ASSERT_NE(owns, nullptr);
    ASSERT_NE(advance, nullptr);

    ASSERT_TRUE(occupy(chain.frontend.Get(), true));
    const uint64_t before = attempts();
    for (unsigned frame = 0; frame < 6; ++frame) {
        advance(FlowGame::kFrameIntervalMicroseconds);
        ASSERT_TRUE(SUCCEEDED(PresentFrontend(chain.frontend.Get(), false)));
        EXPECT_TRUE(owns(chain.frontend.Get())) << "an uncovered lifetime still delegates, uncached";
    }
    EXPECT_EQ(attempts() - before, 1u) << "a refused slot must not be patched again on every Present";

    // Once the slot changes, the owner covers it and caches its decision again.
    ASSERT_TRUE(occupy(chain.frontend.Get(), false));
    advance(FlowGame::kFrameIntervalMicroseconds);
    ASSERT_TRUE(SUCCEEDED(PresentFrontend(chain.frontend.Get(), false)));
    EXPECT_EQ(attempts() - before, 2u);
    const unsigned queries = chain.frontend->deviceQueries;
    for (unsigned frame = 0; frame < 4; ++frame) {
        advance(FlowGame::kFrameIntervalMicroseconds);
        ASSERT_TRUE(SUCCEEDED(PresentFrontend(chain.frontend.Get(), true)));
        EXPECT_TRUE(owns(chain.frontend.Get()));
    }
    EXPECT_EQ(chain.frontend->deviceQueries, queries);
    ExpectNoDebugLayerErrors();
}

}  // namespace
