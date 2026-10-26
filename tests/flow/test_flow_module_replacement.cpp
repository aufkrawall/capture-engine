#include "tests/flow/flow_test_support.h"
#include "common/platform/raii_helpers.h"
#include "hook/hooking/module_export_resolver.h"

#include <dxgi1_3.h>

#include <filesystem>
#include <string>

namespace {
using namespace ce::flow;

std::wstring SystemDxgiPath() {
    wchar_t directory[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(directory, MAX_PATH);
    return length != 0 && length < MAX_PATH ? std::wstring(directory) + L"\\dxgi.dll" : std::wstring();
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

}  // namespace
