import sys

filepath = r'%REPO_ROOT%\hook\apis\dx9_hook.cpp'

with open(filepath, 'r', encoding='utf-8') as f:
    content = f.read()

# Find the old block
old_start = '    // Try to silently upgrade to D3D9Ex for zero-copy shared texture capture.'
old_end_marker = '        HookLogImportant("DX9: Factory QI for IDirect3D9Ex FAILED (hr=0x%08X), no D3D9Ex upgrade possible",'

start_idx = content.find(old_start)
if start_idx == -1:
    print('ERROR: Could not find start marker')
    sys.exit(1)

# Find end: the line after the closing brace
end_search_start = content.find(old_end_marker, start_idx)
if end_search_start == -1:
    print('ERROR: Could not find end marker')
    sys.exit(1)

# Find the closing "    }" for the else block
end_idx = content.find('\n    }', end_search_start)
if end_idx == -1:
    print('ERROR: Could not find closing brace')
    sys.exit(1)
end_idx += len('\n    }')

print(f'Found block at chars {start_idx}-{end_idx}')
print(f'Old block length: {end_idx - start_idx}')
print(f'Old block preview: {content[start_idx:start_idx+80]}...')

new_block = """    // Try to silently upgrade to D3D9Ex for zero-copy shared texture capture.
    // IDirect3DDevice9Ex is vtable-compatible with IDirect3DDevice9, so the
    // game interacts with it identically while our capture gains shared handles.
    //
    // We use s_d3d9ExForUpgrade (created in DetourDirect3DCreate9) since factory
    // QI for IDirect3D9Ex returns E_NOINTERFACE on Direct3DCreate9-created factories.
    // We hook GetDirect3D() on the device to return the game's original factory,
    // preventing pointer-mismatch crashes in games that compare factory pointers.
    HRESULT hr = E_FAIL;
    if (s_d3d9ExForUpgrade) {
        HookLogImportant("DX9: Attempting D3D9Ex device creation via separate factory (managed pool fix will activate)");

        // Enable MANAGED pool remapping BEFORE CreateDeviceEx so any resources
        // created during device initialization are properly remapped
        ManagedPoolFix::g_active = true;

        D3DDISPLAYMODEEX* pModeEx = nullptr;
        D3DDISPLAYMODEEX fullscreenMode = {};
        if (pPresentationParameters && !pPresentationParameters->Windowed) {
            fullscreenMode.Size = sizeof(D3DDISPLAYMODEEX);
            fullscreenMode.Width = pPresentationParameters->BackBufferWidth;
            fullscreenMode.Height = pPresentationParameters->BackBufferHeight;
            fullscreenMode.RefreshRate = pPresentationParameters->FullScreen_RefreshRateInHz;
            fullscreenMode.Format = pPresentationParameters->BackBufferFormat;
            fullscreenMode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
            pModeEx = &fullscreenMode;
        }
        IDirect3DDevice9Ex* deviceEx = nullptr;
        hr = s_d3d9ExForUpgrade->CreateDeviceEx(Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                                 pPresentationParameters, pModeEx, &deviceEx);
        if (SUCCEEDED(hr) && deviceEx) {
            s_gameOriginalFactory = self;
            self->AddRef();
            HookLogImportant("DX9: CreateDevice upgraded to D3D9Ex (zero-copy capture ready, managed pool fix active)");
            *ppReturnedDeviceInterface = static_cast<IDirect3DDevice9*>(deviceEx);
        } else {
            ManagedPoolFix::g_active = false;
            HookLogImportant("DX9: D3D9Ex CreateDeviceEx FAILED (hr=0x%08X), falling back to legacy", (unsigned)hr);
            hr = E_FAIL;
        }
    } else {
        HookLogImportant("DX9: No D3D9Ex factory available, using legacy capture path");
    }"""

content = content[:start_idx] + new_block + content[end_idx:]

with open(filepath, 'w', encoding='utf-8') as f:
    f.write(content)

print('Replacement successful')
