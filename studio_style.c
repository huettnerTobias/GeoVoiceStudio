#include "studio_style.h"
#include "studio_ui.h"

void studio_palette(StudioPalette *c) {
    HIGHCONTRASTW hc = {.cbSize = sizeof hc};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON)) {
        COLORREF face = GetSysColor(COLOR_WINDOW), text = GetSysColor(COLOR_WINDOWTEXT);
        *c = (StudioPalette){.page = face, .panel = face, .line = text, .text = text, .muted = text,
                            .accent = GetSysColor(COLOR_HIGHLIGHT), .header = face, .header_text = text,
                            .mint = GetSysColor(COLOR_HIGHLIGHT), .stripe = face, .contrast = TRUE};
    } else {
        *c = (StudioPalette){.page = RGB(239, 244, 241), .panel = RGB(255, 255, 255),
                            .line = RGB(204, 217, 210), .text = RGB(26, 49, 44), .muted = RGB(75, 99, 90),
                            .accent = RGB(20, 102, 83), .header = RGB(17, 58, 50),
                            .header_text = RGB(245, 255, 249), .mint = RGB(173, 235, 203),
                            .stripe = RGB(246, 249, 247)};
    }
}

static void fill(HDC dc, RECT rect, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, GetStockObject(DC_BRUSH));
}
static void rounded(HDC dc, RECT rect, int radius, COLORREF background, COLORREF stroke) {
    SetDCBrushColor(dc, background);
    SetDCPenColor(dc, stroke);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
}
static int scale(int v, int dpi) { return MulDiv(v, dpi, 96); }

void studio_workspace_paint(HDC dc, RECT client, RECT points, RECT map, int dpi, const StudioPalette *c) {
    int state = SaveDC(dc);
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(DC_PEN));
    fill(dc, client, c->page);
    RECT top = {0, 0, client.right, scale(62, dpi)};
    fill(dc, top, c->header);
    RECT toolbar = {0, top.bottom, client.right, scale(122, dpi)};
    fill(dc, toolbar, c->panel);
    RECT rule = {0, toolbar.bottom - 1, client.right, toolbar.bottom};
    fill(dc, rule, c->line);
    int radius = scale(14, dpi);
    rounded(dc, points, radius, c->panel, c->line);
    rounded(dc, map, radius, c->panel, c->line);
    RECT footer = {0, client.bottom - scale(32, dpi), client.right, client.bottom};
    fill(dc, footer, c->panel);
    rule = (RECT){0, footer.top, client.right, footer.top + 1};
    fill(dc, rule, c->line);

    /* A route with three stops is also recognizable at system zoom. */
    int x = scale(20, dpi), y = scale(15, dpi), size = scale(32, dpi);
    RECT symbol = {x, y, x + size, y + size};
    rounded(dc, symbol, scale(9, dpi), c->mint, c->mint);
    COLORREF ink = c->contrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : c->header;
    HPEN pen = CreatePen(PS_SOLID, scale(2, dpi), ink);
    HGDIOBJ old = SelectObject(dc, pen);
    POINT stops[] = {{x + scale(7, dpi), y + scale(23, dpi)}, {x + scale(15, dpi), y + scale(12, dpi)},
                     {x + scale(25, dpi), y + scale(8, dpi)}};
    Polyline(dc, stops, ARRAYSIZE(stops));
    SetDCBrushColor(dc, c->mint);
    for (size_t i = 0; i < ARRAYSIZE(stops); i++) {
        int r = scale(2, dpi);
        Ellipse(dc, stops[i].x - r, stops[i].y - r, stops[i].x + r + 1, stops[i].y + r + 1);
    }
    SelectObject(dc, old);
    DeleteObject(pen);
    RestoreDC(dc, state);
}

void studio_button_paint(const DRAWITEMSTRUCT *item, int dpi, BOOL hot, const StudioPalette *c) {
    HDC dc = item->hDC;
    int state = SaveDC(dc);
    RECT r = item->rcItem;
    BOOL disabled = (item->itemState & ODS_DISABLED) != 0, pressed = (item->itemState & ODS_SELECTED) != 0;
    BOOL header = item->CtlID == ID_FILE_IMPORT || item->CtlID == ID_FILE_EXPORT;
    BOOL primary = item->CtlID == ID_FILE_EXPORT || item->CtlID == ID_POINT_NEW ||
                   (item->CtlID == ID_ROUTE_NEW && !studio_library.count);
    COLORREF ink = c->text, edge = c->line, face = c->panel;
    COLORREF surround = header ? c->header : c->panel;
    if (c->contrast) {
        if (disabled)
            ink = GetSysColor(COLOR_GRAYTEXT);
        else if (pressed || (primary && !hot)) {
            ink = GetSysColor(COLOR_HIGHLIGHTTEXT);
            face = c->accent;
        }
    } else if (disabled) {
        ink = header ? RGB(179, 204, 191) : RGB(106, 118, 111);
        edge = header ? RGB(65, 99, 82) : RGB(223, 231, 226);
        face = header ? RGB(46, 78, 65) : RGB(245, 247, 245);
    } else if (primary) {
        face = header ? (pressed ? RGB(127, 203, 163) : hot ? RGB(194, 247, 216) : c->mint)
                      : (pressed ? RGB(10, 69, 56) : hot ? RGB(18, 86, 71) : c->accent);
        edge = face;
        ink = header ? c->header : c->panel;
    } else if (header) {
        face = pressed ? RGB(37, 83, 70) : hot ? RGB(48, 95, 79) : c->header;
        edge = RGB(117, 152, 134);
        ink = c->header_text;
    } else {
        face = pressed ? RGB(219, 234, 225) : hot ? RGB(237, 246, 240) : c->panel;
        if (item->CtlID == ID_POINT_DELETE)
            ink = RGB(137, 64, 48);
    }
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(DC_PEN));
    fill(dc, r, surround);
    rounded(dc, r, scale(9, dpi), face, edge);
    wchar_t text[128];
    GetWindowTextW(item->hwndItem, text, ARRAYSIZE(text));
    SelectObject(dc, (HFONT)SendMessageW(item->hwndItem, WM_GETFONT, 0, 0));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ink);
    DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if ((item->itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS) {
        InflateRect(&r, -scale(4, dpi), -scale(4, dpi));
        SetBkColor(dc, face);
        DrawFocusRect(dc, &r);
    }
    RestoreDC(dc, state);
}
