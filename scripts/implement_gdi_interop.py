"""
Implement GDI interop zero-copy DX9 capture for native D3D9.
- Disables D3D9Ex promotion (causes visual corruption)
- Uses GetDC/BitBlt for GPU-accelerated D3D9-to-D3D11 transfer (WDDM 2.0+)
- No D3D9Ex, no SYSTEMMEM, no Desktop Duplication, no WGC
"""
import sys
from script_safety import resolve_repo_path, write_text_atomic

filepath = resolve_repo_path("hook/apis/dx9_hook.cpp")

with open(filepath, 'r', newline='', encoding='utf-8') as f:
    content = f.read()

changes = 0

# ===========================================================================
# 1. Disable D3D9Ex promotion in DetourCreateDevice
# ===========================================================================
old_promo = (
    '    // Try to silently upgrade to D3D9Ex for zero-copy shared texture capture.\r\n'
    '    // IDirect3DDevice9Ex is vtable-compatible with IDirect3DDevice9, so the\r\n'
    '    // game interacts with it identically while our capture gains shared handles.\r\n'
    '    //\r\n'
    '    // Unified factory approach: if the game received a D3D9Ex factory from our\r\n'
    '    // DetourDirect3DCreate9, the factory `self` IS already D3D9Ex. We use the\r\n'
    '    // same factory for CreateDeviceEx, avoiding internal D3D9 state mismatches.\r\n'
    '    // For late-injection (game holds plain D3D9), s_d3d9ExForUpgrade provides\r\n'
    '    // a separate factory as a fallback.\r\n'
    '    HRESULT hr = E_FAIL;\r\n'
    '    if (s_d3d9ExForUpgrade) {\r\n'
    '        // Check if the game\'s factory IS our D3D9Ex factory (unified approach).\r\n'
    '        // In this case, device and factory are properly associated internally.\r\n'
    '        IDirect3D9Ex* selfEx = nullptr;\r\n'
    '        bool isUnifiedFactory = SUCCEEDED(self->QueryInterface(__uuidof(IDirect3D9Ex), (void**)&selfEx)) && selfEx;\r\n'
    '        if (selfEx) selfEx->Release();\r\n'
    '\r\n'
    '        IDirect3D9Ex* factoryForDevice = isUnifiedFactory ? static_cast<IDirect3D9Ex*>(self) : s_d3d9ExForUpgrade;\r\n'
    '        HookLogImportant("DX9: Attempting D3D9Ex device creation (%s factory, managed pool fix will activate)",\r\n'
    '                         isUnifiedFactory ? "unified" : "separate");\r\n'
    '\r\n'
    '        // Enable MANAGED pool remapping BEFORE CreateDeviceEx so any resources\r\n'
    '        // created during device initialization are properly remapped\r\n'
    '        ManagedPoolFix::g_active = true;\r\n'
    '\r\n'
    '        // NOTE: Do NOT change BackBufferFormat to X8R8G8B8. While it prevents DWM\r\n'
    '        // alpha compositing artifacts, it creates a DXGI format mismatch in the\r\n'
    '        // zero-copy capture path (B8G8R8X8 vs B8G8R8A8) causing black video output.\r\n'
    '\r\n'
    '        D3DDISPLAYMODEEX* pModeEx = nullptr;\r\n'
    '        D3DDISPLAYMODEEX fullscreenMode = {};\r\n'
    '        if (pPresentationParameters && !pPresentationParameters->Windowed) {\r\n'
    '            fullscreenMode.Size = sizeof(D3DDISPLAYMODEEX);\r\n'
    '            fullscreenMode.Width = pPresentationParameters->BackBufferWidth;\r\n'
    '            fullscreenMode.Height = pPresentationParameters->BackBufferHeight;\r\n'
    '            fullscreenMode.RefreshRate = pPresentationParameters->FullScreen_RefreshRateInHz;\r\n'
    '            fullscreenMode.Format = pPresentationParameters->BackBufferFormat;\r\n'
    '            fullscreenMode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;\r\n'
    '            pModeEx = &fullscreenMode;\r\n'
    '        }\r\n'
    '        IDirect3DDevice9Ex* deviceEx = nullptr;\r\n'
    '        // Call oCreateDeviceEx directly to avoid going through DetourCreateDeviceEx\r\n'
    '        // which would double-apply vsync/MSAA/BehaviorFlags modifications.\r\n'
    '        if (oCreateDeviceEx) {\r\n'
    '            hr = oCreateDeviceEx(factoryForDevice, Adapter, DeviceType, hFocusWindow, BehaviorFlags,\r\n'
    '                                 pPresentationParameters, pModeEx, &deviceEx);\r\n'
    '        } else {\r\n'
    '            hr = factoryForDevice->CreateDeviceEx(Adapter, DeviceType, hFocusWindow, BehaviorFlags,\r\n'
    '                                                   pPresentationParameters, pModeEx, &deviceEx);\r\n'
    '        }\r\n'
    '        if (SUCCEEDED(hr) && deviceEx) {\r\n'
    '            if (!isUnifiedFactory) {\r\n'
    '                // Separate factory (late injection) - need GetDirect3D hook\r\n'
    '                s_gameOriginalFactory = self;\r\n'
    '                self->AddRef();\r\n'
    '            }\r\n'
    '            HookLogImportant("DX9: CreateDevice upgraded to D3D9Ex (zero-copy capture ready, managed pool fix active)");\r\n'
    '            *ppReturnedDeviceInterface = static_cast<IDirect3DDevice9*>(deviceEx);\r\n'
    '        } else {\r\n'
    '            ManagedPoolFix::g_active = false;\r\n'
    '            HookLogImportant("DX9: D3D9Ex CreateDeviceEx FAILED (hr=0x%08X), falling back to legacy", (unsigned)hr);\r\n'
    '            hr = E_FAIL;\r\n'
    '        }\r\n'
    '    } else {\r\n'
    '        HookLogImportant("DX9: No D3D9Ex factory available, using legacy capture path");\r\n'
    '    }\r\n'
    '\r\n'
    '    if (FAILED(hr)) {\r\n'
    '        hr = oCreateDevice(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters,\r\n'
    '                           ppReturnedDeviceInterface);\r\n'
    '    }'
)

new_promo = (
    '    // D3D9Ex promotion DISABLED - zero-copy capture via GDI interop instead.\r\n'
    '    // Native D3D9 device avoids MANAGED pool issues entirely.\r\n'
    '    HookLogImportant("DX9: Native D3D9 device (zero-copy via GDI interop)");\r\n'
    '    HRESULT hr = oCreateDevice(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters,\r\n'
    '                               ppReturnedDeviceInterface);'
)

if old_promo in content:
    content = content.replace(old_promo, new_promo)
    changes += 1
    print("[1] Disabled D3D9Ex promotion")
else:
    print("[1] ERROR: D3D9Ex promotion block not found")
    sys.exit(1)

# ===========================================================================
# 2. Add GDI interop members after zero-copy metrics
# ===========================================================================
old_members = (
    '    // Per-frame zero-copy metrics (set by PostPresentReadback, read by PresentEnd)\r\n'
    '    int32_t zeroCopyQueryWaitUs = 0;\r\n'
    '    int32_t zeroCopyReadbackUs = 0;'
)
new_members = (
    '    // Per-frame zero-copy metrics (set by PostPresentReadback, read by PresentEnd)\r\n'
    '    int32_t zeroCopyQueryWaitUs = 0;\r\n'
    '    int32_t zeroCopyReadbackUs = 0;\r\n'
    '\r\n'
    '    // GDI interop for zero-copy capture on native D3D9.\r\n'
    '    // Uses GetDC/BitBlt for GPU-accelerated D3D9->D3D11 transfer (WDDM 2.0+).\r\n'
    '    bool useGDIInterop = false;\r\n'
    '    IDirect3DSurface9* gdiCopySurface = nullptr;  // D3D9 render target for StretchRect\r\n'
    '    ID3D11Texture2D* gdiTexture = nullptr;         // D3D11 GDI-compatible intermediate\r\n'
    '    IDXGISurface1* gdiSurface = nullptr;            // DXGI surface for GetDC\r\n'
    '    bool gdiPendingBlit = false;                    // StretchRect done, awaiting BitBlt'
)
if old_members in content:
    content = content.replace(old_members, new_members)
    changes += 1
    print("[2] Added GDI interop members")
else:
    print("[2] ERROR: Member insertion point not found")
    sys.exit(1)

# ===========================================================================
# 3. Add SetupGDIInterop + CompleteGDIInteropCapture methods before CreateD3D11Device
# ===========================================================================
old_create = '    bool CreateD3D11Device() {'
new_methods = (
    '    // Set up GDI interop: D3D9 render target + D3D11 GDI-compatible texture.\r\n'
    '    // On WDDM 2.0+ (Win10+), BitBlt between GPU-backed DCs uses the GPU blitter.\r\n'
    '    bool SetupGDIInterop(IDirect3DDevice9* device) {\r\n'
    '        if (!d3d11Device || !d3d11Context) return false;\r\n'
    '\r\n'
    '        // Create D3D9 render target for StretchRect destination\r\n'
    '        HRESULT hr = device->CreateRenderTarget(\r\n'
    '            width, height, d3d9Format, D3DMULTISAMPLE_NONE, 0, FALSE,\r\n'
    '            &gdiCopySurface, nullptr);\r\n'
    '        if (FAILED(hr)) {\r\n'
    '            HookLogImportant("DX9: GDI interop: CreateRenderTarget failed (0x%08x)", (unsigned)hr);\r\n'
    '            return false;\r\n'
    '        }\r\n'
    '\r\n'
    '        // Verify GetDC works on this render target\r\n'
    '        HDC testDC = nullptr;\r\n'
    '        hr = gdiCopySurface->GetDC(&testDC);\r\n'
    '        if (FAILED(hr) || !testDC) {\r\n'
    '            HookLogImportant("DX9: GDI interop: GetDC on RT failed (0x%08x) - not GDI-compatible",\r\n'
    '                             (unsigned)hr);\r\n'
    '            gdiCopySurface->Release();\r\n'
    '            gdiCopySurface = nullptr;\r\n'
    '            return false;\r\n'
    '        }\r\n'
    '        gdiCopySurface->ReleaseDC(testDC);\r\n'
    '\r\n'
    '        // Create D3D11 GDI-compatible texture\r\n'
    '        D3D11_TEXTURE2D_DESC gdiDesc = {};\r\n'
    '        gdiDesc.Width = width;\r\n'
    '        gdiDesc.Height = height;\r\n'
    '        gdiDesc.MipLevels = 1;\r\n'
    '        gdiDesc.ArraySize = 1;\r\n'
    '        gdiDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;\r\n'
    '        gdiDesc.SampleDesc.Count = 1;\r\n'
    '        gdiDesc.Usage = D3D11_USAGE_DEFAULT;\r\n'
    '        gdiDesc.BindFlags = D3D11_BIND_RENDER_TARGET;\r\n'
    '        gdiDesc.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;\r\n'
    '\r\n'
    '        hr = d3d11Device->CreateTexture2D(&gdiDesc, nullptr, &gdiTexture);\r\n'
    '        if (FAILED(hr)) {\r\n'
    '            HookLogImportant("DX9: GDI interop: D3D11 CreateTexture2D failed (0x%08x)", (unsigned)hr);\r\n'
    '            gdiCopySurface->Release();\r\n'
    '            gdiCopySurface = nullptr;\r\n'
    '            return false;\r\n'
    '        }\r\n'
    '\r\n'
    '        hr = gdiTexture->QueryInterface(__uuidof(IDXGISurface1), (void**)&gdiSurface);\r\n'
    '        if (FAILED(hr)) {\r\n'
    '            HookLogImportant("DX9: GDI interop: IDXGISurface1 QI failed (0x%08x)", (unsigned)hr);\r\n'
    '            gdiTexture->Release(); gdiTexture = nullptr;\r\n'
    '            gdiCopySurface->Release(); gdiCopySurface = nullptr;\r\n'
    '            return false;\r\n'
    '        }\r\n'
    '\r\n'
    '        HookLogImportant("DX9: GDI interop ready: %ux%u (D3D9 RT + D3D11 GDI texture)", width, height);\r\n'
    '        return true;\r\n'
    '    }\r\n'
    '\r\n'
    '    // Complete GDI interop transfer: GetDC + BitBlt + CopySubresourceRegion.\r\n'
    '    // Called after Present when StretchRect has completed (flushed by Present).\r\n'
    '    void CompleteGDIInteropCapture() {\r\n'
    '        if (!gdiCopySurface || !gdiSurface || !d3d11Context) return;\r\n'
    '\r\n'
    '        static int64_t qpcFreq = 0;\r\n'
    '        if (qpcFreq == 0) {\r\n'
    '            LARGE_INTEGER f;\r\n'
    '            QueryPerformanceFrequency(&f);\r\n'
    '            qpcFreq = f.QuadPart;\r\n'
    '        }\r\n'
    '\r\n'
    '        LARGE_INTEGER copyStart;\r\n'
    '        QueryPerformanceCounter(&copyStart);\r\n'
    '\r\n'
    '        // Get GDI DC from D3D9 render target (source)\r\n'
    '        HDC srcDC = nullptr;\r\n'
    '        HRESULT hr = gdiCopySurface->GetDC(&srcDC);\r\n'
    '        if (FAILED(hr) || !srcDC) {\r\n'
    '            static bool logged = false;\r\n'
    '            if (!logged) { HookLogImportant("DX9: GDI: GetDC(D3D9) failed 0x%08x", (unsigned)hr); logged = true; }\r\n'
    '            return;\r\n'
    '        }\r\n'
    '\r\n'
    '        // Get GDI DC from D3D11 texture (destination, discard previous)\r\n'
    '        HDC dstDC = nullptr;\r\n'
    '        hr = gdiSurface->GetDC(TRUE, &dstDC);\r\n'
    '        if (FAILED(hr) || !dstDC) {\r\n'
    '            gdiCopySurface->ReleaseDC(srcDC);\r\n'
    '            static bool logged = false;\r\n'
    '            if (!logged) { HookLogImportant("DX9: GDI: GetDC(D3D11) failed 0x%08x", (unsigned)hr); logged = true; }\r\n'
    '            return;\r\n'
    '        }\r\n'
    '\r\n'
    '        // GPU-accelerated blit on WDDM 2.0+ (both surfaces are GPU-resident)\r\n'
    '        BitBlt(dstDC, 0, 0, width, height, srcDC, 0, 0, SRCCOPY);\r\n'
    '\r\n'
    '        gdiSurface->ReleaseDC(nullptr);\r\n'
    '        gdiCopySurface->ReleaseDC(srcDC);\r\n'
    '\r\n'
    '        LARGE_INTEGER blitEnd;\r\n'
    '        QueryPerformanceCounter(&blitEnd);\r\n'
    '        zeroCopyQueryWaitUs = static_cast<int32_t>(((blitEnd.QuadPart - copyStart.QuadPart) * 1000000) / qpcFreq);\r\n'
    '\r\n'
    '        // Copy from GDI texture to ring buffer shared texture\r\n'
    '        int idx = writeIndex.load(std::memory_order_acquire) % CAPTURE_TEXTURE_COUNT;\r\n'
    '        d3d11Context->CopySubresourceRegion(sharedTextures[idx], 0, 0, 0, 0, gdiTexture, 0, nullptr);\r\n'
    '\r\n'
    '        LARGE_INTEGER qpc;\r\n'
    '        QueryPerformanceCounter(&qpc);\r\n'
    '\r\n'
    '        if (useFences && fence && context4) {\r\n'
    '            fenceValue++;\r\n'
    '            context4->Signal(fence, fenceValue);\r\n'
    '            SignalFrameReady(g_IPC, idx, qpc.QuadPart, fenceValue);\r\n'
    '        } else {\r\n'
    '            d3d11Context->Flush();\r\n'
    '            SignalFrameReady(g_IPC, idx, qpc.QuadPart, 0);\r\n'
    '        }\r\n'
    '\r\n'
    '        zeroCopyReadbackUs = static_cast<int32_t>(((qpc.QuadPart - copyStart.QuadPart) * 1000000) / qpcFreq);\r\n'
    '        AdvanceWriteIndex();\r\n'
    '    }\r\n'
    '\r\n'
    '    bool CreateD3D11Device() {'
)
if old_create in content:
    content = content.replace(old_create, new_methods, 1)
    changes += 1
    print("[3] Added GDI interop methods")
else:
    print("[3] ERROR: CreateD3D11Device insertion point not found")
    sys.exit(1)

# ===========================================================================
# 4. In Init(), after shared handle fails, try GDI interop before staging
# ===========================================================================
old_staging = (
    '            // D3D11 Staging Path: For non-Ex devices, we use GetRenderTargetData\r\n'
    '            // to read the backbuffer into CPU memory, then UpdateSubresource to\r\n'
    '            // upload directly into D3D11 shared textures. This avoids the slow\r\n'
    '            // shmem IPC path entirely and gives the encoder real GPU textures.'
)
new_staging = (
    '            // Try GDI interop for zero-copy capture on native D3D9.\r\n'
    '            // Uses GetDC/BitBlt for GPU-accelerated D3D9->D3D11 transfer.\r\n'
    '            if (SetupGDIInterop(device)) {\r\n'
    '                useGDIInterop = true;\r\n'
    '                HookLogImportant("DX9: GDI interop zero-copy path active");\r\n'
    '                goto create_ring_buffer;\r\n'
    '            }\r\n'
    '            HookLogImportant("DX9: GDI interop unavailable, using D3D11 staging fallback");\r\n'
    '\r\n'
    '            // D3D11 Staging Path: For non-Ex devices, we use GetRenderTargetData\r\n'
    '            // to read the backbuffer into CPU memory, then UpdateSubresource to\r\n'
    '            // upload directly into D3D11 shared textures. This avoids the slow\r\n'
    '            // shmem IPC path entirely and gives the encoder real GPU textures.'
)
if old_staging in content:
    content = content.replace(old_staging, new_staging)
    changes += 1
    print("[4] Added GDI interop attempt in Init()")
else:
    print("[4] ERROR: Init staging insertion point not found")
    sys.exit(1)

# ===========================================================================
# 5. Add goto label before step 10 ring buffer creation
# ===========================================================================
old_step10 = '        EarlyLog("DX9: Init Step 10: Create Ring Buffer Shared Textures");'
new_step10 = '    create_ring_buffer:\r\n        EarlyLog("DX9: Init Step 10: Create Ring Buffer Shared Textures");'
if old_step10 in content:
    content = content.replace(old_step10, new_step10)
    changes += 1
    print("[5] Added create_ring_buffer label")
else:
    print("[5] ERROR: Step 10 label point not found")
    sys.exit(1)

# ===========================================================================
# 6. In CaptureFrame, add GDI interop StretchRect before throttle check
# ===========================================================================
old_capture = (
    '    void CaptureFrame(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer) {\r\n'
    '        if (!initialized || !backBuffer)\r\n'
    '            return;\r\n'
    '\r\n'
    '        // Check if we should throttle capture (encoder is falling behind)'
)
new_capture = (
    '    void CaptureFrame(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer) {\r\n'
    '        if (!initialized || !backBuffer)\r\n'
    '            return;\r\n'
    '\r\n'
    '        // GDI interop: StretchRect backbuffer to copySurface before Present,\r\n'
    '        // then BitBlt + D3D11 copy in PostPresentReadback after Present.\r\n'
    '        if (useGDIInterop) {\r\n'
    '            if (gdiCopySurface) {\r\n'
    '                HRESULT hr = device->StretchRect(backBuffer, nullptr, gdiCopySurface, nullptr, D3DTEXF_NONE);\r\n'
    '                if (SUCCEEDED(hr)) {\r\n'
    '                    gdiPendingBlit = true;\r\n'
    '                }\r\n'
    '            }\r\n'
    '            return;\r\n'
    '        }\r\n'
    '\r\n'
    '        // Check if we should throttle capture (encoder is falling behind)'
)
if old_capture in content:
    content = content.replace(old_capture, new_capture)
    changes += 1
    print("[6] Added GDI interop StretchRect in CaptureFrame")
else:
    print("[6] ERROR: CaptureFrame insertion point not found")
    sys.exit(1)

# ===========================================================================
# 7. In PostPresentReadback, add GDI interop capture
# ===========================================================================
old_post = (
    '    void PostPresentReadback(IDirect3DDevice9* device) {\r\n'
    '        if (!initialized)\r\n'
    '            return;\r\n'
    '\r\n'
    '        static int64_t qpcFreq = 0;'
)
new_post = (
    '    void PostPresentReadback(IDirect3DDevice9* device) {\r\n'
    '        if (!initialized)\r\n'
    '            return;\r\n'
    '\r\n'
    '        // GDI interop: complete capture after Present flushed the StretchRect\r\n'
    '        if (useGDIInterop && gdiPendingBlit) {\r\n'
    '            gdiPendingBlit = false;\r\n'
    '            CompleteGDIInteropCapture();\r\n'
    '            return;\r\n'
    '        }\r\n'
    '\r\n'
    '        static int64_t qpcFreq = 0;'
)
if old_post in content:
    content = content.replace(old_post, new_post)
    changes += 1
    print("[7] Added GDI interop capture in PostPresentReadback")
else:
    print("[7] ERROR: PostPresentReadback insertion point not found")
    sys.exit(1)

# ===========================================================================
# 8. Add GDI-INTEROP to the initialized message
# ===========================================================================
old_msg = (
    '            HookLogImportant("DX9 Capture Initialized (%s%s): %dx%d (LUID: %08x)",\r\n'
    '                    useShmem ? "SHMEM" : (useD3D11Staging ? "D3D11-STAGING" : "ZERO-COPY"),'
)
new_msg = (
    '            HookLogImportant("DX9 Capture Initialized (%s%s): %dx%d (LUID: %08x)",\r\n'
    '                    useShmem         ? "SHMEM"\r\n'
    '                    : useD3D11Staging ? "D3D11-STAGING"\r\n'
    '                    : useGDIInterop   ? "GDI-INTEROP"\r\n'
    '                    : "ZERO-COPY",'
)
if old_msg in content:
    content = content.replace(old_msg, new_msg)
    changes += 1
    print("[8] Updated initialized message")
else:
    print("[8] ERROR: Init message not found")
    sys.exit(1)

# ===========================================================================
# 9. Add GDI interop cleanup in CleanupDX9
# ===========================================================================
old_cleanup = (
    '        if (d3d11SharedTexture) {\r\n'
    '            d3d11SharedTexture->Release();\r\n'
    '            d3d11SharedTexture = nullptr;\r\n'
    '        }'
)
new_cleanup = (
    '        if (gdiSurface) {\r\n'
    '            gdiSurface->Release();\r\n'
    '            gdiSurface = nullptr;\r\n'
    '        }\r\n'
    '        if (gdiTexture) {\r\n'
    '            gdiTexture->Release();\r\n'
    '            gdiTexture = nullptr;\r\n'
    '        }\r\n'
    '        if (gdiCopySurface) {\r\n'
    '            gdiCopySurface->Release();\r\n'
    '            gdiCopySurface = nullptr;\r\n'
    '        }\r\n'
    '        useGDIInterop = false;\r\n'
    '        gdiPendingBlit = false;\r\n'
    '\r\n'
    '        if (d3d11SharedTexture) {\r\n'
    '            d3d11SharedTexture->Release();\r\n'
    '            d3d11SharedTexture = nullptr;\r\n'
    '        }'
)
if old_cleanup in content:
    content = content.replace(old_cleanup, new_cleanup)
    changes += 1
    print("[9] Added GDI interop cleanup")
else:
    print("[9] ERROR: Cleanup insertion point not found")
    sys.exit(1)

# ===========================================================================
# 10. Update the zero-copy stats condition to include GDI interop
# ===========================================================================
old_stats = '        } else if (g_DX9Capture.initialized && !g_DX9Capture.useD3D11Staging && !g_DX9Capture.useShmem) {'
new_stats = '        } else if (g_DX9Capture.initialized && (g_DX9Capture.useGDIInterop || (!g_DX9Capture.useD3D11Staging && !g_DX9Capture.useShmem))) {'
if old_stats in content:
    content = content.replace(old_stats, new_stats)
    changes += 1
    print("[10] Updated zero-copy stats condition")
else:
    print("[10] WARNING: Stats condition not found (non-critical)")

# ===========================================================================
# Write
# ===========================================================================
write_text_atomic(filepath, content, newline='')

print(f"\nTotal changes: {changes}")
if changes >= 9:
    print("SUCCESS: All GDI interop changes applied")
else:
    print(f"WARNING: Only {changes}/10 changes applied")
    sys.exit(1)
