// Windows 11 dark look for the setup window: fonts, title bar, anti-aliased
// shapes and the one custom control (button / accent button / checkbox).

#include "ui.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace ce::setup::ui {

Fonts g_fonts;

int Scale(UINT dpi, int logical) {
    return MulDiv(logical, static_cast<int>(dpi), 96);
}

UINT WindowDpi(HWND window) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static const auto getDpi =
        reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (getDpi && window) {
        const UINT dpi = getDpi(window);
        if (dpi)
            return dpi;
    }
    HDC screen = GetDC(nullptr);
    UINT dpi = 96;
    if (screen) {
        const int measured = GetDeviceCaps(screen, LOGPIXELSX);
        if (measured > 0)
            dpi = static_cast<UINT>(measured);
        ReleaseDC(nullptr, screen);
    }
    return dpi;
}

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------

namespace {

bool FaceInstalled(const wchar_t* face) {
    HDC screen = GetDC(nullptr);
    if (!screen)
        return false;
    LOGFONTW request{};
    request.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(request.lfFaceName, face, LF_FACESIZE - 1);
    bool found = false;
    EnumFontFamiliesExW(
        screen, &request,
        [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM parameter) -> int {
            *reinterpret_cast<bool*>(parameter) = true;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, screen);
    return found;
}

HFONT MakeFont(const wchar_t* face, int pixels, int weight, UINT dpi, bool fixedPitch = false) {
    LOGFONTW font{};
    font.lfHeight = -MulDiv(pixels, static_cast<int>(dpi), 96);
    font.lfWeight = weight;
    font.lfCharSet = DEFAULT_CHARSET;
    font.lfOutPrecision = OUT_TT_PRECIS;
    font.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    font.lfQuality = CLEARTYPE_QUALITY;
    font.lfPitchAndFamily = static_cast<BYTE>(fixedPitch ? FIXED_PITCH | FF_MODERN : VARIABLE_PITCH | FF_SWISS);
    wcsncpy(font.lfFaceName, face, LF_FACESIZE - 1);
    return CreateFontIndirectW(&font);
}

}  // namespace

void DestroyFonts() {
    for (HFONT* font : {&g_fonts.body, &g_fonts.bodyStrong, &g_fonts.caption, &g_fonts.title, &g_fonts.subtitle,
                        &g_fonts.mono}) {
        if (*font)
            DeleteObject(*font);
        *font = nullptr;
    }
}

void CreateFonts(UINT dpi) {
    DestroyFonts();
    // Segoe UI Variable is the Windows 11 UI face; older systems fall back to Segoe UI.
    static const bool variable = FaceInstalled(L"Segoe UI Variable Text");
    const wchar_t* text = variable ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t* display = variable ? L"Segoe UI Variable Display" : L"Segoe UI";
    g_fonts.body = MakeFont(text, 14, FW_NORMAL, dpi);
    g_fonts.bodyStrong = MakeFont(text, 14, FW_SEMIBOLD, dpi);
    g_fonts.caption = MakeFont(text, 12, FW_NORMAL, dpi);
    g_fonts.title = MakeFont(display, 28, FW_SEMIBOLD, dpi);
    g_fonts.subtitle = MakeFont(text, 14, FW_NORMAL, dpi);
    g_fonts.mono = MakeFont(L"Consolas", 12, FW_NORMAL, dpi, true);
    g_fonts.dpi = dpi;
}

// ---------------------------------------------------------------------------
// Window chrome
// ---------------------------------------------------------------------------

void ApplyDarkTitleBar(HWND window) {
    HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dwm)
        return;
    using SetAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    const auto setAttribute = reinterpret_cast<SetAttributeFn>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (setAttribute) {
        const BOOL dark = TRUE;
        if (FAILED(setAttribute(window, 20, &dark, sizeof(dark))))  // DWMWA_USE_IMMERSIVE_DARK_MODE
            setAttribute(window, 19, &dark, sizeof(dark));
        // Windows 11 22000+: caption and border match the page instead of a grey bar.
        const COLORREF caption = kBackground;
        const COLORREF text = kText;
        setAttribute(window, 35, &caption, sizeof(caption));  // DWMWA_CAPTION_COLOR
        setAttribute(window, 36, &text, sizeof(text));        // DWMWA_TEXT_COLOR
        setAttribute(window, 34, &caption, sizeof(caption));  // DWMWA_BORDER_COLOR
        const DWORD rounded = 2;                              // DWMWCP_ROUND
        setAttribute(window, 33, &rounded, sizeof(rounded));  // DWMWA_WINDOW_CORNER_PREFERENCE
    }
    FreeLibrary(dwm);
}

void EnableDarkScrollbars(HWND window) {
    HMODULE theme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!theme)
        return;
    using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    const auto setTheme = reinterpret_cast<SetWindowThemeFn>(GetProcAddress(theme, "SetWindowTheme"));
    if (setTheme)
        setTheme(window, L"DarkMode_Explorer", nullptr);
    FreeLibrary(theme);
}

// ---------------------------------------------------------------------------
// Anti-aliased canvas
// ---------------------------------------------------------------------------

AaCanvas::AaCanvas(HDC target, const RECT& area, COLORREF background) : target_(target), area_(area) {
    const int width = area.right - area.left;
    const int height = area.bottom - area.top;
    dc_ = CreateCompatibleDC(target);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width * kFactor;
    info.bmiHeader.biHeight = -height * kFactor;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    previous_ = SelectObject(dc_, bitmap_);
    HBRUSH brush = CreateSolidBrush(background);
    const RECT whole = {0, 0, width * kFactor, height * kFactor};
    FillRect(dc_, &whole, brush);
    DeleteObject(brush);
}

AaCanvas::~AaCanvas() {
    const int width = area_.right - area_.left;
    const int height = area_.bottom - area_.top;
    SetStretchBltMode(target_, HALFTONE);
    SetBrushOrgEx(target_, 0, 0, nullptr);
    StretchBlt(target_, area_.left, area_.top, width, height, dc_, 0, 0, width * kFactor, height * kFactor, SRCCOPY);
    SelectObject(dc_, previous_);
    DeleteObject(bitmap_);
    DeleteDC(dc_);
}

void AaCanvas::RoundRect(double left, double top, double right, double bottom, double radius, COLORREF fill) {
    HBRUSH brush = CreateSolidBrush(fill);
    HGDIOBJ oldBrush = SelectObject(dc_, brush);
    HGDIOBJ oldPen = SelectObject(dc_, GetStockObject(NULL_PEN));
    const int diameter = static_cast<int>(std::lround(radius * 2 * kFactor));
    // GDI excludes the right/bottom edge and, with a null pen, fills one pixel less.
    ::RoundRect(dc_, static_cast<int>(std::lround(left * kFactor)), static_cast<int>(std::lround(top * kFactor)),
                static_cast<int>(std::lround(right * kFactor)) + 1, static_cast<int>(std::lround(bottom * kFactor)) + 1,
                diameter, diameter);
    SelectObject(dc_, oldPen);
    SelectObject(dc_, oldBrush);
    DeleteObject(brush);
}

void AaCanvas::RoundRectStroked(double left, double top, double right, double bottom, double radius, COLORREF fill,
                                COLORREF stroke, double strokeWidth) {
    RoundRect(left, top, right, bottom, radius, stroke);
    RoundRect(left + strokeWidth, top + strokeWidth, right - strokeWidth, bottom - strokeWidth,
              std::max(0.0, radius - strokeWidth), fill);
}

void AaCanvas::Polyline(const PointF* points, int count, double width, COLORREF color) {
    LOGBRUSH brush{BS_SOLID, color, 0};
    HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
                            std::max<DWORD>(1, static_cast<DWORD>(std::lround(width * kFactor))), &brush, 0, nullptr);
    HGDIOBJ oldPen = SelectObject(dc_, pen);
    std::vector<POINT> scaled(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        scaled[static_cast<size_t>(index)] = {static_cast<LONG>(std::lround(points[index].x * kFactor)),
                                              static_cast<LONG>(std::lround(points[index].y * kFactor))};
    }
    ::Polyline(dc_, scaled.data(), count);
    SelectObject(dc_, oldPen);
    DeleteObject(pen);
}

void AaCanvas::Circle(double centerX, double centerY, double radius, COLORREF fill) {
    RoundRect(centerX - radius, centerY - radius, centerX + radius, centerY + radius, radius, fill);
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

void DrawLabel(HDC dc, HFONT font, COLORREF color, const std::wstring& text, RECT rect, UINT format) {
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format | DT_NOPREFIX);
    SelectObject(dc, old);
}

int MeasureLabelHeight(HDC dc, HFONT font, const std::wstring& text, int width) {
    HGDIOBJ old = SelectObject(dc, font);
    RECT rect = {0, 0, width, 0};
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    return rect.bottom - rect.top;
}

// ---------------------------------------------------------------------------
// Button control
// ---------------------------------------------------------------------------

namespace {

enum class ButtonKind { Push, Accent, Check };

struct ButtonState {
    bool hover = false;
    bool pressed = false;
    bool tracking = false;
    bool focus = false;
    int check = BST_UNCHECKED;
    COLORREF background = kBackground;
};

ButtonKind KindOf(HWND window) {
    const LONG style = GetWindowLongW(window, GWL_STYLE) & BS_TYPEMASK;
    if (style == BS_DEFPUSHBUTTON)
        return ButtonKind::Accent;
    if (style == BS_AUTOCHECKBOX || style == BS_CHECKBOX)
        return ButtonKind::Check;
    return ButtonKind::Push;
}

// One entry per live button window; the UI runs on a single thread, so no lock.
std::map<HWND, ButtonState>& States() {
    static std::map<HWND, ButtonState> states;
    return states;
}

ButtonState* StateOf(HWND window) {
    const auto found = States().find(window);
    return found == States().end() ? nullptr : &found->second;
}

void PaintButton(HWND window, HDC target) {
    ButtonState* state = StateOf(window);
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right;
    const int height = client.bottom;
    if (!state || width <= 0 || height <= 0)
        return;
    const UINT dpi = WindowDpi(window);
    const ButtonKind kind = KindOf(window);
    const bool enabled = IsWindowEnabled(window) != FALSE;

    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    HGDIOBJ previous = SelectObject(dc, bitmap);
    HBRUSH backgroundBrush = CreateSolidBrush(state->background);
    FillRect(dc, &client, backgroundBrush);
    DeleteObject(backgroundBrush);

    wchar_t buffer[512] = {};
    GetWindowTextW(window, buffer, 512);
    std::wstring text = buffer;

    if (kind == ButtonKind::Check) {
        std::wstring title = text;
        std::wstring description;
        const size_t newline = text.find(L'\n');
        if (newline != std::wstring::npos) {
            title = text.substr(0, newline);
            description = text.substr(newline + 1);
        }
        const bool checked = state->check == BST_CHECKED;
        const int box = Scale(dpi, 20);
        const int pad = Scale(dpi, 3);  // room for the focus ring
        {
            AaCanvas canvas(dc, client, state->background);
            if (state->focus && enabled)
                canvas.RoundRectStroked(pad - Scale(dpi, 3), pad - Scale(dpi, 3), pad + box + Scale(dpi, 3),
                                        pad + box + Scale(dpi, 3), Scale(dpi, 7), state->background, kText,
                                        std::max(1, Scale(dpi, 2)));
            COLORREF fill;
            COLORREF stroke;
            if (checked) {
                fill = !enabled ? RGB(85, 85, 85) : state->pressed ? kAccentPressed : state->hover ? kAccentHover : kAccent;
                stroke = fill;
            } else {
                fill = !enabled ? kBackground : state->pressed ? kSurfacePressed : state->hover ? kSurfaceHover : kSurface;
                stroke = enabled ? kStrokeStrong : RGB(90, 90, 90);
            }
            const double border = std::max(1, Scale(dpi, 1));
            canvas.RoundRectStroked(pad, pad, pad + box, pad + box, Scale(dpi, 4), fill, stroke, border);
            if (checked) {
                const PointF tick[3] = {{pad + box * 0.27, pad + box * 0.53},
                                        {pad + box * 0.43, pad + box * 0.68},
                                        {pad + box * 0.74, pad + box * 0.33}};
                canvas.Polyline(tick, 3, std::max(1.4, Scale(dpi, 2) * 0.85), enabled ? kOnAccent : kTextDisabled);
            }
        }
        RECT titleRect = {pad + box + Scale(dpi, 12), pad, width, pad + Scale(dpi, 22)};
        DrawLabel(dc, g_fonts.body, enabled ? kText : kTextDisabled, title, titleRect,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (!description.empty()) {
            RECT descriptionRect = {pad + box + Scale(dpi, 12), pad + Scale(dpi, 22), width, height};
            DrawLabel(dc, g_fonts.caption, enabled ? kTextSecondary : kTextDisabled, description, descriptionRect,
                      DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
    } else {
        const bool accent = kind == ButtonKind::Accent;
        COLORREF fill;
        COLORREF stroke;
        COLORREF label;
        if (accent) {
            fill = !enabled ? kAccentDisabled : state->pressed ? kAccentPressed : state->hover ? kAccentHover : kAccent;
            stroke = fill;
            label = enabled ? kOnAccent : kTextDisabled;
        } else {
            fill = !enabled ? kBackground : state->pressed ? kSurfacePressed : state->hover ? kSurfaceHover : kSurface;
            stroke = kStroke;
            label = enabled ? kText : kTextDisabled;
        }
        {
            AaCanvas canvas(dc, client, state->background);
            if (state->focus && enabled) {
                canvas.RoundRectStroked(0, 0, width - 1, height - 1, Scale(dpi, 5), state->background, kText,
                                        std::max(1, Scale(dpi, 2)));
                const double inset = Scale(dpi, 3);
                canvas.RoundRectStroked(inset, inset, width - 1 - inset, height - 1 - inset, Scale(dpi, 4), fill, stroke,
                                        std::max(1, Scale(dpi, 1)));
            } else {
                canvas.RoundRectStroked(0, 0, width - 1, height - 1, Scale(dpi, 4), fill, stroke,
                                        std::max(1, Scale(dpi, 1)));
            }
        }
        RECT labelRect = client;
        if (state->pressed)
            OffsetRect(&labelRect, 0, 1);
        DrawLabel(dc, accent ? g_fonts.bodyStrong : g_fonts.body, label, text, labelRect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

void NotifyClick(HWND window) {
    ButtonState* state = StateOf(window);
    if (KindOf(window) == ButtonKind::Check && state)
        state->check = state->check == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED;
    InvalidateRect(window, nullptr, FALSE);
    SendMessageW(GetParent(window), WM_COMMAND,
                 MAKEWPARAM(static_cast<WORD>(GetWindowLongPtrW(window, GWLP_ID)), BN_CLICKED),
                 reinterpret_cast<LPARAM>(window));
}

LRESULT CALLBACK ButtonProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    ButtonState* state = StateOf(window);
    switch (message) {
    case WM_NCCREATE:
        States().try_emplace(window);
        break;
    case WM_NCDESTROY:
        States().erase(window);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        PaintButton(window, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        PaintButton(window, reinterpret_cast<HDC>(wParam));
        return 0;
    case WM_SETTEXT:
    case WM_ENABLE: {
        const LRESULT result = DefWindowProcW(window, message, wParam, lParam);
        InvalidateRect(window, nullptr, FALSE);
        return result;
    }
    case WM_MOUSEMOVE:
        if (state && !state->tracking) {
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
            state->tracking = true;
        }
        if (state && !state->hover) {
            state->hover = true;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (state) {
            state->hover = false;
            state->tracking = false;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (state) {
            SetFocus(window);
            SetCapture(window);
            state->pressed = true;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (state && state->pressed) {
            state->pressed = false;
            ReleaseCapture();
            RECT client{};
            GetClientRect(window, &client);
            const POINT point = {static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
            if (PtInRect(&client, point))
                NotifyClick(window);
            else
                InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (state && state->pressed) {
            state->pressed = false;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if (state && wParam == VK_SPACE) {
            state->pressed = true;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_KEYUP:
        if (state && wParam == VK_SPACE && state->pressed) {
            state->pressed = false;
            NotifyClick(window);
            return 0;
        }
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        if (state) {
            state->focus = message == WM_SETFOCUS;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_GETDLGCODE:
        return KindOf(window) == ButtonKind::Check
                   ? DLGC_BUTTON
                   : DLGC_BUTTON | (KindOf(window) == ButtonKind::Accent ? DLGC_DEFPUSHBUTTON : DLGC_UNDEFPUSHBUTTON);
    case BM_GETCHECK:
        return state ? state->check : BST_UNCHECKED;
    case BM_SETCHECK:
        if (state) {
            state->check = wParam == BST_CHECKED ? BST_CHECKED : BST_UNCHECKED;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case CEM_SETBACKGROUND:
        if (state) {
            state->background = static_cast<COLORREF>(lParam);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

bool RegisterButtonClass(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = ButtonProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    windowClass.lpszClassName = kButtonClass;
    return RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

}  // namespace ce::setup::ui
