#include "tests/flow/flow_test_support.h"
#include "common/platform/raii_helpers.h"
#include <filesystem>

namespace {
using namespace ce::flow;

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
}  // namespace
