#ifndef GEOVOICE_STUDIO_STYLE_H
#define GEOVOICE_STUDIO_STYLE_H
#include <windows.h>

typedef struct {
    COLORREF page, panel, line, text, muted, accent, header, header_text, mint, stripe;
    BOOL contrast;
} StudioPalette;

void studio_palette(StudioPalette *colors);
void studio_workspace_paint(HDC dc, RECT client, RECT points, RECT map, int dpi, const StudioPalette *colors);
void studio_button_paint(const DRAWITEMSTRUCT *item, int dpi, BOOL hot, const StudioPalette *colors);
#endif
