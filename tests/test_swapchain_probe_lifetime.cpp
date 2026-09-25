#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../hook/common/dx12_overlay_policy.h"
#include "../hook/common/swapchain_liveness.h"
#include "source_fragment_reader.h"

// ---------------------------------------------------------------------------
// Strange Brigade DX12 closed with STATUS_HEAP_CORRUPTION (0xC0000374) while OptiScaler,
// Special K, ReShade and the Steam overlay were injected (session 20260819_000437). The stack was
// CWrapDXGISwapChain::Release -> ~CWrapDXGISwapChain -> OptiScaler -> free -> RtlFreeHeap failure:
// the destructor's "net-zero" post-destruction refcount probe AddRef'd a chain its own promoted
// releases had just destroyed, and the matching Release then ran the proxy's destructor a SECOND
// time.
//
// The VirtualQuery/AV-guard the probe relied on can never detect that: a freed heap block stays
// MEM_COMMIT, its vtable pointer still resolves to real code, and the AddRef therefore SUCCEEDS on
// a corpse. The only sound rule is ownership - never call a virtual method on a COM object without
// holding a reference to it.
// ---------------------------------------------------------------------------

namespace {

std::string ReadProjectSource(const std::string& relativePath) {
    namespace fs = std::filesystem;
    const fs::path path = fs::current_path() / fs::path(relativePath);
    EXPECT_TRUE(fs::exists(path)) << path.string();
    const std::string text = ce::test_source::ReadLogicalSource(path);
    EXPECT_FALSE(text.empty()) << path.string();
    return text;
}

}  // namespace

TEST(SwapChainProbeLifetimeTest, DestructorHoldsItsOwnReferenceWheneverCEStillOwnsOne) {
    using ce::dx12_overlay_policy::ShouldHoldRealSwapchainDiagnosticReferenceDuringWrapperDestructor;

    // Promoted IDXGISwapChain1..4 references are CE-owned, so a reference can be taken safely - and
    // must be, because those promoted releases are exactly what destroyed OptiScaler's proxy.
    EXPECT_TRUE(ShouldHoldRealSwapchainDiagnosticReferenceDuringWrapperDestructor(
        /*realSwapchainAvailable=*/true, /*streamlineRuntimeNonRetaining=*/false,
        /*holdsPromotedReference=*/true, /*releasesBaseReference=*/false));

    // Base reference only (no promoted interfaces) is still a CE-owned reference.
    EXPECT_TRUE(ShouldHoldRealSwapchainDiagnosticReferenceDuringWrapperDestructor(true, false, false, true));
    EXPECT_TRUE(ShouldHoldRealSwapchainDiagnosticReferenceDuringWrapperDestructor(true, false, true, true));

    // CE owns nothing on the chain: there is no reference to take, and nothing may be touched.
    EXPECT_FALSE(ShouldHoldRealSwapchainDiagnosticReferenceDuringWrapperDestructor(true, false, false, false));

    // No real chain at all.
    EXPECT_FALSE(ShouldHoldRealSwapchainDiagnosticReferenceDuringWrapperDestructor(false, false, true, true));

    // The non-retaining Streamline-runtime wrapper borrows the runtime's creation reference and must
    // never add one: an extra ref pins the old chain across Streamline's FG recreation.
    EXPECT_FALSE(ShouldHoldRealSwapchainDiagnosticReferenceDuringWrapperDestructor(true, true, true, true));
    EXPECT_FALSE(ShouldHoldRealSwapchainDiagnosticReferenceDuringWrapperDestructor(true, true, false, true));
}

TEST(SwapChainProbeLifetimeTest, DestructorTakesTheReferenceBeforeReleasingAndDropsItLast) {
    const std::string wrapper = ReadProjectSource("hook/wrappers/dxgi_swapchain_wrap_lifetime.cpp");

    const size_t decision = wrapper.find("ShouldHoldRealSwapchainDiagnosticReferenceDuringWrapperDestructor(");
    ASSERT_NE(decision, std::string::npos) << "the destructor must decide whether it may hold a reference";
    const size_t guardAddRef = wrapper.find("pRealToFree->AddRef();", decision);
    ASSERT_NE(guardAddRef, std::string::npos);
    const size_t firstPromotedRelease = wrapper.find("pReal4ToFree->Release();");
    ASSERT_NE(firstPromotedRelease, std::string::npos);
    EXPECT_LT(guardAddRef, firstPromotedRelease)
        << "the diagnostic reference must be taken BEFORE the promoted releases - after them the chain "
           "can already be freed, which is the 20260819_000437 heap corruption";

    const size_t residualRelease = wrapper.find("residualRefs = pRealToFree->Release();");
    ASSERT_NE(residualRelease, std::string::npos)
        << "the diagnostic reference must be dropped explicitly, and its return value is the residual count";
    const size_t probeLog = wrapper.find("post-destruction real refcount=%u");
    ASSERT_NE(probeLog, std::string::npos);
    EXPECT_LT(probeLog, residualRelease) << "every diagnostic must run while the reference is still held";
    const size_t attribution = wrapper.find("FinishSwapchainLifetimeAttribution(pRealToFree)");
    ASSERT_NE(attribution, std::string::npos);
    EXPECT_LT(attribution, residualRelease);

    // Nothing may dereference the chain after the final release.
    const std::string residualStatement = "residualRefs = pRealToFree->Release();";
    EXPECT_EQ(wrapper.find("pRealToFree->", residualRelease + residualStatement.size()), std::string::npos)
        << "the real chain must never be touched again after CE drops its last reference";

    // The discredited liveness test must not come back in this destructor.
    EXPECT_EQ(wrapper.find("VirtualQuery"), std::string::npos)
        << "VirtualQuery cannot distinguish a live COM object from a freed one - a freed heap block "
           "stays MEM_COMMIT - so it must not be used to justify touching the chain";
}

TEST(SwapChainProbeLifetimeTest, PinDiagnosticsNeverDereferenceRawTrackedPointers) {
    const std::string tracking = ReadProjectSource("hook/apis/dx12_hook_swapchain_tracking.cpp");

    const size_t diagnostics = tracking.find("void LogAccessDeniedSwapchainPinDiagnostics(");
    ASSERT_NE(diagnostics, std::string::npos);
    const size_t diagnosticsEnd = tracking.find("\n}\n", diagnostics);
    ASSERT_NE(diagnosticsEnd, std::string::npos);
    const std::string body = tracking.substr(diagnostics, diagnosticsEnd - diagnostics);

    // The tracked pointers are raw and nothing removes them when a chain dies, so any call through
    // them can hit a corpse - including the AddRef/Release "probe" this used to do.
    EXPECT_EQ(body.find("chain->AddRef()"), std::string::npos);
    EXPECT_EQ(body.find("chain->Release()"), std::string::npos);
    EXPECT_EQ(body.find("VirtualQuery"), std::string::npos);
    EXPECT_NE(body.find("ce::swapchain_liveness::Query(chain)"), std::string::npos)
        << "the residual pin count must come from CE's own recorded observation instead";

    // Re-tracking an address means a live chain sits there now, so a stale note must be dropped.
    EXPECT_NE(tracking.find("ce::swapchain_liveness::ForgetNote(pSwapChain);"), std::string::npos);
}

TEST(SwapChainLivenessLedgerTest, RecordsAndForgetsObservations) {
    ce::swapchain_liveness::ResetForTesting();

    const void* chainA = reinterpret_cast<const void*>(0x1000);
    const void* chainB = reinterpret_cast<const void*>(0x2000);

    EXPECT_FALSE(ce::swapchain_liveness::Query(chainA).known);
    EXPECT_FALSE(ce::swapchain_liveness::Query(nullptr).known);

    ce::swapchain_liveness::NoteCeReleasedLastOwnedReference(chainA, 3);
    const auto noteA = ce::swapchain_liveness::Query(chainA);
    EXPECT_TRUE(noteA.known);
    EXPECT_TRUE(noteA.ceReleasedLastOwnedReference);
    EXPECT_EQ(noteA.residualRefsAtCeRelease, 3u);

    // The common case: CE's last release destroyed the chain, so nothing pins it.
    ce::swapchain_liveness::NoteCeReleasedLastOwnedReference(chainB, 0);
    const auto noteB = ce::swapchain_liveness::Query(chainB);
    EXPECT_TRUE(noteB.known);
    EXPECT_TRUE(noteB.ceReleasedLastOwnedReference);
    EXPECT_EQ(noteB.residualRefsAtCeRelease, 0u);

    // A live chain reusing the address invalidates the note; the allocator recycles addresses and a
    // dead chain's pin count must never be attributed to a live one.
    ce::swapchain_liveness::ForgetNote(chainA);
    EXPECT_FALSE(ce::swapchain_liveness::Query(chainA).known);
    EXPECT_TRUE(ce::swapchain_liveness::Query(chainB).known);

    ce::swapchain_liveness::ForgetNote(nullptr);
    ce::swapchain_liveness::NoteCeReleasedLastOwnedReference(nullptr, 5);
    EXPECT_EQ(ce::swapchain_liveness::NoteCount(), 1u);

    ce::swapchain_liveness::ResetForTesting();
    EXPECT_EQ(ce::swapchain_liveness::NoteCount(), 0u);
}

TEST(SwapChainLivenessLedgerTest, StaysBoundedAcrossManySwapchainRecreations) {
    ce::swapchain_liveness::ResetForTesting();

    const size_t maxNotes = ce::swapchain_liveness::MaxNotes();
    ASSERT_GT(maxNotes, 0u);
    const size_t recorded = maxNotes * 4;
    for (size_t i = 1; i <= recorded; ++i) {
        ce::swapchain_liveness::NoteCeReleasedLastOwnedReference(
            reinterpret_cast<const void*>(i * 0x100), static_cast<unsigned long>(i));
    }
    EXPECT_EQ(ce::swapchain_liveness::NoteCount(), maxNotes)
        << "FG switching recreates swapchains for hours; the ledger must not grow without limit";

    // The newest observation survives, the oldest is evicted.
    EXPECT_TRUE(ce::swapchain_liveness::Query(reinterpret_cast<const void*>(recorded * 0x100)).known);
    EXPECT_FALSE(ce::swapchain_liveness::Query(reinterpret_cast<const void*>(0x100)).known);

    // Re-recording an address must not add a second insertion-order entry for it.
    const void* repeated = reinterpret_cast<const void*>(recorded * 0x100);
    for (int i = 0; i < 10; ++i) {
        ce::swapchain_liveness::NoteCeReleasedLastOwnedReference(repeated, 7);
    }
    EXPECT_EQ(ce::swapchain_liveness::NoteCount(), maxNotes);
    EXPECT_EQ(ce::swapchain_liveness::Query(repeated).residualRefsAtCeRelease, 7u);

    ce::swapchain_liveness::ResetForTesting();
}

TEST(SwapChainProbeLifetimeTest, PromoteInterfacesDoesNotDuplicateReferencesWhenAlreadyPopulated) {
    const std::string present = ReadProjectSource("hook/wrappers/dxgi_swapchain_wrap_present.cpp");

    const size_t promote = present.find("void CWrapDXGISwapChain::PromoteInterfaces()");
    ASSERT_NE(promote, std::string::npos);
    const size_t promoteEnd = present.find("\n}\n", promote);
    ASSERT_NE(promoteEnd, std::string::npos);
    const std::string body = present.substr(promote, promoteEnd - promote);

    // When wrapping IDXGISwapChain1..4, the constructor already stores and AddRefs m_pReal1..N.
    // Promoting interfaces must check !m_pRealN before calling QueryInterface so it never overwrites
    // an existing interface pointer and leaks the duplicate reference (dumps 20260904_143301).
    EXPECT_NE(body.find("if (!m_pReal1 && SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1))))"),
              std::string::npos);
    EXPECT_NE(body.find("if (!m_pReal2 && SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal2))))"),
              std::string::npos);
    EXPECT_NE(body.find("if (!m_pReal3 && SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal3))))"),
              std::string::npos);
    EXPECT_NE(body.find("if (!m_pReal4 && SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal4))))"),
              std::string::npos);
}

TEST(SwapChainProbeLifetimeTest, GlobalSwapchainDetoursConsumeFactoryReferenceAfterWrapping) {
    const std::string create = ReadProjectSource("hook/apis/dx12_hook_swapchain_create.cpp");

    const size_t createGlobal = create.find("HRESULT STDMETHODCALLTYPE DetourCreateSwapChainGlobal(");
    ASSERT_NE(createGlobal, std::string::npos);

    // The factory returns an owning reference (ref=1). CWrapDXGISwapChain constructor AddRefs (ref=2).
    // The detour must consume the factory's returned reference so the caller's eventual wrapper Release
    // drops the real swapchain to 0 instead of leaking a reference.
    const size_t wrapSite = create.find("*ppSwapChain = new CWrapDXGISwapChain(*ppSwapChain, pDevice);", createGlobal);
    ASSERT_NE(wrapSite, std::string::npos);
    const size_t releaseSite = create.find("pReal->Release();", wrapSite);
    EXPECT_NE(releaseSite, std::string::npos);

    const size_t createHwndGlobal = create.find("HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwndGlobal(");
    ASSERT_NE(createHwndGlobal, std::string::npos);
    EXPECT_GT(createHwndGlobal, releaseSite);

    const size_t wrapHwndSite =
        create.find("*ppSC = (IDXGISwapChain1*)new CWrapDXGISwapChain(pReal, pDevice);", createHwndGlobal);
    ASSERT_NE(wrapHwndSite, std::string::npos);
    const size_t releaseHwndSite = create.find("pReal->Release();", wrapHwndSite);
    EXPECT_NE(releaseHwndSite, std::string::npos);
}

TEST(SwapChainProbeLifetimeTest, StreamlineLifecyclePreservesRuntimeAcrossModeSwitches) {
    const std::string render = ReadProjectSource("testapp/dx12_fg_switch_render.cpp");

    // Mode transitions to FSR or Native OFF must NOT unload Streamline mid-process.
    // Unloading sl.interposer.dll frees sl.dlss_g.dll while NVIDIA driver (160_E658703.bin)
    // retains registered telemetry callback pointers, causing DEP crash 0xC0000005 on DLSS return.
    const size_t fsrFunc = render.find("bool ReinitializeDX12ForFSR(");
    ASSERT_NE(fsrFunc, std::string::npos);
    const size_t fsrFuncEnd = render.find("\n}\n", fsrFunc);
    ASSERT_NE(fsrFuncEnd, std::string::npos);
    const std::string fsrBody = render.substr(fsrFunc, fsrFuncEnd - fsrFunc);
    EXPECT_EQ(fsrBody.find("ShutdownStreamlineSerialized"), std::string::npos);

    const size_t offFunc = render.find("bool ReinitializeDX12ForNativeOff(");
    ASSERT_NE(offFunc, std::string::npos);
    const size_t offFuncEnd = render.find("\n}\n", offFunc);
    ASSERT_NE(offFuncEnd, std::string::npos);
    const std::string offBody = render.substr(offFunc, offFuncEnd - offFunc);
    EXPECT_EQ(offBody.find("ShutdownStreamlineSerialized"), std::string::npos);

    const std::string swapchain = ReadProjectSource("testapp/dx12_fg_switch_swapchain.cpp");
    EXPECT_NE(swapchain.find("bool CreateSwapChainResources(HWND hwnd, bool useFfxSwapChain, bool useStreamlineSwapChain"),
              std::string::npos);
}

// Session testappslowswitchtodlssfgstoppedworkingfromfsrfgtodlssfg: after DLSS -> FSR the renderer
// re-created its device and asked Streamline to bind it again. slSetD3DDevice succeeds once per slInit
// and returned eErrorInvalidIntegration (19), which the app recorded as "unbound" and then refused every
// later FSR -> DLSS switch. Only BindStreamlineDevice may call it, and it must reuse an existing binding.
TEST(SwapChainProbeLifetimeTest, StreamlineDeviceBindingIsRequestedOncePerStreamlineInit) {
    const std::string streamline = ReadProjectSource("testapp/dx12_fg_switch_streamline.cpp");
    ASSERT_FALSE(streamline.empty());
    const char* callers[] = {"testapp/dx12_fg_switch_render.cpp", "testapp/dx12_fg_switch_swapchain.cpp",
                             "testapp/dx12_fg_switch_test.cpp", "testapp/dx12_fg_switch_render_switch.cpp"};
    for (const char* path : callers) {
        const std::string source = ReadProjectSource(path);
        ASSERT_FALSE(source.empty()) << path;
        EXPECT_EQ(source.find("g_SlSetD3DDevice("), std::string::npos) << path;
    }

    const size_t bind = streamline.find("bool BindStreamlineDevice(const char* reason)");
    ASSERT_NE(bind, std::string::npos);
    const size_t bindEnd = streamline.find("\n}\n", bind);
    ASSERT_NE(bindEnd, std::string::npos);
    const std::string body = streamline.substr(bind, bindEnd - bind);
    const size_t reuse = body.find("if (dx12_fg_switch_test_g_SlBoundDevice)");
    const size_t call = body.find("dx12_fg_switch_test_g_SlSetD3DDevice(g_Device.Get())");
    ASSERT_NE(reuse, std::string::npos);
    ASSERT_NE(call, std::string::npos);
    EXPECT_LT(reuse, call);
    EXPECT_NE(body.find("dx12_fg_switch_test_g_SlBoundDevice = g_Device"), std::string::npos);
    EXPECT_EQ(streamline.find("g_SlSetD3DDevice(", bindEnd), std::string::npos);
    EXPECT_NE(streamline.find("dx12_fg_switch_test_g_SlBoundDevice.Reset()"), std::string::npos);
}
