#include "dx12_fg_switch_test_internal.h"

const uint8_t* GlyphRows(char ch) {
    static constexpr GlyphPattern kGlyphs[] = {
        {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
        {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
        {':', {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}}, {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
        {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
        {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
        {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
        {'I', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
        {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}}, {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
        {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
        {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
        {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    };
    static constexpr uint8_t kFallback[] = {0x0E, 0x11, 0x01, 0x06, 0x04, 0x00, 0x04};

    for (const GlyphPattern& glyph : kGlyphs) {
        if (glyph.ch == ch) {
            return glyph.rows;
        }
    }
    return kFallback;
}

const char* CurrentFGStatusText() {
    if (dx12_fg_switch_test_g_CurrentMode == FGMode::FSR) {
        if (dx12_fg_switch_test_g_FsrPresentCallbackStress && !dx12_fg_switch_test_g_FsrLastConfigureUsedPresentCallback) {
            return dx12_fg_switch_test_g_FsrSuspended ? "FG: FSR SUSPENDED INT" : "FG: FSR ACTIVE INT";
        }
        return dx12_fg_switch_test_g_FsrSuspended ? "FG: FSR SUSPENDED" : "FG: FSR ACTIVE";
    }
    if (dx12_fg_switch_test_g_CurrentMode == FGMode::DLSS) {
        return dx12_fg_switch_test_g_DlssSuspended ? "FG: DLSS SUSPENDED" : "FG: DLSS ACTIVE";
    }
    return "FG: OFF";
}

void DrawTextLine(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
                         const float* color, const char* text, LONG x, LONG y, LONG scale) {
    LONG cursor = x;
    for (const char* p = text; p && *p; ++p) {
        if (*p == ' ') {
            cursor += scale * 4;
            continue;
        }

        const uint8_t* rows = GlyphRows(*p);
        for (LONG row = 0; row < 7; ++row) {
            LONG col = 0;
            while (col < 5) {
                while (col < 5 && (rows[row] & (1 << (4 - col))) == 0) {
                    ++col;
                }
                const LONG runStart = col;
                while (col < 5 && (rows[row] & (1 << (4 - col))) != 0) {
                    ++col;
                }
                if (runStart == col) {
                    continue;
                }

                LONG left = cursor + runStart * scale;
                LONG top = y + row * scale;
                LONG right = cursor + col * scale;
                LONG bottom = top + scale;
                if (right <= 0 || bottom <= 0 || left >= dx12_fg_switch_test_g_WindowWidth || top >= dx12_fg_switch_test_g_WindowHeight) {
                    continue;
                }
                if (left < 0) {
                    left = 0;
                }
                if (top < 0) {
                    top = 0;
                }
                if (right > dx12_fg_switch_test_g_WindowWidth) {
                    right = dx12_fg_switch_test_g_WindowWidth;
                }
                if (bottom > dx12_fg_switch_test_g_WindowHeight) {
                    bottom = dx12_fg_switch_test_g_WindowHeight;
                }
                if (left < right && top < bottom) {
                    const D3D12_RECT rect = {left, top, right, bottom};
                    commandList->ClearRenderTargetView(rtvHandle, color, 1, &rect);
                }
            }
        }
        cursor += scale * 6;
    }
}

void DrawStatusText(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle) {
    if (!commandList || dx12_fg_switch_test_g_WindowWidth <= 48 || dx12_fg_switch_test_g_WindowHeight <= 48) {
        return;
    }

    LONG panelRight = dx12_fg_switch_test_g_WindowWidth - 16;
    if (panelRight > 560) {
        panelRight = 560;
    }
    LONG panelBottom = dx12_fg_switch_test_g_WindowHeight - 16;
    if (panelBottom > 112) {
        panelBottom = 112;
    }
    if (panelRight <= 16 || panelBottom <= 16) {
        return;
    }

    const D3D12_RECT panelRect = {16, 16, panelRight, panelBottom};
    const float panelColor[] = {0.015f, 0.015f, 0.015f, 1.0f};
    commandList->ClearRenderTargetView(rtvHandle, panelColor, 1, &panelRect);

    float textColor[] = {0.86f, 0.86f, 0.86f, 1.0f};
    if (dx12_fg_switch_test_g_CurrentMode == FGMode::FSR) {
        if (dx12_fg_switch_test_g_FsrSuspended) {
            textColor[0] = 1.0f;
            textColor[1] = 0.36f;
            textColor[2] = 0.22f;
        } else {
            textColor[0] = 0.22f;
            textColor[1] = 1.0f;
            textColor[2] = 0.54f;
        }
    } else if (dx12_fg_switch_test_g_CurrentMode == FGMode::DLSS) {
        if (dx12_fg_switch_test_g_DlssSuspended) {
            textColor[0] = 1.0f;
            textColor[1] = 0.36f;
            textColor[2] = 0.22f;
        } else {
            textColor[0] = 0.38f;
            textColor[1] = 0.66f;
            textColor[2] = 1.0f;
        }
    }

    DrawTextLine(commandList, rtvHandle, textColor, CurrentFGStatusText(), 30, 30, 4);
    DrawTextLine(commandList, rtvHandle, textColor, "1 OFF  2 DLSS  3 FSR", 30, 76, 3);
}

// Game-style HUD: "100 HP" readout plus a health bar. Drawn into both the FG UI-layer texture
// and the presented backbuffer at the same animated position, so it stays crisp and excluded
// from frame generation (base rate, no ghosting).
void DrawHudOverlay(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle, LONG hudX,
                           LONG hudY) {
    if (!commandList) {
        return;
    }
    const float textColor[] = {0.85f, 1.0f, 0.55f, 1.0f};
    DrawTextLine(commandList, rtvHandle, textColor, "100 HP", hudX, hudY, 5);

    auto drawRect = [&](LONG l, LONG t, LONG r, LONG b, const float* color) {
        if (l < 0)
            l = 0;
        if (t < 0)
            t = 0;
        if (r > dx12_fg_switch_test_g_WindowWidth)
            r = dx12_fg_switch_test_g_WindowWidth;
        if (b > dx12_fg_switch_test_g_WindowHeight)
            b = dx12_fg_switch_test_g_WindowHeight;
        if (l < r && t < b) {
            const D3D12_RECT rect = {l, t, r, b};
            commandList->ClearRenderTargetView(rtvHandle, color, 1, &rect);
        }
    };
    const LONG barTop = hudY + 44;
    const LONG barBottom = barTop + 16;
    const LONG barRight = hudX + 232;
    const float frameColor[] = {0.04f, 0.05f, 0.06f, 1.0f};
    const float fillColor[] = {0.20f, 0.95f, 0.35f, 1.0f};
    drawRect(hudX, barTop, barRight, barBottom, frameColor);
    drawRect(hudX + 3, barTop + 3, barRight - 3, barBottom - 3, fillColor);
}

void UpdateWindowTitle() {
    if (!dx12_fg_switch_test_g_Hwnd) {
        return;
    }
    wchar_t title[256];
    swprintf(title, 256, L"DX12 FG Switch Test - %dx%d (render %dx%d %hs) - %hs%hs", dx12_fg_switch_test_g_WindowWidth, dx12_fg_switch_test_g_WindowHeight,
             dx12_fg_switch_test_g_RenderWidth, dx12_fg_switch_test_g_RenderHeight,
             dx12_fg_switch_test_g_UpscalingEnabled ? testapp::fg::UpscaleQualityName(dx12_fg_switch_test_g_UpscaleQuality) : "no upscaling",
             ModeName(dx12_fg_switch_test_g_CurrentMode), IsModeSuspended(dx12_fg_switch_test_g_CurrentMode) ? " (suspended)" : "");
    SetWindowTextW(dx12_fg_switch_test_g_Hwnd, title);
}
