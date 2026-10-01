#pragma once

// The setup window: one GDI-painted window, a Windows 11 dark look, no dialog
// resources and no common controls. Declarations shared by the theme, the page
// layout/painting and the window logic.

#include "setup.h"

namespace ce::setup::ui {

// Windows 11 dark palette (WinUI dark theme values flattened onto an opaque
// window; GDI cannot alpha-blend text, so every colour is precomputed).
inline constexpr COLORREF kBackground = RGB(32, 32, 32);
inline constexpr COLORREF kFooter = RGB(28, 28, 28);
inline constexpr COLORREF kSurface = RGB(45, 45, 45);
inline constexpr COLORREF kSurfaceHover = RGB(50, 50, 50);
inline constexpr COLORREF kSurfacePressed = RGB(39, 39, 39);
inline constexpr COLORREF kStroke = RGB(62, 62, 62);
inline constexpr COLORREF kStrokeStrong = RGB(155, 155, 155);
inline constexpr COLORREF kText = RGB(255, 255, 255);
inline constexpr COLORREF kTextSecondary = RGB(197, 197, 197);
inline constexpr COLORREF kTextDisabled = RGB(120, 120, 120);
inline constexpr COLORREF kAccent = RGB(76, 194, 255);
inline constexpr COLORREF kAccentHover = RGB(71, 178, 233);
inline constexpr COLORREF kAccentPressed = RGB(66, 163, 211);
inline constexpr COLORREF kAccentDisabled = RGB(70, 70, 70);
inline constexpr COLORREF kOnAccent = RGB(0, 0, 0);
inline constexpr COLORREF kCritical = RGB(255, 153, 164);
inline constexpr COLORREF kCaution = RGB(252, 225, 0);
inline constexpr COLORREF kSuccess = RGB(108, 203, 95);

struct PointF {
    double x;
    double y;
};

struct Fonts {
    HFONT body = nullptr;
    HFONT bodyStrong = nullptr;
    HFONT caption = nullptr;
    HFONT title = nullptr;
    HFONT subtitle = nullptr;
    HFONT mono = nullptr;
    UINT dpi = 96;
};
extern Fonts g_fonts;

int Scale(UINT dpi, int logical);
UINT WindowDpi(HWND window);
void CreateFonts(UINT dpi);
void DestroyFonts();
void ApplyDarkTitleBar(HWND window);
void EnableDarkScrollbars(HWND window);

// Anti-aliased shapes: draws at 4x into a DIB and halftone-downsamples onto the
// target. GDI has no anti-aliasing of its own, and a 1-pixel rounded border is
// the first thing that gives a hand-drawn GDI dialog away.
class AaCanvas {
public:
    AaCanvas(HDC target, const RECT& area, COLORREF background);
    ~AaCanvas();
    AaCanvas(const AaCanvas&) = delete;
    AaCanvas& operator=(const AaCanvas&) = delete;
    // Coordinates are target pixels relative to the area's top-left corner.
    void RoundRect(double left, double top, double right, double bottom, double radius, COLORREF fill);
    void RoundRectStroked(double left, double top, double right, double bottom, double radius, COLORREF fill,
                          COLORREF stroke, double strokeWidth);
    void Polyline(const PointF* points, int count, double width, COLORREF color);
    void Circle(double centerX, double centerY, double radius, COLORREF fill);

private:
    static constexpr int kFactor = 4;
    HDC target_;
    RECT area_;
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
};

// "CESetupButton": push, accent (default) push and checkbox in one window class.
// Checkbox text may carry a second line after '\n' that is drawn as a caption.
inline constexpr wchar_t kButtonClass[] = L"CESetupButton";
inline constexpr UINT CEM_SETBACKGROUND = WM_APP + 40;  // lParam = COLORREF
bool RegisterButtonClass(HINSTANCE instance);

void DrawLabel(HDC dc, HFONT font, COLORREF color, const std::wstring& text, RECT rect, UINT format);
int MeasureLabelHeight(HDC dc, HFONT font, const std::wstring& text, int width);

}  // namespace ce::setup::ui
