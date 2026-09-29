#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../hook/capture/capture_swapchain_binding.h"
#include "source_fragment_reader.h"

namespace {

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

std::string FunctionBody(const std::string& source, const std::string& signature, const std::string& nextSignature) {
    const size_t begin = source.find(signature);
    if (begin == std::string::npos)
        return {};
    const size_t end = source.find(nextSignature, begin + signature.size());
    return source.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

// A COM object whose reference count the binding must leave exactly as it
// found it. The game's final Release then really destroys the chain, and DXGI
// accepts the next swapchain on the HWND.
class FakeSwapChain : public IUnknown {
public:
    explicit FakeSwapChain(bool answersIdentity = true) : m_AnswersIdentity(answersIdentity) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) {
            return E_POINTER;
        }
        *out = nullptr;
        if (!m_AnswersIdentity || riid != __uuidof(IUnknown)) {
            return E_NOINTERFACE;
        }
        AddRef();
        *out = static_cast<IUnknown*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_Refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --m_Refs; }

    ULONG Refs() const { return m_Refs; }

private:
    bool m_AnswersIdentity;
    ULONG m_Refs = 1;  // the game's own reference
};

}  // namespace

// logs/taloscrashdlssfgtofsrfg: a recording under DLSS FG bound the capture to
// Streamline's chain with two owned references; the later FSR FG switch had its
// swapchain create denied and the game terminated.
TEST(CaptureSwapChainBindingTest, BindingHoldsNoReferenceOnTheSwapChain) {
    FakeSwapChain chain;
    ce::capture::SwapChainIdentityBinding binding;

    ASSERT_TRUE(binding.Bind(&chain));
    EXPECT_EQ(chain.Refs(), 1u);
    EXPECT_TRUE(binding.IsBound());
    EXPECT_EQ(binding.Key(), static_cast<const void*>(static_cast<IUnknown*>(&chain)));

    EXPECT_TRUE(binding.Matches(&chain));
    EXPECT_EQ(chain.Refs(), 1u);

    // The game releases its last reference: nothing of the capture keeps it.
    EXPECT_EQ(chain.Release(), 0u);
}

TEST(CaptureSwapChainBindingTest, MatchesOnlyTheBoundIdentity) {
    FakeSwapChain bound;
    FakeSwapChain other;
    ce::capture::SwapChainIdentityBinding binding;

    ASSERT_TRUE(binding.Bind(&bound));
    EXPECT_TRUE(binding.Matches(&bound));
    EXPECT_FALSE(binding.Matches(&other));
    EXPECT_FALSE(binding.Matches(nullptr));
    EXPECT_EQ(other.Refs(), 1u);

    binding.Clear();
    EXPECT_FALSE(binding.IsBound());
    EXPECT_EQ(binding.Key(), nullptr);
    EXPECT_FALSE(binding.Matches(&bound));
}

TEST(CaptureSwapChainBindingTest, ObjectWithoutIdentityDoesNotBind) {
    FakeSwapChain noIdentity(false);
    ce::capture::SwapChainIdentityBinding binding;

    EXPECT_FALSE(binding.Bind(&noIdentity));
    EXPECT_FALSE(binding.IsBound());
    EXPECT_FALSE(binding.Matches(&noIdentity));
    EXPECT_FALSE(binding.Bind(nullptr));
    EXPECT_EQ(noIdentity.Refs(), 1u);
}

// The D3D12 capture keeps no swapchain COM pointer at all, in its live
// generation or in a retired one, and copies from the chain its caller presents.
TEST(CaptureSwapChainBindingTest, D3D12SharedCaptureOwnsNoSwapChainReference) {
    const std::string header = ReadSource("hook/capture/shared_capture.h");
    ASSERT_FALSE(header.empty());
    const size_t d3d12Class = header.find("class SharedCaptureD3D12");
    const size_t managerClass = header.find("class CaptureManager");
    ASSERT_NE(d3d12Class, std::string::npos);
    ASSERT_NE(managerClass, std::string::npos);
    const std::string d3d12 = header.substr(d3d12Class, managerClass - d3d12Class);
    EXPECT_EQ(d3d12.find("ComPtr<IDXGISwapChain"), std::string::npos);
    EXPECT_EQ(d3d12.find("ComPtr<IUnknown>"), std::string::npos);
    EXPECT_NE(d3d12.find("ce::capture::SwapChainIdentityBinding m_SwapChainBinding;"), std::string::npos);

    const std::string source = ReadSource("hook/capture/shared_capture_d3d12.cpp");
    ASSERT_FALSE(source.empty());
    const std::string capture =
        FunctionBody(source, "bool SharedCaptureD3D12::CaptureFrame(", "bool SharedCaptureD3D12::GetCurrentFrame(");
    ASSERT_FALSE(capture.empty());
    const size_t match = capture.find("m_SwapChainBinding.Matches(pSwapChain)");
    const size_t getBuffer = capture.find("pSwapChain->GetBuffer(backBufferIndex");
    ASSERT_NE(match, std::string::npos);
    ASSERT_NE(getBuffer, std::string::npos);
    EXPECT_LT(match, getBuffer);
}

// The never-instantiated SharedCaptureD3D11 kept a ComPtr<IDXGISwapChain> for
// its whole lifetime, the pattern that denied Talos its FSR FG swapchain. No
// shared capture target may own a swapchain reference.
TEST(CaptureSwapChainBindingTest, SharedCaptureTargetsOwnNoSwapChainReference) {
    for (const char* path : {"hook/capture/shared_capture.h", "hook/capture/shared_capture.cpp"}) {
        const std::string source = ReadSource(path);
        ASSERT_FALSE(source.empty()) << path;
        EXPECT_EQ(source.find("ComPtr<IDXGISwapChain"), std::string::npos) << path;
        EXPECT_EQ(source.find("SharedCaptureD3D11"), std::string::npos) << path;
    }
}
