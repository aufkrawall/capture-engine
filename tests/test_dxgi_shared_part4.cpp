#include "test_dxgi_shared_shared.h"

TEST(DXGISharedSourceTest, GetStateFirstPostFSRComebackClearsStaleNativeOwnershipOnExplicitUpgrade) {
    namespace fs = std::filesystem;
    const std::string streamline =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "apis" / "streamline_hook.cpp");
    const std::string dx12 =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "apis" / "dx12_hook.cpp");
    ASSERT_FALSE(streamline.empty());
    ASSERT_FALSE(dx12.empty());

    const size_t upgrade = streamline.find(
        "if (!previousExplicitSetOptionsActivation && updatedExplicitSetOptionsActivation && "
        "signalUpdate.effectiveActive");
    const size_t ownershipRefresh =
        streamline.find("DX12_OnStreamlineExplicitSetOptionsActivationConfirmed();", upgrade);
    const size_t ordinaryEdge = streamline.find("if (previousSignalObserved != signalUpdate.effectiveActive)", upgrade);
    ASSERT_NE(upgrade, std::string::npos);
    ASSERT_NE(ownershipRefresh, std::string::npos);
    ASSERT_NE(ordinaryEdge, std::string::npos);
    EXPECT_LT(ownershipRefresh, ordinaryEdge)
        << "an in-place GetState-to-SetOptions provenance upgrade has no second ON edge";

    const size_t staleOwnershipHelper = dx12.find(
        "bool explicitSetOptionsActivation, bool authoritativeStreamlineHandoff, const char* source) {");
    const size_t staleOwnershipPolicy = dx12.find(
        "ShouldClearStaleNativeFGPresentOwnershipOnStreamlineComeback(", staleOwnershipHelper);
    const size_t noCallbackOwnershipProof =
        dx12.find("DX12_IsNativeFSRInternalNoCallbackCompositionActive()", staleOwnershipHelper);
    const size_t clearNoCallback = dx12.find("ForceClearNativeFSRInternalNoCallbackComposition(", staleOwnershipPolicy);
    const size_t implementation =
        dx12.find("void DX12_OnStreamlineExplicitSetOptionsActivationConfirmed() {");
    const size_t helperCall =
        dx12.find("ClearStaleNativeFGPresentOwnershipForStreamlineComebackLocked(", implementation);
    ASSERT_NE(staleOwnershipHelper, std::string::npos);
    ASSERT_NE(staleOwnershipPolicy, std::string::npos);
    ASSERT_NE(noCallbackOwnershipProof, std::string::npos);
    ASSERT_NE(clearNoCallback, std::string::npos);
    EXPECT_LT(noCallbackOwnershipProof, staleOwnershipPolicy);
    ASSERT_NE(implementation, std::string::npos);
    ASSERT_NE(helperCall, std::string::npos);
}

TEST(DXGISharedSourceTest, ExactPostSLOffKeepAliveRunsBeforeEveryTopLevelDX12PresentRoute) {
    namespace fs = std::filesystem;
    const std::string text =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "common" / "dxgi_shared.cpp");
    ASSERT_FALSE(text.empty());

    const size_t present = text.find(
        "HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {");
    const size_t presentScope = text.find("BeginPostSLOffKeepAlivePresentScope();", present);
    const size_t presentKeepAlive = text.find("\"DXGIShared::DetourPresent pre-routing\"", presentScope);
    const size_t presentRouting = text.find("const void* detourCallerAddress", presentScope);
    ASSERT_NE(present, std::string::npos);
    ASSERT_NE(presentScope, std::string::npos);
    ASSERT_NE(presentKeepAlive, std::string::npos);
    ASSERT_NE(presentRouting, std::string::npos);
    EXPECT_LT(presentScope, presentKeepAlive);
    EXPECT_LT(presentKeepAlive, presentRouting);

    const size_t present1 = text.find(
        "HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,",
        presentRouting);
    const size_t present1Scope = text.find("BeginPostSLOffKeepAlivePresentScope();", present1);
    const size_t present1KeepAlive = text.find("\"DXGIShared::DetourPresent1 pre-routing\"", present1Scope);
    const size_t present1Routing = text.find("const void* detourCallerAddress", present1Scope);
    ASSERT_NE(present1, std::string::npos);
    ASSERT_NE(present1Scope, std::string::npos);
    ASSERT_NE(present1KeepAlive, std::string::npos);
    ASSERT_NE(present1Routing, std::string::npos);
    EXPECT_LT(present1Scope, present1KeepAlive);
    EXPECT_LT(present1KeepAlive, present1Routing);
}
