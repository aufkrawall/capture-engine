#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>

#include "hook/hooking/module_export_resolver.h"
#include "hook/hooking/unhooked_module_export.h"

namespace {

using ce::module_export::CachedUnhookedExport;

TEST(UnhookedModuleExportTest, ResolvesDirectAndForwardedSystemExportsToCode) {
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    ASSERT_NE(kernel32, nullptr);
    ASSERT_NE(ntdll, nullptr);
    // HeapAlloc is a PE forwarder to NTDLL.RtlAllocateHeap, not a code RVA.
    ASSERT_EQ(ce::module_export::ResolveAddressDirect(kernel32, "HeapAlloc"), nullptr);
    CachedUnhookedExport forwarded("HeapAlloc");
    void* expected = reinterpret_cast<void*>(GetProcAddress(kernel32, "HeapAlloc"));
    ASSERT_NE(expected, nullptr);
    ASSERT_EQ(forwarded.Get(kernel32), expected);
    const auto allocate = reinterpret_cast<decltype(&HeapAlloc)>(expected);
    void* memory = allocate(GetProcessHeap(), 0, 16);
    ASSERT_NE(memory, nullptr);
    EXPECT_TRUE(HeapFree(GetProcessHeap(), 0, memory));
    CachedUnhookedExport direct("RtlAllocateHeap");
    EXPECT_EQ(direct.Get(ntdll), reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlAllocateHeap")));
}

TEST(UnhookedModuleExportTest, MissingModulesAndExportsAreRefused) {
    const HMODULE module = GetModuleHandleW(L"kernel32.dll");
    EXPECT_EQ(ce::module_export::ResolveUnhookedExport(nullptr, "HeapAlloc"), nullptr);
    EXPECT_EQ(ce::module_export::ResolveUnhookedExport(module, nullptr), nullptr);
    EXPECT_EQ(ce::module_export::ResolveUnhookedExport(module, ""), nullptr);
    CachedUnhookedExport missing("CE_Test_Export_That_Does_Not_Exist");
    EXPECT_EQ(missing.Get(module), nullptr);
    EXPECT_EQ(missing.Get(nullptr), nullptr);
}

TEST(UnhookedModuleExportTest, OutOfOrderResolutionKeepsTheModuleAndFunctionTogether) {
    const auto native = reinterpret_cast<HMODULE>(uintptr_t{1});
    const auto replacement = reinterpret_cast<HMODULE>(uintptr_t{2});
    int nativeCode = 0;
    int replacementCode = 0;
    std::atomic<unsigned> nativeCalls{0};
    std::atomic<unsigned> replacementCalls{0};
    std::promise<void> nativeEntered;
    auto entered = nativeEntered.get_future();
    std::promise<void> releaseNative;
    auto released = releaseNative.get_future().share();
    CachedUnhookedExport cache("factory", [&](HMODULE module, const char*) -> void* {
        if (module == native) {
            ++nativeCalls;
            nativeEntered.set_value();
            released.wait();
            return &nativeCode;
        }
        ++replacementCalls;
        return &replacementCode;
    });
    auto nativeResult = std::async(std::launch::async, [&] { return cache.Get(native); });
    const bool started = entered.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    EXPECT_TRUE(started);
    auto replacementResult = std::async(std::launch::async, [&] { return cache.Get(replacement); });
    // A timeout is only a deadlock bound; order is controlled by the promises.
    const bool replacementFinished = replacementResult.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    EXPECT_TRUE(replacementFinished);
    releaseNative.set_value();
    EXPECT_EQ(nativeResult.get(), &nativeCode);
    EXPECT_EQ(replacementResult.get(), &replacementCode);
    EXPECT_EQ(cache.Get(replacement), &replacementCode);
    EXPECT_EQ(cache.Get(native), &nativeCode);
    EXPECT_EQ(nativeCalls.load(), 1u);
    EXPECT_EQ(replacementCalls.load(), 1u);
}

TEST(UnhookedModuleExportTest, ResolutionCanReenterForAnotherModule) {
    const auto first = reinterpret_cast<HMODULE>(uintptr_t{1});
    const auto second = reinterpret_cast<HMODULE>(uintptr_t{2});
    int firstCode = 0;
    int secondCode = 0;
    CachedUnhookedExport* active = nullptr;
    CachedUnhookedExport cache("factory", [&](HMODULE module, const char*) -> void* {
        if (module == second)
            return &secondCode;
        EXPECT_EQ(active->Get(second), &secondCode);
        return &firstCode;
    });
    active = &cache;
    EXPECT_EQ(cache.Get(first), &firstCode);
    EXPECT_EQ(cache.Get(second), &secondCode);
}

TEST(UnhookedModuleExportTest, TransientResolutionFailuresAreRetried) {
    const auto module = reinterpret_cast<HMODULE>(uintptr_t{1});
    unsigned calls = 0;
    int code = 0;
    CachedUnhookedExport cache("factory", [&](HMODULE, const char*) -> void* { return ++calls == 1 ? nullptr : &code; });
    EXPECT_EQ(cache.Get(module), nullptr);
    EXPECT_EQ(cache.Get(module), &code);
    EXPECT_EQ(cache.Get(module), &code);
    EXPECT_EQ(calls, 2u);
}

}  // namespace
