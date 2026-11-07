#include <gtest/gtest.h>

#include <d3d9.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>

#include "hook/hooking/vtable_slot_reclaim.h"

// Windows' d3d9 keeps each device's dispatch table in the device's own memory
// and copies a fresh one over it in BeginStateBlock. Every hook CE writes into
// the game's device table - Reset, the texture creation slots, EndScene, the
// state-block lifecycle and the per-draw sampler slots forced AF lives on -
// disappears the moment the game starts recording a block.
// CE's own overlay once did that to Borderlands 2 (20261103_175450, forced AF
// dead); a game recording its own blocks does the same.
//
// CE answers it from its Begin/EndStateBlock detours: every slot the rewrite
// put back to exactly CE's saved original gets CE's detour again
// (hook/hooking/vtable_slot_reclaim.h). This checks the premise and that answer
// on a real device, both kinds. A multithreaded device records through the
// same functions, so everything comes back at BeginStateBlock and its
// EndStateBlock touches nothing. A single-threaded one records SetTexture and
// SetSamplerState through other functions, which CE leaves in place; its
// EndStateBlock puts d3d9's own back, and CE's detours follow. Recording must
// keep working through all of it.
namespace {

using Microsoft::WRL::ComPtr;
using ce::vtable_slot_reclaim::OwnedSlot;
using ce::vtable_slot_reclaim::Reclaim;

constexpr size_t kReset = 16;
constexpr size_t kPresent = 17;
constexpr size_t kCreateTexture = 23;
constexpr size_t kCreateVolumeTexture = 24;
constexpr size_t kCreateCubeTexture = 25;
constexpr size_t kEndScene = 42;
constexpr size_t kCreateStateBlock = 59;
constexpr size_t kBeginStateBlock = 60;
constexpr size_t kEndStateBlock = 61;
constexpr size_t kSetTexture = 65;
constexpr size_t kGetSamplerState = 68;
constexpr size_t kSetSamplerState = 69;

// Every device slot CE hooks.
constexpr std::array<size_t, 12> kCeSlots = {kReset,           kPresent,        kCreateTexture, kCreateVolumeTexture,
                                             kCreateCubeTexture, kEndScene,     kCreateStateBlock,
                                             kBeginStateBlock, kEndStateBlock,  kSetTexture,    kGetSamplerState,
                                             kSetSamplerState};

std::array<void*, 128> g_original = {};
std::array<int, 128> g_calls = {};

template <size_t Slot, typename Fn>
Fn Original() {
    return reinterpret_cast<Fn>(g_original[Slot]);
}

HRESULT STDMETHODCALLTYPE HookedBeginStateBlock(IDirect3DDevice9* device) {
    ++g_calls[kBeginStateBlock];
    return Original<kBeginStateBlock, decltype(&HookedBeginStateBlock)>()(device);
}
HRESULT STDMETHODCALLTYPE HookedEndStateBlock(IDirect3DDevice9* device, IDirect3DStateBlock9** block) {
    ++g_calls[kEndStateBlock];
    return Original<kEndStateBlock, decltype(&HookedEndStateBlock)>()(device, block);
}
HRESULT STDMETHODCALLTYPE HookedSetSamplerState(IDirect3DDevice9* device, DWORD sampler, D3DSAMPLERSTATETYPE type,
                                                DWORD value) {
    ++g_calls[kSetSamplerState];
    return Original<kSetSamplerState, decltype(&HookedSetSamplerState)>()(device, sampler, type, value);
}
HRESULT STDMETHODCALLTYPE HookedGetSamplerState(IDirect3DDevice9* device, DWORD sampler, D3DSAMPLERSTATETYPE type,
                                                DWORD* value) {
    ++g_calls[kGetSamplerState];
    return Original<kGetSamplerState, decltype(&HookedGetSamplerState)>()(device, sampler, type, value);
}
HRESULT STDMETHODCALLTYPE HookedSetTexture(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture) {
    ++g_calls[kSetTexture];
    return Original<kSetTexture, decltype(&HookedSetTexture)>()(device, stage, texture);
}
// The slots the test never calls through.
HRESULT STDMETHODCALLTYPE NotCalled() {
    return E_FAIL;
}
HRESULT STDMETHODCALLTYPE ForeignEndScene(IDirect3DDevice9*) {
    return E_FAIL;
}

void* DetourFor(size_t slot) {
    switch (slot) {
        case kBeginStateBlock:
            return reinterpret_cast<void*>(&HookedBeginStateBlock);
        case kEndStateBlock:
            return reinterpret_cast<void*>(&HookedEndStateBlock);
        case kSetSamplerState:
            return reinterpret_cast<void*>(&HookedSetSamplerState);
        case kGetSamplerState:
            return reinterpret_cast<void*>(&HookedGetSamplerState);
        case kSetTexture:
            return reinterpret_cast<void*>(&HookedSetTexture);
        default:
            return reinterpret_cast<void*>(&NotCalled);
    }
}

class D3D9DispatchTableRewriteTest : public testing::Test {
protected:
    virtual DWORD CreationFlags() const {
        return D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED;
    }

    void SetUp() override {
        WNDCLASSW windowClass = {};
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"CeD3D9DispatchTableRewriteTest";
        RegisterClassW(&windowClass);
        window_ = CreateWindowExW(0, windowClass.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr,
                                  nullptr, windowClass.hInstance, nullptr);
        ASSERT_NE(window_, nullptr);
        d3d_.Attach(Direct3DCreate9(D3D_SDK_VERSION));
        if (!d3d_)
            GTEST_SKIP() << "Direct3D 9 is not available";
        D3DPRESENT_PARAMETERS params = {
            .BackBufferWidth = 64,
            .BackBufferHeight = 64,
            .BackBufferFormat = D3DFMT_UNKNOWN,
            .BackBufferCount = 0,
            .MultiSampleType = D3DMULTISAMPLE_NONE,
            .MultiSampleQuality = 0,
            .SwapEffect = D3DSWAPEFFECT_DISCARD,
            .hDeviceWindow = window_,
            .Windowed = TRUE,
            .EnableAutoDepthStencil = FALSE,
            .AutoDepthStencilFormat = D3DFMT_UNKNOWN,
            .Flags = 0,
            .FullScreen_RefreshRateInHz = 0,
            .PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT,
        };
        const HRESULT hr =
            d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window_, CreationFlags(), &params, &device_);
        if (FAILED(hr))
            GTEST_SKIP() << "no Direct3D 9 HAL device (hr=0x" << std::hex << static_cast<unsigned>(hr) << ")";
        table_ = *reinterpret_cast<void***>(device_.Get());
        g_calls = {};
        // What CE's VTableHook::Create does: save the slot, write the detour.
        for (size_t slot : kCeSlots) {
            g_original[slot] = table_[slot];
            owned_[ownedCount_++] = {&table_[slot], DetourFor(slot), table_[slot]};
            Write(slot, DetourFor(slot));
        }
    }

    void TearDown() override {
        if (device_) {
            for (size_t slot : kCeSlots)
                Write(slot, g_original[slot]);
        }
        device_.Reset();
        d3d_.Reset();
        if (window_)
            DestroyWindow(window_);
    }

    void Write(size_t slot, void* value) {
        DWORD protection = 0;
        ASSERT_TRUE(VirtualProtect(static_cast<void*>(&table_[slot]), sizeof(void*), PAGE_READWRITE, &protection));
        table_[slot] = value;
    }

    bool Hooked(size_t slot) const {
        return table_[slot] == DetourFor(slot);
    }

    ce::vtable_slot_reclaim::Counts ReclaimAll() const {
        return Reclaim(owned_.data(), ownedCount_);
    }

    DWORD MinFilter() {
        DWORD value = 0;
        EXPECT_EQ(device_->GetSamplerState(0, D3DSAMP_MINFILTER, &value), D3D_OK);
        return value;
    }

    HWND window_ = nullptr;
    ComPtr<IDirect3D9> d3d_;
    ComPtr<IDirect3DDevice9> device_;
    void** table_ = nullptr;
    std::array<OwnedSlot, kCeSlots.size()> owned_ = {};
    size_t ownedCount_ = 0;
};

class D3D9SingleThreadedDispatchTableRewriteTest : public D3D9DispatchTableRewriteTest {
protected:
    DWORD CreationFlags() const override {
        return D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
    }
};

}  // namespace

TEST_F(D3D9DispatchTableRewriteTest, EveryHookComesBackAfterEachRewriteAndRecordingStillWorks) {
    ASSERT_EQ(device_->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT), D3D_OK);

    ASSERT_EQ(device_->BeginStateBlock(), D3D_OK);
    EXPECT_EQ(g_calls[kBeginStateBlock], 1);
    size_t erased = 0;
    for (size_t slot : kCeSlots)
        erased += Hooked(slot) ? 0 : 1;
    EXPECT_EQ(erased, kCeSlots.size()) << "the premise: BeginStateBlock rewrote the whole table";
    const auto atBegin = ReclaimAll();
    EXPECT_EQ(atBegin.restored, kCeSlots.size());
    EXPECT_EQ(atBegin.leftAlone, 0u) << "a multithreaded device records through the same functions";

    ASSERT_EQ(device_->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR), D3D_OK);
    EXPECT_EQ(g_calls[kSetSamplerState], 2) << "the recorded set went through the hook";
    ComPtr<IDirect3DStateBlock9> block;
    ASSERT_EQ(device_->EndStateBlock(&block), D3D_OK);
    EXPECT_EQ(g_calls[kEndStateBlock], 1);
    const auto atEnd = ReclaimAll();
    EXPECT_EQ(atEnd.restored, 0u) << "a multithreaded device's EndStateBlock leaves the table alone";
    EXPECT_EQ(atEnd.leftAlone, 0u);
    for (size_t slot : kCeSlots)
        EXPECT_TRUE(Hooked(slot)) << "slot " << slot;

    EXPECT_EQ(MinFilter(), static_cast<DWORD>(D3DTEXF_POINT)) << "recorded, not applied";
    ASSERT_EQ(block->Apply(), D3D_OK);
    EXPECT_EQ(MinFilter(), static_cast<DWORD>(D3DTEXF_LINEAR));
}

TEST_F(D3D9SingleThreadedDispatchTableRewriteTest, RecordingEntriesStayUntilEndStateBlockPutsTheHooksBack) {
    ASSERT_EQ(device_->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT), D3D_OK);

    ASSERT_EQ(device_->BeginStateBlock(), D3D_OK);
    const auto atBegin = ReclaimAll();
    EXPECT_EQ(atBegin.restored, kCeSlots.size() - 2) << "the premise: BeginStateBlock rewrote the whole table";
    EXPECT_EQ(atBegin.leftAlone, 2u);
    EXPECT_FALSE(Hooked(kSetTexture)) << "recorded through another function, left to d3d9";
    EXPECT_FALSE(Hooked(kSetSamplerState));
    // What CE needs while the block is recorded: its EndStateBlock detour, to
    // put the rest back, and everything that is not a state setter.
    for (size_t slot : {kReset, kPresent, kCreateTexture, kCreateVolumeTexture, kCreateCubeTexture, kEndScene,
                        kCreateStateBlock, kBeginStateBlock, kEndStateBlock, kGetSamplerState})
        EXPECT_TRUE(Hooked(slot)) << "slot " << slot;

    ASSERT_EQ(device_->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR), D3D_OK);
    EXPECT_EQ(g_calls[kSetSamplerState], 1) << "only the set before the recording reached the hook";
    ComPtr<IDirect3DStateBlock9> block;
    ASSERT_EQ(device_->EndStateBlock(&block), D3D_OK);
    EXPECT_EQ(g_calls[kEndStateBlock], 1);
    const auto atEnd = ReclaimAll();
    EXPECT_EQ(atEnd.restored, 2u) << "EndStateBlock put back only the two recording entries";
    EXPECT_EQ(atEnd.leftAlone, 0u);
    for (size_t slot : kCeSlots)
        EXPECT_TRUE(Hooked(slot)) << "slot " << slot;

    EXPECT_EQ(MinFilter(), static_cast<DWORD>(D3DTEXF_POINT)) << "recorded, not applied";
    ASSERT_EQ(block->Apply(), D3D_OK);
    EXPECT_EQ(MinFilter(), static_cast<DWORD>(D3DTEXF_LINEAR));
}

TEST_F(D3D9DispatchTableRewriteTest, AnotherToolsHookInASlotIsLeftAlone) {
    ASSERT_EQ(device_->BeginStateBlock(), D3D_OK);
    // Between the rewrite and CE's answer another tool hooks EndScene.
    Write(kEndScene, reinterpret_cast<void*>(&ForeignEndScene));
    const auto counts = ReclaimAll();
    EXPECT_EQ(counts.leftAlone, 1u);
    EXPECT_EQ(table_[kEndScene], reinterpret_cast<void*>(&ForeignEndScene));
    ComPtr<IDirect3DStateBlock9> block;
    ASSERT_EQ(device_->EndStateBlock(&block), D3D_OK);
}

TEST_F(D3D9DispatchTableRewriteTest, NothingIsWrittenWhileTheHooksAreInPlace) {
    const auto counts = ReclaimAll();
    EXPECT_EQ(counts.restored, 0u);
    EXPECT_EQ(counts.leftAlone, 0u);
}
