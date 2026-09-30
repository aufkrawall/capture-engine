#include <gtest/gtest.h>

#include <d3d12.h>
#include <dxgi.h>

#include <filesystem>
#include <string>

#include "../hook/common/backbuffer_reference_trace.h"
#include "source_fragment_reader.h"

namespace trace = ce::backbuffer_reference_trace;

namespace {

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

// A reference-counted stand-in for a swapchain back buffer. Only IUnknown is
// ever called through it, so the rest of ID3D12Resource is never reached.
class FakeBuffer : public IUnknown {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
        *out = this;
        AddRefInternal();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return AddRefInternal(); }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    ULONG AddRefInternal() { return ++refs; }
    ULONG refs = 1;
};

// Only GetBuffer is used; every other IDXGISwapChain method is a stub.
class FakeSwapChain : public IDXGISwapChain {
public:
    FakeBuffer buffers[3];
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID, void** out) override {
        if (index >= 3) {
            *out = nullptr;
            return DXGI_ERROR_INVALID_CALL;
        }
        buffers[index].AddRefInternal();  // DXGI takes the reference internally
        *out = &buffers[index];
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Present(UINT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL, IDXGIOutput*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL*, IDXGIOutput**) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT, UINT, UINT, DXGI_FORMAT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT*) override { return E_NOTIMPL; }
};

// A holder that keeps what it takes: the module-level leak the trace exists to find.
__attribute__((noinline)) IUnknown* LeakyHolderTakes(IDXGISwapChain* chain, UINT index) {
    void* buffer = nullptr;
    chain->GetBuffer(index, __uuidof(IUnknown), &buffer);
    return static_cast<IUnknown*>(buffer);
}

__attribute__((noinline)) void BalancedUserTouches(IUnknown* buffer) {
    buffer->AddRef();
    buffer->Release();
}

}  // namespace

TEST(BackBufferReferenceTraceTest, RecordsPerSiteAndReportsOverflow) {
    trace::SiteTally table[3];
    EXPECT_TRUE(trace::Record(table, 3, 0x1000, true));
    EXPECT_TRUE(trace::Record(table, 3, 0x1000, true));
    EXPECT_TRUE(trace::Record(table, 3, 0x1000, false));
    EXPECT_TRUE(trace::Record(table, 3, 0x2000, false));
    EXPECT_TRUE(trace::Record(table, 3, 0x3000, true));
    EXPECT_FALSE(trace::Record(table, 3, 0x4000, true)) << "a full table must say so";
    EXPECT_FALSE(trace::Record(table, 3, 0, true));
    EXPECT_EQ(table[0].site.load(), 0x1000u);
    EXPECT_EQ(table[0].acquired.load(), 2);
    EXPECT_EQ(table[0].released.load(), 1);
    EXPECT_EQ(table[1].released.load(), 1);

    trace::Clear(table, 3);
    for (const auto& entry : table) {
        EXPECT_EQ(entry.site.load(), 0u);
        EXPECT_EQ(entry.acquired.load(), 0);
        EXPECT_EQ(entry.released.load(), 0);
    }
}

// End to end on fake COM objects: only the registered buffers are counted, the
// swapchain's vtable is never written (a slot-hooking overlay skips a function
// that jumps into another module - logs/20260927_031545), and uninstalling restores
// every patched resource slot.
TEST(BackBufferReferenceTraceTest, CountsOnlyTrackedBuffersAndRestoresTheVtables) {
    FakeSwapChain chain;
    FakeBuffer untracked;
    // Volatile: the compiler may treat vtable contents as invariant and fold
    // the reads across the patch.
    // NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign) - the compiler-generated vptr is set by the constructor
    void* volatile* bufferVtable = *reinterpret_cast<void* volatile**>(&chain.buffers[0]);
    void* volatile* chainVtable = *reinterpret_cast<void* volatile**>(&chain);
    void* const originalAddRef = bufferVtable[1];
    void* const originalGetBuffer = chainVtable[9];

    BackBufferReferenceTrace_Track(&chain, 3, "unit-test");
    EXPECT_NE(bufferVtable[1], originalAddRef);
    EXPECT_EQ(chainVtable[9], originalGetBuffer) << "the swapchain's GetBuffer slot must stay pristine";
    for (const FakeBuffer& buffer : chain.buffers) {
        EXPECT_EQ(buffer.refs, 1u) << "registering must leave the counts as it found them";
    }

    IUnknown* leaked = LeakyHolderTakes(&chain, 1);
    BalancedUserTouches(leaked);
    BalancedUserTouches(&untracked);
    EXPECT_EQ(chain.buffers[1].refs, 2u);
    EXPECT_EQ(untracked.refs, 1u);

    // The log call walks the tables; it must not disturb the counts.
    BackBufferReferenceTrace_Log(&chain, "unit-test");
    EXPECT_EQ(chain.buffers[1].refs, 2u);

    BackBufferReferenceTrace_Uninstall();
    EXPECT_EQ(bufferVtable[1], originalAddRef);
    EXPECT_EQ(chainVtable[9], originalGetBuffer);
    leaked->Release();
}

// DXGI's GetBuffer is hooked below its entry, and only while the slot still names DXGI's
// function: a slot-hooking overlay skips a function whose entry already jumps elsewhere.
TEST(BackBufferReferenceTraceTest, GetBufferIsHookedBelowDxgisEntryNeverInTheSlotOrAtTheEntry) {
    const std::string source = ReadSource("hook/common/backbuffer_reference_trace.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_EQ(source.find("PatchSlot(swapChainVtable"), std::string::npos);
    EXPECT_EQ(source.find("InlineHook::InstallPublished(getBufferTarget"), std::string::npos);
    const size_t owner = source.find("DXGIShared::IsAddressInsideSystemDXGI(getBufferTarget)");
    const size_t install = source.find("InlineHook::InstallDeepHookPublished(getBufferTarget");
    ASSERT_NE(owner, std::string::npos);
    ASSERT_NE(install, std::string::npos);
    EXPECT_LT(owner, install);
}

// Armed after every successful D3D12 resize, read out on a refused one.
TEST(BackBufferReferenceTraceTest, ResizeDiagnosticsArmTheTraceOnSuccessAndReadItOnFailure) {
    const std::string resize = ReadSource("hook/common/dxgi_shared_resize.cpp");
    const size_t begin = resize.find("void EndD3D12ResizeDiagnostics(");
    ASSERT_NE(begin, std::string::npos);
    const std::string body = resize.substr(begin, resize.find("\n}\n", begin) - begin);
    const size_t throttle = body.find("s_succeededLogs");
    const size_t track = body.find("BackBufferReferenceTrace_Track(pSwapChain, preparation.bufferCount, source)");
    const size_t log = body.find("BackBufferReferenceTrace_Log(pSwapChain, source)");
    ASSERT_NE(track, std::string::npos);
    ASSERT_NE(log, std::string::npos);
    ASSERT_NE(throttle, std::string::npos);
    EXPECT_LT(track, throttle) << "the log throttle must never stop the trace from following a new chain";
    EXPECT_LT(log, throttle);

    const std::string teardown = ReadSource("hook/common/dxgi_shared_hooks_present_vtable.cpp");
    EXPECT_NE(teardown.find("BackBufferReferenceTrace_Uninstall();"), std::string::npos);
}
