// Layout and painting of every setup page. Everything is measured through
// Scale() with the window's current DPI, so a monitor change re-runs exactly
// these functions.

#include "wizard.h"

#include <algorithm>

namespace ce::setup {
namespace {

int S(const Wizard& wizard, int logical) {
    return ui::Scale(wizard.dpi, logical);
}

void Show(HWND control, bool visible) {
    if (control)
        ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
}

std::wstring PageTitle(const Wizard& wizard) {
    switch (wizard.page) {
    case Page::License:
        return kProductName;
    case Page::Location:
        return L"Choose install location";
    case Page::Options:
        return L"Choose what to set up";
    case Page::ConfirmRemove:
        return L"Uninstall Capture Engine";
    case Page::Progress:
        return wizard.uninstall ? L"Removing Capture Engine"
                                : (wizard.existing.state.installed ? L"Updating Capture Engine"
                                                                   : L"Installing Capture Engine");
    case Page::Finish:
        return wizard.resultTitle;
    }
    return {};
}

std::wstring PageSubtitle(const Wizard& wizard) {
    switch (wizard.page) {
    case Page::License:
        return std::wstring(L"Setup  \x00B7  Version ") + kVersion;
    case Page::Location:
        return L"Capture Engine will be installed in this folder.";
    case Page::Options:
        return L"All of these can be changed later from the Capture Engine tray menu.";
    case Page::ConfirmRemove:
        return std::wstring(L"Version ") + kVersion;
    case Page::Progress:
        return L"Please keep this window open.";
    case Page::Finish:
        return {};
    }
    return {};
}

void DrawField(Wizard& wizard, HDC dc, const RECT& frame) {
    RECT area = frame;
    InflateRect(&area, 1, 1);
    ui::AaCanvas canvas(dc, area, ui::kBackground);
    canvas.RoundRectStroked(1, 1, area.right - area.left - 1, area.bottom - area.top - 1, S(wizard, 4), ui::kSurface,
                            ui::kStroke, std::max(1, S(wizard, 1)));
}

RECT ContentRect(const Wizard& wizard, const RECT& client) {
    return {S(wizard, kMargin), S(wizard, kHeaderHeight), client.right - S(wizard, kMargin),
            client.bottom - S(wizard, kFooterHeight) - S(wizard, 16)};
}

void DrawStatusGlyph(Wizard& wizard, HDC dc, int left, int top, COLORREF color, bool success) {
    const int size = S(wizard, 28);
    RECT area = {left, top, left + size, top + size};
    ui::AaCanvas canvas(dc, area, ui::kBackground);
    canvas.Circle(size / 2.0, size / 2.0, size / 2.0 - 1, color);
    const double width = std::max(1.6, S(wizard, 2) * 0.9);
    if (success) {
        const ui::PointF tick[3] = {{size * 0.29, size * 0.52}, {size * 0.44, size * 0.67}, {size * 0.72, size * 0.34}};
        canvas.Polyline(tick, 3, width, ui::kOnAccent);
    } else {
        const ui::PointF first[2] = {{size * 0.33, size * 0.33}, {size * 0.67, size * 0.67}};
        const ui::PointF second[2] = {{size * 0.67, size * 0.33}, {size * 0.33, size * 0.67}};
        canvas.Polyline(first, 2, width, ui::kOnAccent);
        canvas.Polyline(second, 2, width, ui::kOnAccent);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void LayoutControls(Wizard& wizard) {
    if (!wizard.window)
        return;
    RECT client{};
    GetClientRect(wizard.window, &client);
    const RECT content = ContentRect(wizard, client);
    const int contentWidth = content.right - content.left;

    // Footer buttons.
    const int buttonWidth = S(wizard, 112);
    const int buttonHeight = S(wizard, 32);
    const int gap = S(wizard, 8);
    const int footerTop = client.bottom - S(wizard, kFooterHeight);
    const int buttonTop = footerTop + (S(wizard, kFooterHeight) - buttonHeight) / 2;
    const int right = client.right - S(wizard, 24);
    MoveWindow(wizard.cancel, right - buttonWidth, buttonTop, buttonWidth, buttonHeight, TRUE);
    MoveWindow(wizard.next, right - 2 * buttonWidth - gap, buttonTop, buttonWidth, buttonHeight, TRUE);
    MoveWindow(wizard.back, right - 3 * buttonWidth - 2 * gap, buttonTop, buttonWidth, buttonHeight, TRUE);

    const int bottomRow = content.bottom - S(wizard, 30);
    if (wizard.license) {
        const int noticeHeight = wizard.existing.state.installed ? S(wizard, 48) : 0;
        const int top = content.top + noticeHeight;
        const int inset = S(wizard, 10);
        MoveWindow(wizard.license, content.left + inset, top + inset, contentWidth - 2 * inset,
                   bottomRow - S(wizard, 12) - top - 2 * inset, TRUE);
    }
    if (wizard.accept)
        MoveWindow(wizard.accept, content.left, bottomRow, contentWidth, S(wizard, 30), TRUE);
    if (wizard.launch)
        MoveWindow(wizard.launch, content.left, bottomRow, contentWidth, S(wizard, 30), TRUE);

    if (wizard.path) {
        const int fieldTop = content.top + S(wizard, 28);
        const int fieldHeight = S(wizard, 36);
        const int browseWidth = S(wizard, 104);
        const int editHeight = S(wizard, 22);
        MoveWindow(wizard.path, content.left + S(wizard, 12), fieldTop + (fieldHeight - editHeight) / 2,
                   contentWidth - browseWidth - gap - S(wizard, 24), editHeight, TRUE);
        MoveWindow(wizard.browse, content.right - browseWidth, fieldTop, browseWidth, fieldHeight, TRUE);
    }

    HWND options[] = {wizard.optDesktop, wizard.optStartMenu, wizard.optAutostart,
                      wizard.optService, wizard.optAdmin,     wizard.optPawnIo};
    const int pitch = S(wizard, 49);
    int row = 0;
    for (HWND option : options) {
        if (option)
            MoveWindow(option, content.left, content.top + row * pitch, contentWidth, S(wizard, 46), TRUE);
        ++row;
    }
    if (wizard.removeData)
        MoveWindow(wizard.removeData, content.left, content.top + S(wizard, 150), contentWidth, S(wizard, 46), TRUE);
}

void UpdatePageControls(Wizard& wizard) {
    const Page page = wizard.page;
    Show(wizard.license, page == Page::License);
    Show(wizard.accept, page == Page::License);
    Show(wizard.path, page == Page::Location);
    Show(wizard.browse, page == Page::Location);
    for (HWND option : {wizard.optDesktop, wizard.optStartMenu, wizard.optAutostart, wizard.optService, wizard.optAdmin,
                        wizard.optPawnIo})
        Show(option, page == Page::Options);
    Show(wizard.removeData, page == Page::ConfirmRemove);
    Show(wizard.launch, page == Page::Finish && !wizard.uninstall && wizard.workSucceeded);

    const bool working = page == Page::Progress;
    const bool finished = page == Page::Finish;
    Show(wizard.back, page == Page::Location || page == Page::Options);
    Show(wizard.next, !working);
    Show(wizard.cancel, !finished);
    EnableWindow(wizard.cancel, !working);

    const wchar_t* label = L"Next";
    bool nextEnabled = true;
    switch (page) {
    case Page::License:
        nextEnabled = wizard.accept && SendMessageW(wizard.accept, BM_GETCHECK, 0, 0) == BST_CHECKED;
        break;
    case Page::Location:
        nextEnabled = wizard.directoryValid;
        break;
    case Page::Options:
        label = wizard.existing.state.installed ? L"Update" : L"Install";
        break;
    case Page::ConfirmRemove:
        label = L"Uninstall";
        break;
    case Page::Finish:
        label = L"Finish";
        break;
    case Page::Progress:
        break;
    }
    SetWindowTextW(wizard.next, label);
    EnableWindow(wizard.next, nextEnabled);
    LayoutControls(wizard);
    InvalidateRect(wizard.window, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void PaintWizard(Wizard& wizard, HDC dc, const RECT& client) {
    HBRUSH background = CreateSolidBrush(ui::kBackground);
    FillRect(dc, &client, background);
    DeleteObject(background);

    const int margin = S(wizard, kMargin);
    const int footerTop = client.bottom - S(wizard, kFooterHeight);
    RECT footer = {0, footerTop, client.right, client.bottom};
    HBRUSH footerBrush = CreateSolidBrush(ui::kFooter);
    FillRect(dc, &footer, footerBrush);
    DeleteObject(footerBrush);
    RECT rule = {0, footerTop, client.right, footerTop + 1};
    HBRUSH ruleBrush = CreateSolidBrush(ui::kStroke);
    FillRect(dc, &rule, ruleBrush);
    DeleteObject(ruleBrush);

    // Header: title and subtitle on the left, the program icon on the right.
    int titleLeft = margin;
    if (wizard.page == Page::Finish) {
        DrawStatusGlyph(wizard, dc, margin, S(wizard, 31), wizard.resultColor, wizard.workSucceeded);
        titleLeft += S(wizard, 40);
    }
    ui::DrawLabel(dc, ui::g_fonts.title, ui::kText, PageTitle(wizard),
                  {titleLeft, S(wizard, 22), client.right - margin - S(wizard, 56), S(wizard, 66)},
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const std::wstring subtitle = PageSubtitle(wizard);
    if (!subtitle.empty())
        ui::DrawLabel(dc, ui::g_fonts.subtitle, ui::kTextSecondary, subtitle,
                      {margin, S(wizard, 68), client.right - margin - S(wizard, 56), S(wizard, 92)},
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (wizard.icon) {
        const int size = S(wizard, 40);
        DrawIconEx(dc, client.right - margin - size, S(wizard, 26), wizard.icon, size, size, 0, nullptr, DI_NORMAL);
    }

    const RECT content = ContentRect(wizard, client);
    const int contentWidth = content.right - content.left;

    switch (wizard.page) {
    case Page::License: {
        int top = content.top;
        if (wizard.existing.state.installed) {
            std::wstring notice = L"Capture Engine";
            if (!wizard.existing.version.empty())
                notice += L" " + wizard.existing.version;
            notice += L" is already installed";
            if (!wizard.existing.directory.empty())
                notice += L" in " + wizard.existing.directory;
            notice += L". Setup will update it and keep your config.ini.";
            ui::DrawLabel(dc, ui::g_fonts.body, ui::kTextSecondary, notice,
                          {content.left, content.top, content.right, content.top + S(wizard, 44)},
                          DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
            top += S(wizard, 48);
        }
        if (wizard.license) {
            RECT editRect{};
            GetWindowRect(wizard.license, &editRect);
            MapWindowPoints(nullptr, wizard.window, reinterpret_cast<POINT*>(&editRect), 2);
            RECT frame = {content.left, top, content.right, editRect.bottom + S(wizard, 10)};
            DrawField(wizard, dc, frame);
        }
        break;
    }
    case Page::Location: {
        ui::DrawLabel(dc, ui::g_fonts.caption, ui::kTextSecondary, L"Install folder",
                      {content.left, content.top, content.right, content.top + S(wizard, 22)},
                      DT_LEFT | DT_TOP | DT_SINGLELINE);
        const int fieldTop = content.top + S(wizard, 28);
        const int browseWidth = S(wizard, 104);
        DrawField(wizard, dc,
                  {content.left, fieldTop, content.right - browseWidth - S(wizard, 8), fieldTop + S(wizard, 36)});
        ui::DrawLabel(dc, ui::g_fonts.body, wizard.directoryValid ? ui::kTextSecondary : ui::kCritical,
                      wizard.directoryNote,
                      {content.left, fieldTop + S(wizard, 52), content.right, content.bottom},
                      DT_LEFT | DT_TOP | DT_WORDBREAK);
        break;
    }
    case Page::Options:
        break;
    case Page::ConfirmRemove: {
        std::wstring text = L"Capture Engine will be removed from\n" + wizard.directory +
                            L"\n\nThe elevation service, the startup entry and the shortcuts are removed too. "
                            L"Your recordings, screenshots and benchmark reports are never deleted.";
        ui::DrawLabel(dc, ui::g_fonts.body, ui::kText, text, {content.left, content.top, content.right, content.top + S(wizard, 140)},
                      DT_LEFT | DT_TOP | DT_WORDBREAK);
        break;
    }
    case Page::Progress: {
        int percent = 0;
        std::wstring status;
        EnterCriticalSection(&wizard.lock);
        percent = wizard.percent;
        status = wizard.status;
        LeaveCriticalSection(&wizard.lock);
        ui::DrawLabel(dc, ui::g_fonts.body, ui::kText, status,
                      {content.left, content.top + S(wizard, 8), content.right, content.top + S(wizard, 32)},
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_PATH_ELLIPSIS);
        const int barTop = content.top + S(wizard, 48);
        const int barHeight = S(wizard, 6);
        RECT bar = {content.left, barTop, content.right, barTop + barHeight};
        {
            ui::AaCanvas canvas(dc, bar, ui::kBackground);
            canvas.RoundRect(0, 0, contentWidth - 1, barHeight - 1, barHeight / 2.0, ui::kSurface);
            const int filled = std::clamp(contentWidth * percent / 100, percent > 0 ? barHeight : 0, contentWidth);
            if (filled > 0)
                canvas.RoundRect(0, 0, filled - 1, barHeight - 1, barHeight / 2.0, ui::kAccent);
        }
        ui::DrawLabel(dc, ui::g_fonts.caption, ui::kTextSecondary, std::to_wstring(percent) + L"%",
                      {content.left, barTop + S(wizard, 14), content.right, barTop + S(wizard, 34)},
                      DT_LEFT | DT_TOP | DT_SINGLELINE);
        break;
    }
    case Page::Finish: {
        int y = content.top;
        for (const ResultLine& line : wizard.resultLines) {
            const int height = ui::MeasureLabelHeight(dc, ui::g_fonts.body, line.text, contentWidth);
            if (y + height > content.bottom - S(wizard, 36))
                break;
            ui::DrawLabel(dc, ui::g_fonts.body, line.color, line.text, {content.left, y, content.right, y + height},
                          DT_LEFT | DT_TOP | DT_WORDBREAK);
            y += height + S(wizard, 10);
        }
        break;
    }
    }
}

}  // namespace ce::setup
