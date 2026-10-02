#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "hook/present/resize_reference_probe.h"
#include "source_fragment_reader.h"

namespace probe = ce::resize_reference_probe;

namespace {

// A back buffer whose reference count the probe must leave exactly as it found
// it: GetBuffer adds one, the probe's Release takes it back.
struct FakeBuffer {
    ULONG refs = 1;
    ULONG AddRef() { return ++refs; }
    ULONG Release() { return --refs; }
};

struct FakeSwapChain {
    std::vector<FakeBuffer> buffers;
    bool refuseInterface = false;
    HRESULT GetBuffer(UINT index, REFIID, void** out) {
        if (refuseInterface || index >= buffers.size()) {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        buffers[index].AddRef();
        *out = &buffers[index];
        return S_OK;
    }
};

std::string Formatted(const probe::BackBufferReferences& references) {
    char text[128] = {};
    probe::Format(references, text, sizeof(text));
    return text;
}

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

// A function's text, from its signature to the closing brace at column 0.
std::string FunctionBody(const std::string& source, const std::string& signaturePrefix) {
    const size_t begin = source.find(signaturePrefix);
    if (begin == std::string::npos)
        return {};
    const size_t end = source.find("\n}\n", begin);
    return source.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

}  // namespace

TEST(ResizeReferenceProbeTest, ReportsWhatOthersHoldAndLeavesCountsUnchanged) {
    FakeSwapChain swapChain;
    swapChain.buffers = {FakeBuffer{1}, FakeBuffer{3}, FakeBuffer{1}};
    const auto references = probe::Probe<FakeBuffer>(&swapChain, 3, IID_IUnknown);
    ASSERT_EQ(references.probed, 3u);
    EXPECT_EQ(Formatted(references), "[1,3,1]");
    // An extra reference (the holder that makes DXGI refuse a resize) stands out.
    EXPECT_EQ(swapChain.buffers[1].refs, 3u);
    EXPECT_EQ(swapChain.buffers[0].refs, 1u);
}

TEST(ResizeReferenceProbeTest, AChainOfAnotherApiProbesEmpty) {
    FakeSwapChain swapChain;
    swapChain.buffers = {FakeBuffer{}, FakeBuffer{}};
    swapChain.refuseInterface = true;
    const auto references = probe::Probe<FakeBuffer>(&swapChain, 2, IID_IUnknown);
    EXPECT_EQ(references.probed, 0u);
    EXPECT_EQ(Formatted(references), "[]");
}

TEST(ResizeReferenceProbeTest, StopsAtTheFirstUnavailableBufferAndAtTheCap) {
    FakeSwapChain shortChain;
    shortChain.buffers = {FakeBuffer{}, FakeBuffer{}};
    EXPECT_EQ(probe::Probe<FakeBuffer>(&shortChain, 5, IID_IUnknown).probed, 2u);

    FakeSwapChain longChain;
    longChain.buffers.resize(probe::kMaxProbedBuffers + 4);
    EXPECT_EQ(probe::Probe<FakeBuffer>(&longChain, probe::kMaxProbedBuffers + 4, IID_IUnknown).probed,
              probe::kMaxProbedBuffers);
    EXPECT_EQ(probe::Probe<FakeBuffer>(static_cast<FakeSwapChain*>(nullptr), 3, IID_IUnknown).probed, 0u);
}

TEST(ResizeReferenceProbeTest, FormatNeverOverrunsASmallBuffer) {
    probe::BackBufferReferences references;
    references.probed = probe::kMaxProbedBuffers;
    for (UINT i = 0; i < references.probed; ++i)
        references.heldByOthers[i] = 1234567;
    char text[12];
    probe::Format(references, text, sizeof(text));
    EXPECT_LT(std::string(text).size(), sizeof(text));
}

TEST(ResizeReferenceProbeTest, HeldByOthersExcludesTheCallersOwnReferenceAndLeavesTheCount) {
    FakeBuffer buffer{1};  // only the caller's own GetBuffer reference
    EXPECT_EQ(probe::HeldByOthersBesidesCaller(&buffer), 0u);
    buffer.refs = 4;
    EXPECT_EQ(probe::HeldByOthersBesidesCaller(&buffer), 3u);
    EXPECT_EQ(buffer.refs, 4u);
    EXPECT_EQ(probe::HeldByOthersBesidesCaller(static_cast<FakeBuffer*>(nullptr)), 0u);
}

TEST(ResizeReferenceProbeTest, CaptureStagesLogABaselineThenOnlyCopiesThatAddReferences) {
    const probe::CaptureStageReferences clean{0, 0, 0, 0};
    probe::CaptureStageReferences executeHolds{0, 0, 1, 1};
    probe::CaptureStageReferences recordHolds{1, 2, 2, 2};
    probe::CaptureStageReferences alreadyHeld{3, 3, 3, 3};

    EXPECT_FALSE(probe::StagesAddedReferences(clean));
    EXPECT_TRUE(probe::StagesAddedReferences(executeHolds));
    EXPECT_TRUE(probe::StagesAddedReferences(recordHolds));
    // References that were there before the copy are not the copy's.
    EXPECT_FALSE(probe::StagesAddedReferences(alreadyHeld));

    for (UINT copy = 0; copy < probe::kBaselineCopiesPerGeneration; ++copy)
        EXPECT_TRUE(probe::ShouldLogCaptureStageReferences(copy, clean, 0));
    EXPECT_FALSE(probe::ShouldLogCaptureStageReferences(probe::kBaselineCopiesPerGeneration, clean, 0));
    EXPECT_FALSE(probe::ShouldLogCaptureStageReferences(500, alreadyHeld, 0));
    EXPECT_TRUE(probe::ShouldLogCaptureStageReferences(500, executeHolds, 0));
    EXPECT_TRUE(probe::ShouldLogCaptureStageReferences(500, executeHolds, 15));
    EXPECT_FALSE(probe::ShouldLogCaptureStageReferences(500, executeHolds, 16));
    EXPECT_TRUE(probe::ShouldLogCaptureStageReferences(500, executeHolds, 256));
}

// Talos Reawakened + FSR FG (logs/20260926_090625): every back buffer carried
// three extra references and the game's resize died of DXGI_ERROR_INVALID_CALL.
// The same [3,3,3] recurred with capture not bound to the chain at all
// (logs/20260926_094906), so capture is not that holder - but a capture that
// is copying from a chain still has to let it go before the resize forwards,
// and the back buffers are logged after that.
TEST(ResizeReferenceProbeTest, EveryResizeDetourReleasesTheCaptureBeforeForwarding) {
    const std::string resize = ReadSource("hook/present/dxgi_shared_resize.cpp");
    ASSERT_FALSE(resize.empty());

    const std::string prepare = FunctionBody(resize, "D3D12ResizePreparation PrepareD3D12Resize(");
    const size_t firstProbe = prepare.find("Probe<ID3D12Resource>");
    const size_t release = prepare.find("DX12_ReleaseCaptureForSwapChainResize(");
    const size_t secondProbe = prepare.find("Probe<ID3D12Resource>", release);
    ASSERT_NE(firstProbe, std::string::npos);
    ASSERT_NE(release, std::string::npos);
    ASSERT_NE(secondProbe, std::string::npos);
    EXPECT_LT(firstProbe, release);

    const struct {
        const char* detour;
        const char* forward;
    } detours[] = {
        {"HRESULT STDMETHODCALLTYPE DetourResizeBuffersReconcileOnly(", "dxgi_shared_oResizeBuffersReconcile(pSwapChain"},
        {"HRESULT STDMETHODCALLTYPE DetourResizeBuffers1ReconcileOnly(",
         "dxgi_shared_oResizeBuffers1Reconcile(pSwapChain"},
        {"HRESULT STDMETHODCALLTYPE DetourResizeBuffers(", "HRESULT hr = dxgi_shared_oResizeBuffers(pSwapChain"},
        {"HRESULT STDMETHODCALLTYPE DetourResizeBuffers1(", "HRESULT hr = dxgi_shared_oResizeBuffers1(pSwapChain"},
    };
    for (const auto& entry : detours) {
        const std::string body = FunctionBody(resize, entry.detour);
        ASSERT_FALSE(body.empty()) << entry.detour;
        const size_t prepared = body.find("PrepareD3D12Resize(pSwapChain)");
        ASSERT_NE(prepared, std::string::npos) << entry.detour;
        const size_t forwarded = body.find(entry.forward, prepared);
        EXPECT_NE(forwarded, std::string::npos) << entry.detour << " must forward after releasing the capture";
    }

    // The full detours also forward early: nested wrapper resizes, CE's own
    // wrapper chain and the first D3D12 resize of the process. A recording can
    // already copy from the chain on each of them.
    for (const char* detour :
         {"HRESULT STDMETHODCALLTYPE DetourResizeBuffers(", "HRESULT STDMETHODCALLTYPE DetourResizeBuffers1("}) {
        const std::string body = FunctionBody(resize, detour);
        const size_t shortcuts = body.find("wrapperResizeDepth.fetch_add(1) > 0");
        ASSERT_NE(shortcuts, std::string::npos) << detour;
        size_t releases = 0;
        for (size_t at = body.find("DX12_ReleaseCaptureForSwapChainResize(pSwapChain", shortcuts);
             at != std::string::npos; at = body.find("DX12_ReleaseCaptureForSwapChainResize(pSwapChain", at + 1))
            ++releases;
        size_t preparations = 0;
        for (size_t at = body.find("PrepareD3D12Resize(pSwapChain)", shortcuts); at != std::string::npos;
             at = body.find("PrepareD3D12Resize(pSwapChain)", at + 1))
            ++preparations;
        EXPECT_EQ(releases, 2u) << detour << ": nested and CE-wrapper forwards";
        EXPECT_EQ(preparations, 2u) << detour << ": first-resize and main forwards";
    }

    const std::string wrapper = ReadSource("hook/wrappers/dxgi_swapchain_wrap_modern.cpp");
    size_t wrapperReleases = 0;
    for (size_t at = wrapper.find("DX12_ReleaseCaptureForSwapChainResize(this"); at != std::string::npos;
         at = wrapper.find("DX12_ReleaseCaptureForSwapChainResize(this", at + 1))
        ++wrapperReleases;
    EXPECT_EQ(wrapperReleases, 2u) << "the wrapper's ResizeBuffers and ResizeBuffers1";
}

// The release waits for the capture's own copies before it drops anything the
// GPU may still execute, and never drops the command list or allocators of a
// copy that did not finish.
TEST(ResizeReferenceProbeTest, CaptureReleaseDrainsItsOwnCopiesBeforeReleasing) {
    const std::string capture = ReadSource("hook/capture/shared_capture_d3d12.cpp");
    const std::string release = FunctionBody(capture, "SharedCaptureD3D12::ResizeRelease SharedCaptureD3D12::ReleaseForSwapChainResize(");
    ASSERT_FALSE(release.empty());
    const size_t drain = release.find("SetEventOnCompletion(result.pendingFenceValue");
    const size_t reset = release.find("if (Reset())");
    const size_t listRelease = release.find("m_CommandList.Reset()");
    ASSERT_NE(drain, std::string::npos);
    ASSERT_NE(reset, std::string::npos);
    ASSERT_NE(listRelease, std::string::npos);
    EXPECT_LT(drain, reset);
    EXPECT_NE(release.find("if (!result.waitTimedOut)"), std::string::npos);
    EXPECT_LT(release.find("if (!result.waitTimedOut)"), listRelease);
}
