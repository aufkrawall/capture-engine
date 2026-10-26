#include <gtest/gtest.h>

#include <dxgi.h>
#include <wrl/client.h>

#include "hook/runtime/com_object_module.h"

namespace {

using ce::com_object_module::ImplementingModule;
using ce::com_object_module::IsImplementedBy;

// A polymorphic object stands in for a translation layer's adapter: its vtable lives in this image.
struct ForeignAdapter {
    virtual ~ForeignAdapter() = default;
    virtual int GetDesc() { return 0; }
};

TEST(ComObjectModuleTest, ObjectIsAttributedToTheImageHoldingItsVtable) {
    ForeignAdapter adapter;
    EXPECT_EQ(ImplementingModule(&adapter), GetModuleHandleW(nullptr));
    EXPECT_TRUE(IsImplementedBy(&adapter, GetModuleHandleW(nullptr)));
    // GTA V Enhanced: DXVK's adapter reached System32's D3D11 and crashed it.
    EXPECT_FALSE(IsImplementedBy(&adapter, GetModuleHandleW(L"kernel32.dll")));
}

TEST(ComObjectModuleTest, MissingObjectOrModuleIsNeverNative) {
    ForeignAdapter adapter;
    EXPECT_EQ(ImplementingModule(nullptr), nullptr);
    EXPECT_FALSE(IsImplementedBy(nullptr, GetModuleHandleW(nullptr)));
    // A failed System32 dxgi.dll load leaves no module to accept any adapter.
    EXPECT_FALSE(IsImplementedBy(&adapter, nullptr));

    void* const nullVtable = nullptr;
    EXPECT_EQ(ImplementingModule(&nullVtable), nullptr);
}

TEST(ComObjectModuleTest, SystemDxgiAdaptersAreAttributedToSystemDxgi) {
    // The guard is only sound if System32's own adapters pass it; check that on the real runtime.
    const HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    ASSERT_NE(dxgi, nullptr);
    using CreateFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    const auto create = reinterpret_cast<CreateFactory1>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    ASSERT_NE(create, nullptr);

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    ASSERT_TRUE(SUCCEEDED(create(IID_PPV_ARGS(&factory))));
    EXPECT_TRUE(IsImplementedBy(factory.Get(), dxgi));
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(0, &adapter) == DXGI_ERROR_NOT_FOUND) {
        FreeLibrary(dxgi);
        GTEST_SKIP() << "no DXGI adapter in this session";
    }
    EXPECT_TRUE(IsImplementedBy(adapter.Get(), dxgi));
    EXPECT_FALSE(IsImplementedBy(adapter.Get(), GetModuleHandleW(nullptr)));
    adapter.Reset();
    factory.Reset();
    FreeLibrary(dxgi);
}

}  // namespace
