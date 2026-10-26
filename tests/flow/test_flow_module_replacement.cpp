#include "tests/flow/flow_test_support.h"
#include "common/platform/raii_helpers.h"
#include "hook/hooking/module_export_resolver.h"
#include "hook/runtime/com_object_module.h"

#include <d3d11.h>
#include <dxgi1_3.h>

#include <filesystem>
#include <string>

namespace {
using namespace ce::flow;

std::wstring SystemPath(const wchar_t* name) {
    wchar_t directory[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(directory, MAX_PATH);
    return length != 0 && length < MAX_PATH ? std::wstring(directory) + L"\\" + name : std::wstring();
}

std::wstring SystemDxgiPath() {
    return SystemPath(L"dxgi.dll");
}

template <typename Function>
Function Export(HMODULE module, const char* name) {
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

// GTA V Enhanced maps System32's dxgi.dll and d3d12.dll before CE loads, and a [ThirdParty] custom_dll_paths
// copy of the same name used to become a second, unused image: the game ran on native D3D12 instead of
// vkd3d-proton (session 20261026_145243). This executable imports System32's dxgi.dll statically too.
TEST(FlowModuleReplacement, CustomDllNamedLikeAResidentRuntimeServesTheApplication) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    const HMODULE systemDxgi = GetModuleHandleW(SystemDxgiPath().c_str());
    ASSERT_NE(systemDxgi, nullptr);
    // Protected imports and manually walked export tables bypass IAT/GetProcAddress redirection.
    // Keep the actual native entry address before adoption and call that same address afterwards.
    const auto cachedFactory = reinterpret_cast<decltype(&CreateDXGIFactory2)>(
        ce::module_export::ResolveAddressDirect(systemDxgi, "CreateDXGIFactory2"));
    ASSERT_NE(cachedFactory, nullptr);
    HMODULE cachedOwner = nullptr;
    ASSERT_TRUE(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(cachedFactory), &cachedOwner));
    ASSERT_EQ(cachedOwner, systemDxgi);
    const auto preload = Export<void (*)(const char* const*, size_t)>(GetModuleHandleW(L"capture_hook_x64.dll"),
                                                                       "CEFlow_PreloadCustomDlls");
    ASSERT_NE(preload, nullptr);
    const std::string proxyPath = std::filesystem::absolute("replacement\\dxgi.dll").string();
    const char* paths[] = {proxyPath.c_str()};
    preload(paths, 1);

    const std::wstring proxyPathW = std::filesystem::absolute("replacement\\dxgi.dll").wstring();
    HMODULE proxy = nullptr;
    ASSERT_TRUE(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, proxyPathW.c_str(), &proxy));
    ASSERT_NE(proxy, systemDxgi);
    const auto calls = Export<long (*)()>(proxy, "CEFake_ReplacementCalls");
    const auto wrapped = Export<HMODULE (*)()>(proxy, "CEFake_WrappedImage");
    ASSERT_NE(calls, nullptr);
    ASSERT_NE(wrapped, nullptr);

    // This executable's own static import now reaches the replacement (directly, or through CE's
    // factory wrapper, whose original moved with it).
    long before = calls();
    IDXGIFactory2* factory = nullptr;
    ASSERT_TRUE(SUCCEEDED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory))));
    factory->Release();
    EXPECT_EQ(calls(), before + 1);

    before = calls();
    factory = nullptr;
    ASSERT_TRUE(SUCCEEDED(cachedFactory(0, __uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory))));
    ASSERT_NE(factory, nullptr);
    factory->Release();
    EXPECT_EQ(calls(), before + 1) << "an address cached before adoption must reach the replacement too";

    const auto nativeFactory = Export<HRESULT (*)(void*)>(GetModuleHandleW(L"capture_hook_x64.dll"),
                                                           "CEFlow_CallFactoryFromHook");
    ASSERT_NE(nativeFactory, nullptr);
    before = calls();
    EXPECT_TRUE(SUCCEEDED(nativeFactory(reinterpret_cast<void*>(cachedFactory))));
    EXPECT_EQ(calls(), before) << "CE's own explicit native runtime calls must stay native";

    // A module mapped afterwards binds its dxgi.dll import to the first image of that name; CE moves it.
    const HMODULE importer = LoadLibraryW(L"replacement_importer.dll");
    ASSERT_NE(importer, nullptr);
    const auto unloadImporter = ce::make_scope_guard([&] { FreeLibrary(importer); });
    const auto createThroughImport = Export<HRESULT (*)()>(importer, "CEFake_CreateFactoryThroughImport");
    const auto getModuleW = Export<HMODULE (*)(const wchar_t*)>(importer, "CEFake_GetModuleHandleW");
    const auto getModuleA = Export<HMODULE (*)(const char*)>(importer, "CEFake_GetModuleHandleA");
    const auto loadLibrary = Export<HMODULE (*)(const wchar_t*)>(importer, "CEFake_LoadLibraryW");
    const auto getProc = Export<void* (*)(HMODULE, const char*)>(importer, "CEFake_GetProcAddress");
    ASSERT_NE(createThroughImport, nullptr);
    ASSERT_NE(getModuleW, nullptr);
    ASSERT_NE(getModuleA, nullptr);
    ASSERT_NE(loadLibrary, nullptr);
    ASSERT_NE(getProc, nullptr);
    before = calls();
    EXPECT_TRUE(SUCCEEDED(createThroughImport()));
    EXPECT_EQ(calls(), before + 1);
    // Already when its DllMain ran: the rebind rides the loader notification, not the returned load.
    const auto importAtAttach = Export<void* (*)()>(importer, "CEFake_FactoryImportAtAttach");
    ASSERT_NE(importAtAttach, nullptr);
    HMODULE attachOwner = nullptr;
    EXPECT_TRUE(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCWSTR>(importAtAttach()), &attachOwner));
    EXPECT_EQ(attachOwner, proxy);

    // Application code that asks by name gets the replacement...
    EXPECT_EQ(getModuleW(L"dxgi.dll"), proxy);
    EXPECT_EQ(getModuleA("DXGI"), proxy);
    const HMODULE byName = loadLibrary(L"dxgi.dll");
    EXPECT_EQ(byName, proxy);
    if (byName)
        FreeLibrary(byName);
    // ...and so does a request for the superseded image's own path or handle: Streamline's interposer
    // loads "<system directory>\dxgi.dll" itself.
    EXPECT_EQ(getModuleW(SystemDxgiPath().c_str()), proxy);
    const HMODULE byPath = loadLibrary(SystemDxgiPath().c_str());
    EXPECT_EQ(byPath, proxy);
    if (byPath)
        FreeLibrary(byPath);
    EXPECT_EQ(getProc(systemDxgi, "CEFake_ReplacementCalls"), reinterpret_cast<void*>(calls));

    // The proxy itself reached System32's image through that same path: a configured custom DLL keeps
    // the image it wraps, or every export it forwards would call itself.
    EXPECT_EQ(wrapped(), systemDxgi);

    // The Vulkan layer explicitly asks System32 for its native capture interop factory. Its
    // GetProcAddress import is hooked too; returning an application wrapper would substitute
    // DXVK's factory and hand a translation adapter to native D3D11 (GTA session 20261026_165613).
    const auto layerPath = std::filesystem::absolute("native_interop/VK_LAYER_CE_overlay.dll");
    std::filesystem::create_directories(layerPath.parent_path());
    std::filesystem::copy_file("replacement_importer.dll", layerPath, std::filesystem::copy_options::overwrite_existing);
    const HMODULE nativeLayer = LoadLibraryW(layerPath.c_str());
    ASSERT_NE(nativeLayer, nullptr);
    const auto unloadLayer = ce::make_scope_guard([&] { FreeLibrary(nativeLayer); });
    const auto nativeGetProc = Export<void* (*)(HMODULE, const char*)>(nativeLayer, "CEFake_GetProcAddress");
    const auto nativeCreate = Export<HRESULT (*)(HMODULE)>(nativeLayer, "CEFake_CreateFactoryThroughProc");
    ASSERT_NE(nativeGetProc, nullptr);
    ASSERT_NE(nativeCreate, nullptr);
    EXPECT_EQ(nativeGetProc(systemDxgi, "CreateDXGIFactory2"), reinterpret_cast<void*>(cachedFactory));
    before = calls();
    EXPECT_TRUE(SUCCEEDED(nativeCreate(systemDxgi)));
    EXPECT_EQ(calls(), before) << "the Vulkan layer's explicit native factory must stay native";

    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ExpectNoDebugLayerErrors();

    const auto shutdown = Export<void (*)()>(GetModuleHandleW(L"capture_hook_x64.dll"), "CEFlow_Shutdown");
    ASSERT_NE(shutdown, nullptr);
    shutdown();
    before = calls();
    factory = nullptr;
    ASSERT_TRUE(SUCCEEDED(cachedFactory(0, __uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory))));
    ASSERT_NE(factory, nullptr);
    factory->Release();
    EXPECT_EQ(calls(), before + 1) << "host shutdown cannot undo the application's adopted runtime";
}

TEST(FlowModuleReplacement, AdoptsAConfiguredRuntimeThatWasAlreadyLoaded) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    const HMODULE systemDxgi = GetModuleHandleW(SystemDxgiPath().c_str());
    ASSERT_NE(systemDxgi, nullptr);
    const auto proxyPath = std::filesystem::absolute("replacement/dxgi.dll");
    const HMODULE proxy = LoadLibraryW(proxyPath.c_str());
    ASSERT_NE(proxy, nullptr);
    const auto unloadProxy = ce::make_scope_guard([&] { FreeLibrary(proxy); });
    ASSERT_NE(proxy, systemDxgi);
    const auto calls = Export<long (*)()>(proxy, "CEFake_ReplacementCalls");
    ASSERT_NE(calls, nullptr);
    const auto preload = Export<void (*)(const char* const*, size_t)>(GetModuleHandleW(L"capture_hook_x64.dll"),
                                                                      "CEFlow_PreloadCustomDlls");
    ASSERT_NE(preload, nullptr);
    const std::string path = proxyPath.string();
    const char* paths[] = {path.c_str()};
    preload(paths, 1);
    const long before = calls();
    IDXGIFactory2* factory = nullptr;
    ASSERT_TRUE(SUCCEEDED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory))));
    ASSERT_NE(factory, nullptr);
    factory->Release();
    EXPECT_EQ(calls(), before + 1);
    const HMODULE importer = LoadLibraryW(L"replacement_importer.dll");
    ASSERT_NE(importer, nullptr);
    const auto unloadImporter = ce::make_scope_guard([&] { FreeLibrary(importer); });
    const auto getModule = Export<HMODULE (*)(const wchar_t*)>(importer, "CEFake_GetModuleHandleW");
    ASSERT_NE(getModule, nullptr);
    EXPECT_EQ(getModule(L"dxgi.dll"), proxy);
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ExpectNoDebugLayerErrors();
}

// Application code under a replaced dxgi.dll enumerates the replacement's adapters. GTA V Enhanced's Epic
// Online Services overlay passed DXVK's to System32's D3D11CreateDevice while initializing its Vulkan overlay,
// which faulted in d3d11!CCreateDeviceCache::CAdapterCache::GetWddmVersion (session 20261026_175321). The
// native runtime must receive System32's adapter with the same LUID instead.
TEST(FlowModuleReplacement, NativeD3D11ReceivesTheSystemAdapterForAReplacementAdapter) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    const HMODULE hook = GetModuleHandleW(L"capture_hook_x64.dll");
    const auto preload = Export<void (*)(const char* const*, size_t)>(hook, "CEFlow_PreloadCustomDlls");
    const auto service = Export<void (*)()>(hook, "CEFlow_ServiceModuleReplacement");
    ASSERT_NE(preload, nullptr);
    ASSERT_NE(service, nullptr);
    // Before adoption: afterwards this executable's own query for that path answers the replacement.
    const HMODULE systemDxgi = GetModuleHandleW(SystemDxgiPath().c_str());
    ASSERT_NE(systemDxgi, nullptr);
    const std::string proxyPath = std::filesystem::absolute("replacement\\dxgi.dll").string();
    const char* paths[] = {proxyPath.c_str()};
    preload(paths, 1);
    HMODULE proxy = nullptr;
    ASSERT_TRUE(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   std::filesystem::absolute("replacement\\dxgi.dll").wstring().c_str(), &proxy));
    ASSERT_NE(proxy, systemDxgi);
    const auto createTranslated = Export<HRESULT (*)(IDXGIAdapter**)>(proxy, "CEFake_CreateTranslatedAdapter");
    ASSERT_NE(createTranslated, nullptr);

    // Loaded (or already resident) after adoption; the hook thread's service pass guards it.
    const HMODULE d3d11 = LoadLibraryW(SystemPath(L"d3d11.dll").c_str());
    ASSERT_NE(d3d11, nullptr);
    const auto unloadD3D11 = ce::make_scope_guard([&] { FreeLibrary(d3d11); });
    service();
    // The code entry itself: what a cached address or another overlay's GetProcAddress detour reaches.
    const auto create =
        reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(ce::module_export::ResolveAddressDirect(d3d11, "D3D11CreateDevice"));
    ASSERT_NE(create, nullptr);

    IDXGIAdapter* translated = nullptr;
    ASSERT_TRUE(SUCCEEDED(createTranslated(&translated)));
    const auto releaseTranslated = ce::make_scope_guard([&] { translated->Release(); });
    ASSERT_EQ(ce::com_object_module::ImplementingModule(translated), proxy) << "the fake must not be System32's";
    DXGI_ADAPTER_DESC translatedDesc = {};
    ASSERT_TRUE(SUCCEEDED(translated->GetDesc(&translatedDesc)));

    ID3D11Device* device = nullptr;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    const HRESULT hr = create(translated, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 1, D3D11_SDK_VERSION, &device,
                              nullptr, nullptr);
    ASSERT_TRUE(SUCCEEDED(hr)) << "D3D11CreateDevice hr=0x" << std::hex << static_cast<unsigned long>(hr);
    ASSERT_NE(device, nullptr);
    const auto releaseDevice = ce::make_scope_guard([&] { device->Release(); });

    IDXGIDevice* dxgiDevice = nullptr;
    ASSERT_TRUE(SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))));
    IDXGIAdapter* used = nullptr;
    const HRESULT adapterHr = dxgiDevice->GetAdapter(&used);
    dxgiDevice->Release();
    ASSERT_TRUE(SUCCEEDED(adapterHr));
    const auto releaseUsed = ce::make_scope_guard([&] { used->Release(); });
    EXPECT_TRUE(ce::com_object_module::IsImplementedBy(used, systemDxgi)) << "the device must sit on System32's adapter";
    DXGI_ADAPTER_DESC usedDesc = {};
    ASSERT_TRUE(SUCCEEDED(used->GetDesc(&usedDesc)));
    EXPECT_EQ(usedDesc.AdapterLuid.LowPart, translatedDesc.AdapterLuid.LowPart);
    EXPECT_EQ(usedDesc.AdapterLuid.HighPart, translatedDesc.AdapterLuid.HighPart);

    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ExpectNoDebugLayerErrors();
}

}  // namespace
