#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <new>
#include <string>

#include "hook/capture/capture_swapchain_binding.h"
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

// A DXGI object with a real private-data store, so the binding can stamp it.
// `keepsPrivateData=false` models an object that accepts SetPrivateData but
// never returns it.
class FakeDxgiSwapChain final : public IDXGIObject {
public:
    explicit FakeDxgiSwapChain(bool keepsPrivateData = true) : m_KeepsPrivateData(keepsPrivateData) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) {
            return E_POINTER;
        }
        *out = nullptr;
        if (riid != __uuidof(IUnknown) && riid != __uuidof(IDXGIObject)) {
            return E_NOINTERFACE;
        }
        AddRef();
        *out = static_cast<IDXGIObject*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_Refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --m_Refs; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override {
        if (guid != ce::capture::kCaptureBindingStampGuid || size != sizeof(m_Stamp) || !data) {
            return E_INVALIDARG;
        }
        if (m_KeepsPrivateData) {
            std::memcpy(&m_Stamp, data, sizeof(m_Stamp));
            m_HasStamp = true;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override {
        if (!size || guid != ce::capture::kCaptureBindingStampGuid || !m_HasStamp) {
            return DXGI_ERROR_NOT_FOUND;
        }
        if (!data || *size < sizeof(m_Stamp)) {
            *size = sizeof(m_Stamp);
            return DXGI_ERROR_MORE_DATA;
        }
        std::memcpy(data, &m_Stamp, sizeof(m_Stamp));
        *size = sizeof(m_Stamp);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void** parent) override {
        if (parent) {
            *parent = nullptr;
        }
        return E_NOTIMPL;
    }

    ULONG Refs() const { return m_Refs; }

private:
    bool m_KeepsPrivateData;
    bool m_HasStamp = false;
    uint64_t m_Stamp = 0;
    ULONG m_Refs = 1;  // the game's own reference
};

}  // namespace

// FlowInjectFGSwitch.CallbackFSRKeepsRecording (2026-10-15): the allocator put
// the next swapchain into the released chain's block. Same size and format, so
// an identity-only binding kept the old capture generation and no new transport
// generation began - one generation across three swapchains under load.
TEST(CaptureSwapChainBindingTest, NewChainAtReusedAddressDoesNotInheritTheBinding) {
    alignas(FakeDxgiSwapChain) unsigned char storage[sizeof(FakeDxgiSwapChain)];
    auto* departing = new (storage) FakeDxgiSwapChain();
    ce::capture::SwapChainIdentityBinding binding;

    ASSERT_TRUE(binding.Bind(departing));
    EXPECT_NE(binding.Stamp(), 0u);
    EXPECT_EQ(binding.Classify(departing), ce::capture::SwapChainBindingMatch::kMatch);
    EXPECT_EQ(departing->Refs(), 1u);
    const void* departingKey = binding.Key();
    departing->~FakeDxgiSwapChain();

    auto* arriving = new (storage) FakeDxgiSwapChain();
    ASSERT_EQ(ce::capture::ResolveComIdentityKey(arriving), departingKey);
    EXPECT_EQ(binding.Classify(arriving), ce::capture::SwapChainBindingMatch::kReusedAddress);
    EXPECT_FALSE(binding.Matches(arriving));
    EXPECT_EQ(arriving->Refs(), 1u);

    // Re-initializing for the arriving chain binds it under a fresh stamp.
    const uint64_t departingStamp = binding.Stamp();
    ASSERT_TRUE(binding.Bind(arriving));
    EXPECT_NE(binding.Stamp(), departingStamp);
    EXPECT_TRUE(binding.Matches(arriving));
    EXPECT_EQ(arriving->Refs(), 1u);
    arriving->~FakeDxgiSwapChain();
}

TEST(CaptureSwapChainBindingTest, StampedBindingStillMatchesOnlyTheBoundObject) {
    FakeDxgiSwapChain bound;
    FakeDxgiSwapChain other;
    ce::capture::SwapChainIdentityBinding binding;

    EXPECT_EQ(binding.Classify(&bound), ce::capture::SwapChainBindingMatch::kUnbound);
    ASSERT_TRUE(binding.Bind(&bound));
    for (int present = 0; present < 3; ++present) {
        EXPECT_TRUE(binding.Matches(&bound));
    }
    EXPECT_EQ(binding.Classify(&other), ce::capture::SwapChainBindingMatch::kOtherIdentity);
    EXPECT_EQ(bound.Refs(), 1u);
    EXPECT_EQ(other.Refs(), 1u);

    binding.Clear();
    EXPECT_EQ(binding.Stamp(), 0u);
    EXPECT_FALSE(binding.Matches(&bound));
}

// An object that drops private data must not look like a different chain on
// every present (which would re-initialize the capture each frame): the binding
// verifies the stamp at Bind and otherwise falls back to identity alone.
TEST(CaptureSwapChainBindingTest, ObjectThatDropsPrivateDataFallsBackToIdentity) {
    FakeDxgiSwapChain forgetful(false);
    ce::capture::SwapChainIdentityBinding binding;

    ASSERT_TRUE(binding.Bind(&forgetful));
    EXPECT_EQ(binding.Stamp(), 0u);
    EXPECT_TRUE(binding.Matches(&forgetful));
    EXPECT_EQ(forgetful.Refs(), 1u);
}

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
    EXPECT_EQ(binding.Stamp(), 0u);  // no IDXGIObject: identity alone
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
