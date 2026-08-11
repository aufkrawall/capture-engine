#include "test_dxgi_shared_shared.h"

#include "../hook/common/dxgi_shared_internal.h"

namespace {

// The DXGI swapchain class vftable lives in read-only image data
// (dxgi.dll .rdata). CE makes it writable only transiently inside
// VirtualProtect regions, so every observation of a slot must be a plain
// volatile read - a `lock cmpxchg` used as a read faults on the read-only
// page (regression: 20260811_192706, all three dumps crash in
// RepairVTableHooksIfNeeded::<lambda0> at the first CAS).

HRESULT STDMETHODCALLTYPE DummyPresent(IDXGISwapChain*, UINT, UINT) {
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DummyPresent1(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*) {
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DummyResizeBuffers(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT) {
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DummyResizeBuffers1(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*,
                                              IUnknown* const*) {
    return S_OK;
}

void** AllocateWritableVTablePage() {
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const size_t pageSize = systemInfo.dwPageSize;
    void* page = VirtualAlloc(nullptr, pageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return page ? static_cast<void**>(page) : nullptr;
}

bool MakePageReadOnly(void* page) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(page, &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    DWORD oldProtect = 0;
    return VirtualProtect(mbi.BaseAddress, mbi.RegionSize, PAGE_READONLY, &oldProtect) != 0;
}

void AssertPageStillReadOnly(const void* page) {
    MEMORY_BASIC_INFORMATION mbi{};
    ASSERT_NE(VirtualQuery(page, &mbi, sizeof(mbi)), 0u);
    EXPECT_EQ(mbi.Protect & 0xFF, PAGE_READONLY);
}

struct ScopedVTableStateGuard {
    ~ScopedVTableStateGuard() {
        DXGIShared::dxgi_shared_s_hookedVTable = nullptr;
        DXGIShared::dxgi_shared_oPresent = nullptr;
        DXGIShared::dxgi_shared_oPresent1 = nullptr;
        DXGIShared::dxgi_shared_oResizeBuffers = nullptr;
        DXGIShared::dxgi_shared_oResizeBuffers1 = nullptr;
    }
};

void ReleaseVTablePage(void* vtable) {
    MEMORY_BASIC_INFORMATION cleanupInfo{};
    if (VirtualQuery(vtable, &cleanupInfo, sizeof(cleanupInfo)) != 0) {
        DWORD oldProtect = 0;
        VirtualProtect(cleanupInfo.BaseAddress, cleanupInfo.RegionSize, PAGE_READWRITE, &oldProtect);
    }
    VirtualFree(vtable, 0, MEM_RELEASE);
}

}  // namespace

TEST(DXGISharedVTableRepairTest, RepairReclaimsRestoredSlotsOnReadOnlyClassVftable) {
    void** vtable = AllocateWritableVTablePage();
    ASSERT_NE(vtable, nullptr);

    // Fill the slots while the page is writable (production installs hooks
    // inside a VirtualProtect region, then restores the read-only image
    // protection), and only then lock the page down like the real class
    // vftable in dxgi.dll.
    vtable[8] = (void*)DummyPresent;
    vtable[22] = (void*)DummyPresent1;
    ASSERT_TRUE(MakePageReadOnly(vtable));

    DXGIShared::dxgi_shared_s_hookedVTable = vtable;
    DXGIShared::dxgi_shared_oPresent = &DummyPresent;
    DXGIShared::dxgi_shared_oPresent1 = &DummyPresent1;
    ScopedVTableStateGuard stateGuard;

    // Pre-fix this crashed with an access violation: the first slot
    // observation used InterlockedCompareExchangePointer, i.e. `lock
    // cmpxchg`, which requires write access on the read-only vtable page.
    EXPECT_NO_FATAL_FAILURE(DXGIShared::RepairVTableHooksIfNeeded());

    EXPECT_EQ(vtable[8], (void*)DXGIShared::DetourPresent);
    EXPECT_EQ(vtable[22], (void*)DXGIShared::DetourPresent1);
    AssertPageStillReadOnly(vtable);

    ReleaseVTablePage(vtable);
}

TEST(DXGISharedVTableRepairTest, DetachRestoresOwnedSlotsOnReadOnlyClassVftable) {
    void** vtable = AllocateWritableVTablePage();
    ASSERT_NE(vtable, nullptr);

    vtable[8] = (void*)DXGIShared::DetourPresent;
    vtable[22] = (void*)DXGIShared::DetourPresent1;
    vtable[13] = (void*)DXGIShared::DetourResizeBuffers;
    vtable[39] = (void*)DXGIShared::DetourResizeBuffers1;
    ASSERT_TRUE(MakePageReadOnly(vtable));

    DXGIShared::dxgi_shared_s_hookedVTable = vtable;
    DXGIShared::dxgi_shared_oPresent = &DummyPresent;
    DXGIShared::dxgi_shared_oPresent1 = &DummyPresent1;
    DXGIShared::dxgi_shared_oResizeBuffers = &DummyResizeBuffers;
    DXGIShared::dxgi_shared_oResizeBuffers1 = &DummyResizeBuffers1;
    ScopedVTableStateGuard stateGuard;

    // Same read-only class-vftable constraint: DetachOwnedVTableSlot must not
    // run a locked operation on the page before VirtualProtect.
    EXPECT_NO_FATAL_FAILURE(DXGIShared::RemoveSwapchainVTableHooks());

    EXPECT_EQ(vtable[8], (void*)DummyPresent);
    EXPECT_EQ(vtable[22], (void*)DummyPresent1);
    EXPECT_EQ(vtable[13], (void*)DummyResizeBuffers);
    EXPECT_EQ(vtable[39], (void*)DummyResizeBuffers1);
    EXPECT_EQ(DXGIShared::dxgi_shared_s_hookedVTable, nullptr);
    AssertPageStillReadOnly(vtable);

    ReleaseVTablePage(vtable);
}