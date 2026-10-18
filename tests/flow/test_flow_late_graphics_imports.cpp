#include "tests/flow/flow_test_support.h"
#include "common/platform/raii_helpers.h"
#include <d3d10_1.h>
#include <filesystem>

namespace {
using namespace ce::flow;

const wchar_t* g_lateModulePath = nullptr;
HMODULE g_lateModule = nullptr;

void LoadModuleAfterUESnapshot() {
    g_lateModule = LoadLibraryW(g_lateModulePath);
}

TEST(FlowLateGraphicsImports, ExistingRuntimeLoadsStayLocalAndNewImportersResolveWrappedExports) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    const auto hook = GetModuleHandleW(L"capture_hook_x64.dll");
    const auto notify = reinterpret_cast<void (*)(HMODULE, const char*)>(
        GetProcAddress(hook, "CEFlow_NotifyModuleLoad"));
    const auto sweeps = reinterpret_cast<uint64_t (*)()>(GetProcAddress(hook, "CEFlow_ImportSweeps"));
    const auto wrapper = reinterpret_cast<void* (*)()>(GetProcAddress(hook, "CEFlow_D3D11CreateWrapper"));
    ASSERT_NE(notify, nullptr);
    ASSERT_NE(sweeps, nullptr);
    ASSERT_NE(wrapper, nullptr);
    const auto original = reinterpret_cast<void* (*)()>(GetProcAddress(hook, "CEFlow_D3D11CreateOriginal"));
    ASSERT_NE(original, nullptr);
    const auto runtime = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    ASSERT_NE(runtime, nullptr);
    const auto releaseRuntime = ce::make_scope_guard([&] { FreeLibrary(runtime); });
    notify(runtime, "d3d11.dll");
    const auto before = sweeps();
    for (unsigned repeat = 0; repeat < 32; ++repeat) notify(runtime, "d3d11.dll");
    EXPECT_EQ(sweeps(), before);

    // A newly mapped, eligible caller must receive the dynamic wrapper on its
    // first resolution without a process-wide GetProcAddress import sweep.
    const auto path = std::filesystem::path(game.LogDirectory()) / "ce_late_import_probe.dll";
    ASSERT_FALSE(std::filesystem::exists(path));
    ASSERT_TRUE(std::filesystem::copy_file("sl.reflex.dll", path));
    const auto removeProbe = ce::make_scope_guard([&] { std::filesystem::remove(path); });
    const auto probe = LoadLibraryW(path.c_str());
    ASSERT_NE(probe, nullptr);
    const auto unloadProbe = ce::make_scope_guard([&] { FreeLibrary(probe); });
    const auto resolve = reinterpret_cast<void* (*)(HMODULE)>(GetProcAddress(probe, "CEFake_ResolveD3D11"));
    ASSERT_NE(resolve, nullptr);
    EXPECT_EQ(resolve(runtime), wrapper());
    EXPECT_EQ(sweeps(), before);
    const auto exchangeResolver = reinterpret_cast<void* (*)(void*)>(
        GetProcAddress(probe, "CEFake_ExchangeResolver"));
    const auto patchLoaded = reinterpret_cast<void (*)(HMODULE)>(
        GetProcAddress(hook, "CEFlow_PatchLoadedGraphicsImports"));
    ASSERT_NE(exchangeResolver, nullptr);
    ASSERT_NE(patchLoaded, nullptr);
    void* unresolved = reinterpret_cast<void*>(uintptr_t{1});
    void* savedResolver = exchangeResolver(unresolved);
    ASSERT_NE(savedResolver, nullptr);
    // Simulate the loader's pre-binding slot, then restore it before assertions.
    patchLoaded(probe);
    EXPECT_EQ(exchangeResolver(savedResolver), unresolved);
    EXPECT_EQ(resolve(runtime), wrapper());

    // Overlay and API-proxy consumers must keep their original exports.
    const auto overlayPath = std::filesystem::path(game.LogDirectory()) / "RTSSHooks64.dll";
    ASSERT_FALSE(std::filesystem::exists(overlayPath));
    ASSERT_TRUE(std::filesystem::copy_file("sl.reflex.dll", overlayPath));
    const auto removeOverlay = ce::make_scope_guard([&] { std::filesystem::remove(overlayPath); });
    const auto overlay = LoadLibraryW(overlayPath.c_str());
    ASSERT_NE(overlay, nullptr);
    const auto unloadOverlay = ce::make_scope_guard([&] { FreeLibrary(overlay); });
    const auto resolveOverlay = reinterpret_cast<void* (*)(HMODULE)>(
        GetProcAddress(overlay, "CEFake_ResolveD3D11"));
    ASSERT_NE(resolveOverlay, nullptr);
    EXPECT_EQ(resolveOverlay(runtime), original());
    EXPECT_NE(resolveOverlay(runtime), wrapper());
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowLateGraphicsImports, LoaderReturnsDoNotWakeOrRequeueDiscoveryButImageChangesAndFallbackDo) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    const auto hook = GetModuleHandleW(L"capture_hook_x64.dll");
    const auto notify = reinterpret_cast<void (*)(HMODULE, const char*)>(
        GetProcAddress(hook, "CEFlow_NotifyModuleLoad"));
    const auto setEvent = reinterpret_cast<HANDLE (*)(HANDLE)>(
        GetProcAddress(hook, "CEFlow_SetHookWakeEvent"));
    const auto discover = reinterpret_cast<void (*)(bool)>(
        GetProcAddress(hook, "CEFlow_SetUE5ModuleDiscovery"));
    const auto pending = reinterpret_cast<unsigned (*)()>(
        GetProcAddress(hook, "CEFlow_PendingUE5Modules"));
    const auto returns = reinterpret_cast<uint64_t (*)(bool)>(
        GetProcAddress(hook, "CEFlow_ModuleLoadReturns"));
    const auto setNative = reinterpret_cast<bool (*)(bool)>(
        GetProcAddress(hook, "CEFlow_SetDllNotificationsActive"));
    const auto scan = reinterpret_cast<void (*)(void (*)())>(
        GetProcAddress(hook, "CEFlow_ScanUE5Modules"));
    ASSERT_NE(notify, nullptr);
    ASSERT_NE(setEvent, nullptr);
    ASSERT_NE(discover, nullptr);
    ASSERT_NE(pending, nullptr);
    ASSERT_NE(returns, nullptr);
    ASSERT_NE(setNative, nullptr);
    ASSERT_NE(scan, nullptr);
    ASSERT_TRUE(setNative(true)) << "the real loader notification must be registered";
    const HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ASSERT_NE(wake, nullptr);
    const auto closeWake = ce::make_scope_guard([&] { CloseHandle(wake); });
    const HANDLE previousEvent = setEvent(wake);
    const auto restore = ce::make_scope_guard([&] {
        setNative(true);
        setEvent(previousEvent);
        discover(false);
    });
    discover(true);
    const auto runtime = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    ASSERT_NE(runtime, nullptr);
    const auto releaseRuntime = ce::make_scope_guard([&] { FreeLibrary(runtime); });
    // Prime immediate hook setup before measuring repeated references.
    notify(runtime, "d3d11.dll");
    discover(true);
    ASSERT_TRUE(ResetEvent(wake));
    const uint64_t coalescedBefore = returns(false);
    const uint64_t fallbackBefore = returns(true);
    for (unsigned repeat = 0; repeat < 32; ++repeat) {
        const auto loaded = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        ASSERT_EQ(loaded, runtime);
        notify(loaded, "d3d11.dll");
        ASSERT_TRUE(FreeLibrary(loaded));
    }
    EXPECT_EQ(WaitForSingleObject(wake, 0), WAIT_TIMEOUT);
    EXPECT_EQ(pending(), 0u);
    EXPECT_GE(returns(false), coalescedBefore + 32);
    EXPECT_EQ(returns(true), fallbackBefore);

    // A new image, its unload, and a reload must each retain native service
    // wakeups. This also covers Windows reusing the previous image's address.
    const auto path = std::filesystem::path(game.LogDirectory()) / "ce_service_load_probe.dll";
    ASSERT_TRUE(std::filesystem::copy_file("sl.reflex.dll", path));
    const auto removeProbe = ce::make_scope_guard([&] { std::filesystem::remove(path); });
    for (unsigned generation = 0; generation < 2; ++generation) {
        const auto probe = LoadLibraryW(path.c_str());
        ASSERT_NE(probe, nullptr);
        const auto unloadProbe = ce::make_scope_guard([&] { FreeLibrary(probe); });
        EXPECT_EQ(WaitForSingleObject(wake, 0), WAIT_OBJECT_0);
        EXPECT_GT(pending(), 0u);
        discover(true);
        ASSERT_TRUE(ResetEvent(wake));
    }
    EXPECT_EQ(WaitForSingleObject(wake, 0), WAIT_OBJECT_0) << "unload must wake service too";

    // Load a real new image after the full scan took its snapshot. Its native
    // notification must survive the scan for the next pending-module pass.
    discover(true);
    g_lateModulePath = path.c_str();
    scan(&LoadModuleAfterUESnapshot);
    const auto unloadLateModule = ce::make_scope_guard([&] {
        if (g_lateModule)
            FreeLibrary(g_lateModule);
        g_lateModule = nullptr;
        g_lateModulePath = nullptr;
    });
    ASSERT_NE(g_lateModule, nullptr);
    EXPECT_GT(pending(), 0u) << "the snapshot must not erase a later image's discovery request";

    // Failed/unavailable native registration retains the loader-return path.
    ASSERT_TRUE(setNative(false));
    discover(true);
    ASSERT_TRUE(ResetEvent(wake));
    const uint64_t fallbackAtStart = returns(true);
    notify(runtime, "d3d11.dll");
    EXPECT_GT(returns(true), fallbackAtStart);
    EXPECT_GT(pending(), 0u);
    EXPECT_EQ(WaitForSingleObject(wake, 0), WAIT_OBJECT_0);
    setNative(true);
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

// d3d10_1.dll creates its devices through d3d11.dll's D3D11CreateDeviceAndSwapChain, and d3d11.dll
// maps as its dependency without a load CE observes. The loaded-module pass redirected that
// import to CE's wrapper before anything resolved the wrapper's original, so every D3D10.1 device
// creation failed with E_FAIL ("oD3D11CreateDeviceAndSwapChain is NULL", 2026-10-18).
TEST(FlowLateGraphicsImports, D3D10_1LoadedAfterInitCreatesItsDevice) {
    FlowGame game(CurrentTestName());
    EXPECT_EQ(GetModuleHandleW(L"d3d11.dll"), nullptr) << "the scenario needs d3d11.dll to map with d3d10_1.dll";
    const HMODULE runtime = LoadLibraryExW(L"d3d10_1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    ASSERT_NE(runtime, nullptr);
    const auto create =
        reinterpret_cast<decltype(&D3D10CreateDevice1)>(GetProcAddress(runtime, "D3D10CreateDevice1"));
    ASSERT_NE(create, nullptr);
    ID3D10Device1* device = nullptr;
    const HRESULT hr = create(nullptr, D3D10_DRIVER_TYPE_WARP, nullptr, 0, D3D10_FEATURE_LEVEL_10_0,
                              D3D10_1_SDK_VERSION, &device);
    EXPECT_TRUE(SUCCEEDED(hr) && device) << "hr=0x" << std::hex << static_cast<uint32_t>(hr);
    if (device) {
        device->Release();
    }
}
}  // namespace
