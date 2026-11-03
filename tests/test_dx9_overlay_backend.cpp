#include <gtest/gtest.h>

#include <d3d9.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "hook/overlay/custom_overlay_dx9.h"

// The D3D9 overlay backend on a real device. It used to save and restore the
// application's state with a D3DSBT_ALL block captured and applied around every
// draw: every render state, sampler, texture stage, shader constant, transform
// and light, each frame, plus a driver revalidating all of it on the game's next
// draw. It now reads and restores by hand exactly the states and bindings it
// changes, so each must still come back exactly - including the vertex
// declaration it replaces with an FVF, and the viewport and scissor rectangle
// that switching to the back buffer resets.
//
// Nor may it record a state block on the application's device: Windows' d3d9
// keeps each device's dispatch table in the device's own memory and copies a
// fresh one over it in BeginStateBlock, erasing every hook CE or any other tool
// wrote into it (tests/test_d3d9_dispatch_table_rewrite.cpp). A recorded
// overlay block did exactly that to Borderlands 2's device - forced anisotropic
// filtering went dead the moment the overlay first drew (session
// 20261103_175450).
namespace {
using Microsoft::WRL::ComPtr;
using CustomOverlay::DrawCommand;
using CustomOverlay::DrawVertex;
using CustomOverlay::DX9Backend;

constexpr UINT kSize = 64;

struct RenderStateValue {
    D3DRENDERSTATETYPE state;
    DWORD value;
};

// Each differs from what the overlay sets.
constexpr RenderStateValue kGameRenderStates[] = {
    {D3DRS_ALPHABLENDENABLE, FALSE},
    {D3DRS_SRCBLEND, D3DBLEND_ONE},
    {D3DRS_DESTBLEND, D3DBLEND_ZERO},
    {D3DRS_BLENDOP, D3DBLENDOP_MAX},
    {D3DRS_ZENABLE, D3DZB_TRUE},
    {D3DRS_CULLMODE, D3DCULL_CW},
    {D3DRS_LIGHTING, TRUE},
    {D3DRS_SEPARATEALPHABLENDENABLE, TRUE},
    {D3DRS_SRCBLENDALPHA, D3DBLEND_ZERO},
    {D3DRS_DESTBLENDALPHA, D3DBLEND_ONE},
    {D3DRS_BLENDOPALPHA, D3DBLENDOP_MIN},
    {D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED},
    {D3DRS_ALPHATESTENABLE, TRUE},
    {D3DRS_STENCILENABLE, TRUE},
    {D3DRS_SCISSORTESTENABLE, TRUE},
    {D3DRS_SRGBWRITEENABLE, TRUE},
    {D3DRS_FOGENABLE, TRUE},
};

struct StageStateValue {
    DWORD stage;
    D3DTEXTURESTAGESTATETYPE state;
    DWORD value;
};

constexpr StageStateValue kGameStageStates[] = {
    {0, D3DTSS_COLOROP, D3DTOP_ADD},       {0, D3DTSS_COLORARG1, D3DTA_CURRENT}, {0, D3DTSS_COLORARG2, D3DTA_TFACTOR},
    {0, D3DTSS_ALPHAOP, D3DTOP_SUBTRACT},  {0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR}, {0, D3DTSS_ALPHAARG2, D3DTA_CURRENT},
    {1, D3DTSS_COLOROP, D3DTOP_MODULATE},  {1, D3DTSS_ALPHAOP, D3DTOP_ADD},
};

struct SamplerStateValue {
    D3DSAMPLERSTATETYPE state;
    DWORD value;
};

constexpr SamplerStateValue kGameSamplerStates[] = {
    {D3DSAMP_MINFILTER, D3DTEXF_POINT},
    {D3DSAMP_MAGFILTER, D3DTEXF_POINT},
    {D3DSAMP_MIPFILTER, D3DTEXF_LINEAR},
    {D3DSAMP_SRGBTEXTURE, 1},
};

constexpr D3DVERTEXELEMENT9 kGameDeclaration[] = {
    {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
    {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
    D3DDECL_END(),
};
constexpr UINT kGameStride = 20;

class DX9OverlayBackendTest : public testing::Test {
protected:
    virtual DWORD CreationFlags() const {
        return D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
    }

    void SetUp() override {
        WNDCLASSW windowClass = {};
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"CeDx9OverlayBackendTest";
        RegisterClassW(&windowClass);
        window_ = CreateWindowExW(0, windowClass.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, kSize, kSize, nullptr,
                                  nullptr, windowClass.hInstance, nullptr);
        ASSERT_NE(window_, nullptr);

        d3d_.Attach(Direct3DCreate9(D3D_SDK_VERSION));
        if (!d3d_)
            GTEST_SKIP() << "Direct3D 9 is not available";
        D3DPRESENT_PARAMETERS params = {};
        params.Windowed = TRUE;
        params.SwapEffect = D3DSWAPEFFECT_DISCARD;
        params.BackBufferFormat = D3DFMT_UNKNOWN;
        params.BackBufferWidth = kSize;
        params.BackBufferHeight = kSize;
        params.hDeviceWindow = window_;
        HRESULT hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window_, CreationFlags(), &params,
                                        &device_);
        if (FAILED(hr))
            GTEST_SKIP() << "no Direct3D 9 HAL device (hr=0x" << std::hex << static_cast<unsigned>(hr) << ")";

        backend_ = std::make_unique<DX9Backend>(device_.Get());
        const std::vector<uint8_t> font(4 * 4 * 4, 0xFF);
        ASSERT_TRUE(backend_->Initialize(4, 4, font.data()));
        vertices_ = {{0, 0, 0, 0, 0xFF0000FF}, {8, 0, 1, 0, 0xFF0000FF}, {8, 8, 1, 1, 0xFF0000FF},
                     {0, 8, 0, 1, 0xFF0000FF}};
        indices_ = {0, 1, 2, 0, 2, 3, 0, 1, 2, 0, 2, 3};
        DrawCommand text = {};
        text.vertexCount = 4;
        text.indexCount = 6;
        text.useTexture = true;
        DrawCommand solid = text;
        solid.indexOffset = 6;
        solid.useTexture = false;
        commands_ = {text, solid};

        ASSERT_EQ(device_->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &gameTexture_, nullptr), D3D_OK);
        ASSERT_EQ(device_->CreateVertexDeclaration(kGameDeclaration, &gameDeclaration_), D3D_OK);
        ASSERT_EQ(device_->CreateVertexBuffer(kGameStride * 4, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &gameVertices_,
                                              nullptr),
                  D3D_OK);
        ASSERT_EQ(device_->CreateIndexBuffer(12, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &gameIndices_,
                                             nullptr),
                  D3D_OK);
        ASSERT_EQ(device_->CreateRenderTarget(kSize, kSize, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                              &gameTarget_, nullptr),
                  D3D_OK);
        ASSERT_EQ(device_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer_), D3D_OK);
    }

    void TearDown() override {
        if (backend_)
            backend_->Shutdown();
        backend_.reset();
        backBuffer_.Reset();
        gameTarget_.Reset();
        gameIndices_.Reset();
        gameVertices_.Reset();
        gameDeclaration_.Reset();
        gameTexture_.Reset();
        device_.Reset();
        d3d_.Reset();
        if (window_)
            DestroyWindow(window_);
    }

    // What a game leaves bound when it presents, with every state the overlay
    // touches set to something the overlay does not use.
    void SetGameState(IDirect3DSurface9* target, const D3DVIEWPORT9& viewport, const RECT& scissor) {
        ASSERT_EQ(device_->SetRenderTarget(0, target), D3D_OK);
        // After the target: switching it resets both.
        ASSERT_EQ(device_->SetViewport(&viewport), D3D_OK);
        ASSERT_EQ(device_->SetScissorRect(&scissor), D3D_OK);
        for (const RenderStateValue& value : kGameRenderStates)
            ASSERT_EQ(device_->SetRenderState(value.state, value.value), D3D_OK) << value.state;
        for (const StageStateValue& value : kGameStageStates)
            ASSERT_EQ(device_->SetTextureStageState(value.stage, value.state, value.value), D3D_OK) << value.state;
        for (const SamplerStateValue& value : kGameSamplerStates)
            ASSERT_EQ(device_->SetSamplerState(0, value.state, value.value), D3D_OK) << value.state;
        ASSERT_EQ(device_->SetTexture(0, gameTexture_.Get()), D3D_OK);
        ASSERT_EQ(device_->SetVertexDeclaration(gameDeclaration_.Get()), D3D_OK);
        ASSERT_EQ(device_->SetStreamSource(0, gameVertices_.Get(), 0, kGameStride), D3D_OK);
        ASSERT_EQ(device_->SetIndices(gameIndices_.Get()), D3D_OK);
    }

    void ExpectGameState(IDirect3DSurface9* target, const D3DVIEWPORT9& viewport, const RECT& scissor) {
        for (const RenderStateValue& value : kGameRenderStates) {
            DWORD actual = 0xDEADBEEF;
            EXPECT_EQ(device_->GetRenderState(value.state, &actual), D3D_OK);
            EXPECT_EQ(actual, value.value) << "render state " << value.state;
        }
        for (const StageStateValue& value : kGameStageStates) {
            DWORD actual = 0xDEADBEEF;
            EXPECT_EQ(device_->GetTextureStageState(value.stage, value.state, &actual), D3D_OK);
            EXPECT_EQ(actual, value.value) << "stage " << value.stage << " state " << value.state;
        }
        for (const SamplerStateValue& value : kGameSamplerStates) {
            DWORD actual = 0xDEADBEEF;
            EXPECT_EQ(device_->GetSamplerState(0, value.state, &actual), D3D_OK);
            EXPECT_EQ(actual, value.value) << "sampler state " << value.state;
        }

        ComPtr<IDirect3DBaseTexture9> texture;
        EXPECT_EQ(device_->GetTexture(0, &texture), D3D_OK);
        EXPECT_EQ(texture.Get(), gameTexture_.Get());
        ComPtr<IDirect3DVertexDeclaration9> declaration;
        EXPECT_EQ(device_->GetVertexDeclaration(&declaration), D3D_OK);
        EXPECT_EQ(declaration.Get(), gameDeclaration_.Get()) << "the overlay's FVF must not stay bound";
        ComPtr<IDirect3DVertexBuffer9> vertices;
        UINT offset = 99;
        UINT stride = 99;
        EXPECT_EQ(device_->GetStreamSource(0, &vertices, &offset, &stride), D3D_OK);
        EXPECT_EQ(vertices.Get(), gameVertices_.Get());
        EXPECT_EQ(offset, 0u);
        EXPECT_EQ(stride, kGameStride);
        ComPtr<IDirect3DIndexBuffer9> indices;
        EXPECT_EQ(device_->GetIndices(&indices), D3D_OK);
        EXPECT_EQ(indices.Get(), gameIndices_.Get());
        ComPtr<IDirect3DVertexShader9> vertexShader;
        ComPtr<IDirect3DPixelShader9> pixelShader;
        EXPECT_EQ(device_->GetVertexShader(&vertexShader), D3D_OK);
        EXPECT_EQ(device_->GetPixelShader(&pixelShader), D3D_OK);
        EXPECT_EQ(vertexShader.Get(), nullptr);
        EXPECT_EQ(pixelShader.Get(), nullptr);

        ComPtr<IDirect3DSurface9> boundTarget;
        EXPECT_EQ(device_->GetRenderTarget(0, &boundTarget), D3D_OK);
        EXPECT_EQ(boundTarget.Get(), target);
        D3DVIEWPORT9 actualViewport = {};
        EXPECT_EQ(device_->GetViewport(&actualViewport), D3D_OK);
        EXPECT_EQ(actualViewport.X, viewport.X);
        EXPECT_EQ(actualViewport.Y, viewport.Y);
        EXPECT_EQ(actualViewport.Width, viewport.Width);
        EXPECT_EQ(actualViewport.Height, viewport.Height);
        EXPECT_FLOAT_EQ(actualViewport.MinZ, viewport.MinZ);
        EXPECT_FLOAT_EQ(actualViewport.MaxZ, viewport.MaxZ);
        RECT actualScissor = {};
        EXPECT_EQ(device_->GetScissorRect(&actualScissor), D3D_OK);
        EXPECT_EQ(actualScissor.left, scissor.left);
        EXPECT_EQ(actualScissor.top, scissor.top);
        EXPECT_EQ(actualScissor.right, scissor.right);
        EXPECT_EQ(actualScissor.bottom, scissor.bottom);
    }

    void Render() {
        ASSERT_EQ(device_->BeginScene(), D3D_OK);
        backend_->Render(vertices_, indices_, commands_, kSize, kSize);
        ASSERT_EQ(device_->EndScene(), D3D_OK);
    }

    // Writes `hook` into the device's CreateQuery slot, which the overlay never
    // calls while it draws, and returns what the slot held.
    void* HookCreateQuerySlot(void* hook) {
        void** table = *reinterpret_cast<void***>(device_.Get());
        DWORD protection = 0;
        EXPECT_TRUE(VirtualProtect(&table[kCreateQuerySlot], sizeof(void*), PAGE_READWRITE, &protection));
        void* previous = table[kCreateQuerySlot];
        table[kCreateQuerySlot] = hook;
        return previous;
    }
    void* CreateQuerySlot() const {
        return (*reinterpret_cast<void***>(device_.Get()))[kCreateQuerySlot];
    }

    static constexpr size_t kCreateQuerySlot = 118;

    HWND window_ = nullptr;
    ComPtr<IDirect3D9> d3d_;
    ComPtr<IDirect3DDevice9> device_;
    ComPtr<IDirect3DTexture9> gameTexture_;
    ComPtr<IDirect3DVertexDeclaration9> gameDeclaration_;
    ComPtr<IDirect3DVertexBuffer9> gameVertices_;
    ComPtr<IDirect3DIndexBuffer9> gameIndices_;
    ComPtr<IDirect3DSurface9> gameTarget_;
    ComPtr<IDirect3DSurface9> backBuffer_;
    std::unique_ptr<DX9Backend> backend_;
    std::vector<DrawVertex> vertices_;
    std::vector<uint16_t> indices_;
    std::vector<DrawCommand> commands_;
};

}  // namespace

TEST_F(DX9OverlayBackendTest, RestoresEveryStateItChangesIncludingTheTargetsViewportAndScissor) {
    // The game left an offscreen target bound: the overlay switches to the back
    // buffer, which resets the viewport and scissor rectangle.
    const D3DVIEWPORT9 viewport = {4, 4, 32, 32, 0.25f, 0.75f};
    const RECT scissor = {1, 2, 30, 40};
    SetGameState(gameTarget_.Get(), viewport, scissor);
    Render();
    ExpectGameState(gameTarget_.Get(), viewport, scissor);
}

TEST_F(DX9OverlayBackendTest, EachFrameCapturesTheStateTheGameHasThen) {
    const D3DVIEWPORT9 first = {4, 4, 32, 32, 0.25f, 0.75f};
    const RECT firstScissor = {1, 2, 30, 40};
    SetGameState(gameTarget_.Get(), first, firstScissor);
    Render();

    // Next frame: the back buffer is bound and some values changed. The block
    // recorded on the first frame must restore these, not the first frame's.
    const D3DVIEWPORT9 second = {2, 6, 16, 20, 0.0f, 0.5f};
    const RECT secondScissor = {3, 4, 10, 12};
    SetGameState(backBuffer_.Get(), second, secondScissor);
    Render();
    ExpectGameState(backBuffer_.Get(), second, secondScissor);
}

namespace {

HRESULT STDMETHODCALLTYPE HookedCreateQuery(IDirect3DDevice9*, D3DQUERYTYPE, IDirect3DQuery9**) {
    return D3DERR_INVALIDCALL;
}

}  // namespace

TEST_F(DX9OverlayBackendTest, NeverRewritesTheDevicesDispatchTable) {
    void* const hook = reinterpret_cast<void*>(&HookedCreateQuery);
    void* const original = HookCreateQuerySlot(hook);
    SetGameState(gameTarget_.Get(), {4, 4, 32, 32, 0.25f, 0.75f}, {1, 2, 30, 40});
    Render();
    Render();
    EXPECT_EQ(CreateQuerySlot(), hook) << "the overlay erased a hook in the application's device";

    // The premise: recording a block on this runtime does erase it.
    ASSERT_EQ(device_->BeginStateBlock(), D3D_OK);
    ComPtr<IDirect3DStateBlock9> recorded;
    ASSERT_EQ(device_->EndStateBlock(&recorded), D3D_OK);
    EXPECT_NE(CreateQuerySlot(), hook);
    HookCreateQuerySlot(original);
}

namespace {

DWORD ReadPixel(IDirect3DDevice9* device, IDirect3DSurface9* target, UINT x, UINT y) {
    D3DSURFACE_DESC desc = {};
    EXPECT_EQ(target->GetDesc(&desc), D3D_OK);
    ComPtr<IDirect3DSurface9> readback;
    EXPECT_EQ(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &readback,
                                                  nullptr),
              D3D_OK);
    EXPECT_EQ(device->GetRenderTargetData(target, readback.Get()), D3D_OK);
    D3DLOCKED_RECT locked = {};
    EXPECT_EQ(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY), D3D_OK);
    const DWORD pixel = static_cast<const DWORD*>(locked.pBits)[y * (locked.Pitch / 4) + x] & 0x00FFFFFFu;
    readback->UnlockRect();
    return pixel;
}

}  // namespace

// Each uploaded frame lands behind the previous one in the vertex buffer and is
// drawn from there; the indices stay where the first frame put them.
TEST_F(DX9OverlayBackendTest, EachFrameDrawsItsOwnGeometryWhereverItLandsInTheVertexBuffer) {
    constexpr DWORD kRed = 0x00FF0000u;
    ASSERT_EQ(device_->SetRenderTarget(0, backBuffer_.Get()), D3D_OK);
    for (int frame = 0; frame < 3; ++frame) {
        const float left = 16.0f * static_cast<float>(frame);
        for (size_t i = 0; i < vertices_.size(); ++i)
            vertices_[i].x = left + ((i == 1 || i == 2) ? 8.0f : 0.0f);
        backend_->OnDrawDataChanged();
        ASSERT_EQ(device_->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000u, 1.0f, 0), D3D_OK);
        Render();
        EXPECT_EQ(ReadPixel(device_.Get(), backBuffer_.Get(), static_cast<UINT>(left) + 4, 4), kRed) << frame;
        if (frame > 0)
            EXPECT_EQ(ReadPixel(device_.Get(), backBuffer_.Get(), 4, 4), 0u) << "frame " << frame << " drew old geometry";
    }
}

// Drawn through the font texture, a shape must look exactly as it does on the diffuse-only path.
TEST_F(DX9OverlayBackendTest, AShapeThroughTheAtlasMatchesTheDiffusePathExactly) {
    ASSERT_EQ(device_->SetRenderTarget(0, backBuffer_.Get()), D3D_OK);
    ASSERT_EQ(device_->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF204060u, 1.0f, 0), D3D_OK);

    DX9Backend atlasBackend(device_.Get());
    CustomOverlay::Renderer renderer;
    ASSERT_TRUE(renderer.Initialize(&atlasBackend, 1.0f));
    constexpr uint32_t kOpaque = 0xFFC06030;       // ABGR
    constexpr uint32_t kTranslucent = 0x80C06030;  // ABGR
    renderer.BeginFrame(kSize, kSize);
    renderer.DrawRectFilled(8, 8, 8, 8, kOpaque);
    renderer.DrawText(0, 40, "A", 0xFFFFFFFF);
    renderer.DrawRectFilled(24, 8, 8, 8, kTranslucent);
    ASSERT_EQ(device_->BeginScene(), D3D_OK);
    renderer.EndFrame();
    ASSERT_EQ(device_->EndScene(), D3D_OK);
    ASSERT_EQ(renderer.GetCommands().size(), 1u) << "shapes and text in one draw call";
    ASSERT_TRUE(renderer.GetCommands().front().useTexture);
    EXPECT_EQ(ReadPixel(device_.Get(), backBuffer_.Get(), 12, 12), 0x003060C0u);

    // The same translucent rectangle on the diffuse-only path, elsewhere on the same background.
    const auto rect = [](float left) {
        return std::vector<DrawVertex>{{left, 8, 0, 0, kTranslucent},
                                       {left + 8, 8, 0, 0, kTranslucent},
                                       {left + 8, 16, 0, 0, kTranslucent},
                                       {left, 16, 0, 0, kTranslucent}};
    };
    vertices_ = rect(40);
    indices_ = {0, 1, 2, 0, 2, 3};
    DrawCommand solid = {};
    solid.vertexCount = 4;
    solid.indexCount = 6;
    solid.useTexture = false;
    commands_ = {solid};
    backend_->OnDrawDataChanged();
    Render();
    EXPECT_EQ(ReadPixel(device_.Get(), backBuffer_.Get(), 28, 12), ReadPixel(device_.Get(), backBuffer_.Get(), 44, 12));
}

TEST_F(DX9OverlayBackendTest, RestoresAFixedFunctionFormatTheGameSetInsteadOfADeclaration) {
    SetGameState(gameTarget_.Get(), {4, 4, 32, 32, 0.25f, 0.75f}, {1, 2, 30, 40});
    constexpr DWORD kGameFvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX2;
    ASSERT_EQ(device_->SetFVF(kGameFvf), D3D_OK);
    Render();
    DWORD fvf = 0;
    EXPECT_EQ(device_->GetFVF(&fvf), D3D_OK);
    EXPECT_EQ(fvf, kGameFvf);
}

namespace {

// A pure device answers no Get* call for state a block can hold, so the overlay
// cannot read the game's values there and brackets its draw with a D3DSBT_ALL
// block instead - which is captured and applied, never recorded.
class DX9PureDeviceOverlayBackendTest : public DX9OverlayBackendTest {
protected:
    DWORD CreationFlags() const override {
        return D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_PUREDEVICE | D3DCREATE_FPU_PRESERVE;
    }
};

}  // namespace

TEST_F(DX9PureDeviceOverlayBackendTest, DrawsWithoutRewritingTheDevicesDispatchTable) {
    void* const hook = reinterpret_cast<void*>(&HookedCreateQuery);
    void* const original = HookCreateQuerySlot(hook);
    SetGameState(gameTarget_.Get(), {4, 4, 32, 32, 0.25f, 0.75f}, {1, 2, 30, 40});
    Render();
    Render();
    EXPECT_EQ(CreateQuerySlot(), hook);
    // Still the game's target: the overlay switched to the back buffer and back.
    ComPtr<IDirect3DSurface9> target;
    EXPECT_EQ(device_->GetRenderTarget(0, &target), D3D_OK);
    EXPECT_EQ(target.Get(), gameTarget_.Get());
    HookCreateQuerySlot(original);
}
