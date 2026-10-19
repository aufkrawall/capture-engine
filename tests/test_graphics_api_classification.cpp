#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <new>
#include <string>

#include "hook/present/dxgi_shared_detail/streamline_present_routing.h"
#include "hook/present/dxgi_shared_detail/types_and_state.h"
#include "hook/present/swapchain_api_classifier.h"
#include "hook/runtime/api_translator_modules.h"
#include "hook/runtime/graphics_api_identity.h"
#include "source_fragment_reader.h"

// Which API the overlay names and which overlay/capture path a swapchain takes. Three
// misclassifications found 2026-10-18: DXVK D3D11 games as D3D10, D3D11 games as "DX11.1"-"DX11.4"
// from interface queries the game never made, and vkd3d-proton as plain "DX12".
namespace {
using DXGIShared::APIType;
using DXGIShared::SelectPrimarySwapChainAPIType;
using ce::graphics_api_identity::ApiEvidenceCaller;
using ce::graphics_api_identity::ClassifyApiEvidenceCaller;

constexpr unsigned kFL10_0 = 0xa000;
constexpr unsigned kFL10_1 = 0xa100;
constexpr unsigned kFL11_0 = 0xb000;
constexpr unsigned kFL11_1 = 0xb100;
constexpr unsigned kFL12_1 = 0xc100;

// DXVK's D3D11DXGIDevice::QueryInterface answers ID3D10Device/ID3D10Device1 on every device, and its
// swapchain's GetDevice forwards there: a DXVK D3D11 swapchain answers both, like D3D10 on D3D11.
TEST(GraphicsApiClassificationTest, DxvkD3D11SwapchainsStayD3D11) {
    for (unsigned level : {kFL11_0, kFL11_1, kFL12_1}) {
        EXPECT_EQ(SelectPrimarySwapChainAPIType(false, true, true, false, level), APIType::D3D11) << level;
        // A D3D10 runtime some other module loaded does not make an 11-level device D3D10.
        EXPECT_EQ(SelectPrimarySwapChainAPIType(false, true, true, true, level), APIType::D3D11) << level;
    }
    // A D3D10-level D3D11 device without any D3D10 runtime is still D3D11.
    EXPECT_EQ(SelectPrimarySwapChainAPIType(false, true, true, false, kFL10_0), APIType::D3D11);
}

TEST(GraphicsApiClassificationTest, NativeAndDxvkD3D10SwapchainsStayD3D10) {
    // Windows' D3D10 on D3D11 and DXVK's d3d10core: a D3D10 runtime and a 10-level device.
    EXPECT_EQ(SelectPrimarySwapChainAPIType(false, true, true, true, kFL10_0), APIType::D3D10);
    EXPECT_EQ(SelectPrimarySwapChainAPIType(false, true, true, true, kFL10_1), APIType::D3D10);
    EXPECT_EQ(SelectPrimarySwapChainAPIType(false, false, true, false, 0), APIType::D3D10);
    EXPECT_EQ(SelectPrimarySwapChainAPIType(true, true, true, true, kFL10_1), APIType::D3D12);
}

// A swapchain whose device answers one API (ID3D11Device, or ID3D12Device with `d3d12`), counting
// GetDevice (IDXGISwapChain slot 7). It keeps DXGI private data (slots 3 and 5) unless
// `keepsPrivateData` is false: then SetPrivateData succeeds and GetPrivateData finds nothing, like a
// proxy that drops it.
struct CountingChain {
    void** vtable;
    bool d3d12 = false;
    bool keepsPrivateData = true;
    int getDeviceCalls = 0;
    ULONG deviceRefs = 1;
    void* device[1] = {};
    GUID dataGuid = {};
    UINT dataSize = 0;
    unsigned char data[16] = {};
};
HRESULT STDMETHODCALLTYPE CountingQueryInterface(void*, REFIID, void** out) {
    *out = nullptr;
    return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE CountingAddRef(void*) {
    return 2;
}
ULONG STDMETHODCALLTYPE CountingRelease(void*) {
    return 1;
}
void* g_countingDeviceVTable[3] = {reinterpret_cast<void*>(&CountingQueryInterface),
                                   reinterpret_cast<void*>(&CountingAddRef), reinterpret_cast<void*>(&CountingRelease)};
HRESULT STDMETHODCALLTYPE CountingSetPrivateData(CountingChain* self, REFGUID guid, UINT size, const void* data) {
    if (!self->keepsPrivateData)
        return S_OK;
    if (size > sizeof(self->data))
        return E_INVALIDARG;
    self->dataGuid = guid;
    self->dataSize = size;
    std::memcpy(self->data, data, size);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE CountingGetPrivateData(CountingChain* self, REFGUID guid, UINT* size, void* data) {
    if (!self->keepsPrivateData || self->dataSize == 0 || guid != self->dataGuid)
        return DXGI_ERROR_NOT_FOUND;
    if (*size < self->dataSize)
        return DXGI_ERROR_MORE_DATA;
    std::memcpy(data, self->data, self->dataSize);
    *size = self->dataSize;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE CountingGetDevice(CountingChain* self, REFIID riid, void** out) {
    ++self->getDeviceCalls;
    *out = nullptr;
    if (riid != (self->d3d12 ? __uuidof(ID3D12Device) : __uuidof(ID3D11Device)))
        return E_NOINTERFACE;
    self->device[0] = static_cast<void*>(g_countingDeviceVTable);
    *out = static_cast<void*>(self->device);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE CountingUnused(void*) {
    return E_NOTIMPL;
}
void* g_countingChainVTable[8] = {
    reinterpret_cast<void*>(&CountingQueryInterface), reinterpret_cast<void*>(&CountingAddRef),
    reinterpret_cast<void*>(&CountingRelease),        reinterpret_cast<void*>(&CountingSetPrivateData),
    reinterpret_cast<void*>(&CountingUnused),         reinterpret_cast<void*>(&CountingGetPrivateData),
    reinterpret_cast<void*>(&CountingUnused),         reinterpret_cast<void*>(&CountingGetDevice)};

// The thread's fallback keeps addresses; on every way out of a test (an ASSERT returns early) it must
// not keep pointing into that test's stack frame.
struct ResetClassificationCacheOnExit {
    ~ResetClassificationCacheOnExit() { DXGIShared::ResetSwapChainApiCacheForTesting(); }
};

// The D3D11 overlay classified its swapchain on every Present: four GetDevice calls through the
// runtime, three refused (Mirror's Edge on DXUP, 20261018_151515).
TEST(GraphicsApiClassificationTest, APresentingSwapchainIsClassifiedOnce) {
    CountingChain first{g_countingChainVTable};
    CountingChain second{g_countingChainVTable};
    auto* firstChain = reinterpret_cast<IDXGISwapChain*>(&first);
    auto* secondChain = reinterpret_cast<IDXGISwapChain*>(&second);
    const ResetClassificationCacheOnExit resetOnExit;
    for (int present = 0; present < 100; ++present)
        ASSERT_EQ(DXGIShared::ClassifySwapChainAPICached(firstChain), APIType::D3D11);
    EXPECT_EQ(first.getDeviceCalls, 4) << "one classification, not one per Present";
    // Another swapchain is classified for itself; each keeps its own verdict, so alternating
    // between them asks neither again.
    EXPECT_EQ(DXGIShared::ClassifySwapChainAPICached(secondChain), APIType::D3D11);
    EXPECT_EQ(second.getDeviceCalls, 4);
    EXPECT_EQ(DXGIShared::ClassifySwapChainAPICached(firstChain), APIType::D3D11);
    EXPECT_EQ(first.getDeviceCalls, 4);
    // The uncached decision still asks every time.
    EXPECT_EQ(DXGIShared::ClassifySwapChainAPI(firstChain), APIType::D3D11);
    EXPECT_EQ(first.getDeviceCalls, 8);
}

// The allocator hands a released swapchain's block to the next one. A cache keyed by the address
// alone gave the new chain the dead one's API, so a D3D12 chain took the D3D11 overlay and capture
// paths. The verdict lives on the object now and dies with it.
TEST(GraphicsApiClassificationTest, NewChainAtAReusedAddressIsClassifiedForItself) {
    alignas(CountingChain) unsigned char storage[sizeof(CountingChain)];
    auto* chain = reinterpret_cast<IDXGISwapChain*>(storage);
    const ResetClassificationCacheOnExit resetOnExit;
    auto* d3d11 = new (storage) CountingChain{g_countingChainVTable};
    ASSERT_EQ(DXGIShared::ClassifySwapChainAPICached(chain), APIType::D3D11);
    d3d11->~CountingChain();

    auto* d3d12 = new (storage) CountingChain{g_countingChainVTable, true};
    EXPECT_EQ(DXGIShared::ClassifySwapChainAPICached(chain), APIType::D3D12) << "the dead chain's verdict outlived it";
    EXPECT_EQ(DXGIShared::ClassifySwapChainAPICached(chain), APIType::D3D12);
    EXPECT_EQ(d3d12->getDeviceCalls, 4) << "the new chain is classified once and keeps its own verdict";
    d3d12->~CountingChain();
}

// An object that drops private data (some proxies do) still gets the address cache, so its Presents
// stay cheap; a later object at that address that keeps the tag is not the one cached and is asked.
TEST(GraphicsApiClassificationTest, AChainWithoutPrivateDataIsCachedByAddressOnlyWhileItIsThere) {
    alignas(CountingChain) unsigned char storage[sizeof(CountingChain)];
    auto* chain = reinterpret_cast<IDXGISwapChain*>(storage);
    const ResetClassificationCacheOnExit resetOnExit;
    auto* proxy = new (storage) CountingChain{g_countingChainVTable, false, false};
    for (int present = 0; present < 100; ++present)
        ASSERT_EQ(DXGIShared::ClassifySwapChainAPICached(chain), APIType::D3D11);
    EXPECT_EQ(proxy->getDeviceCalls, 4) << "the address fallback still classifies once";
    proxy->~CountingChain();

    auto* next = new (storage) CountingChain{g_countingChainVTable, true};
    EXPECT_EQ(DXGIShared::ClassifySwapChainAPICached(chain), APIType::D3D12)
        << "an object that keeps the tag is not the one cached by address";
    EXPECT_EQ(next->getDeviceCalls, 4);
    next->~CountingChain();
}

TEST(GraphicsApiClassificationTest, OnlyTheApplicationsOwnQueriesShowAD3D11MinorVersion) {
    // resolved, CE, device runtime, translator frontend, Windows, third-party overlay
    EXPECT_EQ(ClassifyApiEvidenceCaller(true, false, false, false, false, false), ApiEvidenceCaller::kApplication);
    // CE's own prerender fence path asks for ID3D11Device5 / ID3D11DeviceContext4.
    EXPECT_EQ(ClassifyApiEvidenceCaller(true, true, false, false, false, false), ApiEvidenceCaller::kCaptureEngine);
    // Windows' or DXVK's d3d11.dll asking its own device.
    EXPECT_EQ(ClassifyApiEvidenceCaller(true, false, true, false, false, false), ApiEvidenceCaller::kDeviceRuntime);
    // DXUP's d3d9.dll asking its own d3d11 for ID3D11DeviceContext1 (Mirror's Edge, 20261018_143747):
    // the game's DLL directory holds it, yet it is the runtime, not the game.
    EXPECT_EQ(ClassifyApiEvidenceCaller(true, false, false, true, false, false), ApiEvidenceCaller::kDeviceRuntime);
    EXPECT_EQ(ClassifyApiEvidenceCaller(true, false, false, false, true, false), ApiEvidenceCaller::kWindows);
    EXPECT_EQ(ClassifyApiEvidenceCaller(true, false, false, false, false, true), ApiEvidenceCaller::kThirdPartyOverlay);
    EXPECT_EQ(ClassifyApiEvidenceCaller(false, false, false, false, false, false), ApiEvidenceCaller::kUnresolved);
    // CE outranks the rest: its module is never the application.
    EXPECT_EQ(ClassifyApiEvidenceCaller(true, true, true, true, true, true), ApiEvidenceCaller::kCaptureEngine);
}

// Any d3d9.dll outside System32 is a translation runtime CE does not probe; which one it is decides
// the log and the label. DXUP's d3d9.dll was reported as DXVK (Mirror's Edge, 20261018_151515).
TEST(GraphicsApiClassificationTest, ANonSystemD3D9RuntimeIsNamedByItsVersionResource) {
    using ce::api_translator::D3D9RuntimeKind;
    using ce::api_translator::D3D9RuntimeKindName;
    using ce::api_translator::IsNonSystemD3D9Runtime;
    using ce::api_translator::SelectD3D9RuntimeKind;
    // loaded, outside System32, "dxvk" in the version resource, "dxup" in it
    EXPECT_EQ(SelectD3D9RuntimeKind(false, false, false, false), D3D9RuntimeKind::kNotLoaded);
    EXPECT_EQ(SelectD3D9RuntimeKind(true, false, false, false), D3D9RuntimeKind::kSystem);
    EXPECT_EQ(SelectD3D9RuntimeKind(true, true, true, false), D3D9RuntimeKind::kDXVK);
    EXPECT_EQ(SelectD3D9RuntimeKind(true, true, false, true), D3D9RuntimeKind::kDXUP);
    EXPECT_EQ(SelectD3D9RuntimeKind(true, true, false, false), D3D9RuntimeKind::kOtherReplacement);
    // A version resource never makes a System32 DLL a replacement.
    EXPECT_EQ(SelectD3D9RuntimeKind(true, false, true, true), D3D9RuntimeKind::kSystem);

    // Every replacement keeps CE's non-system handling (no synthetic probe, no zero-copy, no pacing).
    for (const auto kind : {D3D9RuntimeKind::kDXVK, D3D9RuntimeKind::kDXUP, D3D9RuntimeKind::kOtherReplacement})
        EXPECT_TRUE(IsNonSystemD3D9Runtime(kind)) << D3D9RuntimeKindName(kind);
    EXPECT_FALSE(IsNonSystemD3D9Runtime(D3D9RuntimeKind::kSystem));
    EXPECT_FALSE(IsNonSystemD3D9Runtime(D3D9RuntimeKind::kNotLoaded));
    EXPECT_STREQ(D3D9RuntimeKindName(D3D9RuntimeKind::kDXUP), "DXUP");
    EXPECT_STREQ(D3D9RuntimeKindName(D3D9RuntimeKind::kDXVK), "DXVK");
}

TEST(GraphicsApiClassificationTest, WindowsModulesAreThoseUnderTheWindowsDirectory) {
    using ce::graphics_api_identity::IsPathUnderDirectory;
    EXPECT_TRUE(IsPathUnderDirectory("C:\\Windows\\System32\\dxgi.dll", "C:\\Windows"));
    EXPECT_TRUE(IsPathUnderDirectory("c:\\windows\\SysWOW64\\d3d11.dll", "C:\\Windows"));
    EXPECT_FALSE(IsPathUnderDirectory("C:\\WindowsApps\\game.exe", "C:\\Windows"));
    EXPECT_FALSE(IsPathUnderDirectory("D:\\Games\\X\\d3d11.dll", "C:\\Windows"));
    EXPECT_FALSE(IsPathUnderDirectory("C:\\Windows\\System32\\dxgi.dll", ""));
}

TEST(GraphicsApiClassificationTest, D3D12LabelNamesTheTranslator) {
    using ce::graphics_api_identity::D3D12Label;
    EXPECT_STREQ(D3D12Label(false, false), "DX12");
    EXPECT_STREQ(D3D12Label(true, false), "DX12 (VKD3D-Proton)");
    EXPECT_STREQ(D3D12Label(false, true), "DX12 (VKD3D)");
    // The same name as the Vulkan layer's label for vkd3d-proton.
    EXPECT_STREQ(DXGIShared::SelectTranslatedGraphicsAPIName(false, false, true, false), D3D12Label(true, false));
}

// DXUP translates DirectDraw (with D3D6/7), D3D8, D3D9 and D3D11 to one native D3D12 swapchain,
// whose overlay said "DX12" until 2026-10-18. Its legacy frontends always load its d3d11/dxgi.
TEST(GraphicsApiClassificationTest, DxupGamesNameTheApplicationsApi) {
    using ce::graphics_api_identity::DirectDrawVersion;
    using ce::graphics_api_identity::DxupFrontend;
    using ce::graphics_api_identity::DxupFrontendLabel;
    using ce::graphics_api_identity::SelectDxupFrontend;
    // ddraw, d3d8, d3d9, d3d11/dxgi loaded; DirectDraw and D3D9 evidence.
    EXPECT_EQ(SelectDxupFrontend(false, false, false, true, false, false), DxupFrontend::kD3D11);
    EXPECT_EQ(SelectDxupFrontend(false, false, true, true, false, true), DxupFrontend::kD3D9);
    EXPECT_EQ(SelectDxupFrontend(false, false, true, true, false, false), DxupFrontend::kD3D9);
    EXPECT_EQ(SelectDxupFrontend(false, true, false, true, false, false), DxupFrontend::kD3D8);
    EXPECT_EQ(SelectDxupFrontend(true, false, false, true, true, false), DxupFrontend::kDirectDraw);
    // A D3D9 game that maps DXUP's DirectDraw for its movies is still D3D9.
    EXPECT_EQ(SelectDxupFrontend(true, false, true, true, false, true), DxupFrontend::kD3D9);
    EXPECT_EQ(SelectDxupFrontend(false, false, false, false, false, false), DxupFrontend::kNone);

    EXPECT_EQ(DxupFrontendLabel(DxupFrontend::kD3D11, DirectDrawVersion::Unknown, 0, false, 0), "DX11 (DXUP)");
    EXPECT_EQ(DxupFrontendLabel(DxupFrontend::kD3D11, DirectDrawVersion::Unknown, 0, false, 1), "DX11.1 (DXUP)");
    EXPECT_EQ(DxupFrontendLabel(DxupFrontend::kD3D9, DirectDrawVersion::Unknown, 0, false, 1), "DX9 (DXUP)");
    EXPECT_EQ(DxupFrontendLabel(DxupFrontend::kD3D9, DirectDrawVersion::Unknown, 0, true, 0), "DX9Ex (DXUP)");
    EXPECT_EQ(DxupFrontendLabel(DxupFrontend::kD3D8, DirectDrawVersion::Unknown, 0, false, 0), "DX8 (DXUP)");
    EXPECT_EQ(DxupFrontendLabel(DxupFrontend::kDirectDraw, DirectDrawVersion::DirectDraw7, 7, false, 0), "DX7 (DXUP)");
    EXPECT_EQ(DxupFrontendLabel(DxupFrontend::kDirectDraw, DirectDrawVersion::DirectDraw4, 0, false, 0),
              "DirectDraw4 (DXUP)");
    EXPECT_EQ(DxupFrontendLabel(DxupFrontend::kNone, DirectDrawVersion::Unknown, 0, false, 0), "");
    EXPECT_EQ(ce::graphics_api_identity::WithDxup("DX9Ex"), "DX9Ex (DXUP)");
    // The overlay's label buffer holds the longest one.
    EXPECT_LT(DxupFrontendLabel(DxupFrontend::kDirectDraw, DirectDrawVersion::DirectDraw7, 0, false, 0).size(), 32u);
}

TEST(GraphicsApiClassificationSourceTest, EveryLabelAndClassifierTakesTheSharedDecision) {
    namespace fs = std::filesystem;
    const fs::path root = fs::current_path() / "hook";
    const auto read = [&](const fs::path& relative) { return ce::test_source::ReadFile(root / relative); };
    // Both swapchain classifiers.
    EXPECT_NE(read("d3d11/dx11_hook_helpers.cpp").find("DXGIShared::ClassifySwapChainAPICached(swapChain)"),
              std::string::npos);
    EXPECT_NE(read("present/dxgi_shared.cpp").find("ClassifySwapChainAPICached(pSwapChain)"), std::string::npos);
    // No D3D12 overlay path names its API by itself.
    for (const char* file : {"d3d12/dx12_hook_ffx.cpp", "d3d12/dx12_hook_ffx_ui_composite.cpp",
                             "d3d12/dx12_streamline_ui_overlay.cpp", "d3d12/dx12_hook_overlay_render.cpp",
                             "d3d12/dx12_hook_postsl_render_route.cpp",
                             "d3d12/dx12_hook_process_session_draw_record.cpp"}) {
        SCOPED_TRACE(file);
        const std::string source = read(file);
        ASSERT_FALSE(source.empty());
        EXPECT_NE(source.find("Dx12OverlayApiLabel()"), std::string::npos);
        EXPECT_EQ(source.find("SetGraphicsAPI(\"DX12\")"), std::string::npos);
        EXPECT_EQ(source.find("const char* api = \"DX12\""), std::string::npos);
        EXPECT_EQ(source.find("s_isVKD3D"), std::string::npos);
    }
    // The D3D12 label asks for a DXUP frontend first; every path CE draws on a DXUP device names it.
    const std::string dx12Label = read("d3d12/dx12_api_label.h");
    EXPECT_LT(dx12Label.find("DxupOverlayApiLabel("), dx12Label.find("D3D12Label("));
    EXPECT_NE(read("runtime/dxup_api_label.h").find("SelectDxupFrontend("), std::string::npos);
    // DXUP's D3D11 output carries its D3D9/D3D8/DirectDraw games too: the D3D11 overlay names the
    // frontend, not "DX11.x (DXUP)" (Mirror's Edge, 20261018_143747).
    const std::string dx11Overlay = read("d3d11/dx11_hook_overlay.cpp");
    EXPECT_NE(dx11Overlay.find("DxupOverlayApiLabel(d3d11Minor)"), std::string::npos);
    EXPECT_EQ(dx11Overlay.find("WithDxup(ce::graphics_api_identity::D3D11Label"), std::string::npos);
    EXPECT_NE(read("d3d11/dx11_hook.cpp").find("IsDxupModuleHandle(callerModule)"), std::string::npos);
    // Only the label asks whether d3d9.dll is DXVK; every other caller means "not System32's".
    for (const char* file : {"runtime/main_install.cpp", "d3d9/dx9_hook_pacing.cpp", "d3d9/dx9_hook_present.cpp",
                             "d3d9/dx9_hook_capture_init.cpp", "d3d9/dx9_hook_capture_direct_ring.cpp",
                             "d3d9/dx9_hook_overlay.cpp", "ddraw/ddraw_hook_helpers.cpp", "d3d9/dx9_hook.h"}) {
        SCOPED_TRACE(file);
        const std::string source = read(file);
        ASSERT_FALSE(source.empty());
        EXPECT_EQ(source.find("IsDXVKD3D9WrapperLoaded"), std::string::npos);
    }
    EXPECT_NE(read("d3d9/dx9_hook_overlay.cpp").find("D3D9RuntimeKind::kDXVK"), std::string::npos);
    EXPECT_NE(read("runtime/main_install.cpp").find("d3d9Runtime=%s"), std::string::npos);
    // The label and the DXVK check read version resources: kept until the module set changes.
    EXPECT_NE(dx11Overlay.find("modules == t_modules"), std::string::npos);
    EXPECT_NE(read("d3d11/dx11_hook_helpers.cpp").find("CachedForModuleSet(LoadedModuleSetGeneration()"),
              std::string::npos);
    for (const char* file : {"d3d9/dx9_hook_overlay.cpp", "d3d8/dx8_hook_helpers.cpp", "ddraw/ddraw_hook_helpers.cpp",
                             "d3d11/dx11_hook_overlay.cpp"}) {
        SCOPED_TRACE(file);
        EXPECT_NE(read(file).find("ce::api_translator::IsDxupModuleLoaded(\""), std::string::npos);
    }
    // Every D3D11 minor-version report carries its caller.
    for (const char* file : {"d3d11/dx11_hook_helpers.cpp", "wrappers/d3d11_device_wrap.cpp",
                             "wrappers/d3d11_devicecontext_forward.cpp"}) {
        SCOPED_TRACE(file);
        const std::string source = read(file);
        EXPECT_NE(source.find("CE_API_EVIDENCE_CALLER()"), std::string::npos);
        EXPECT_EQ(source.find(" QI\");"), std::string::npos) << "a report without its caller";
    }
}

}  // namespace
