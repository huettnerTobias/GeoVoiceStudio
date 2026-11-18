#include "map_internal.h"
#include <windowsx.h>
#include "map_image.h"
#include "map_orthofoto.h"
#include <commctrl.h>
#include <shellapi.h>
#include <strsafe.h>
#include <stdlib.h>
#include <float.h>

#define CACHE_COUNT 256
#define TILE_READY (WM_APP + 411)
#define MAP_REG L"Software\\GeoVoice\\Studio\\Map"
typedef struct {
    LONG refs;
    MapVector *vector;
    wchar_t path[MAP_PATH], credit[512];
    unsigned zoom_levels;
} Source;
typedef struct {
    int x, y, z;
    BOOL valid;
    HBITMAP bitmap;
    ULONGLONG used;
} Tile;
typedef struct {
    double x, y;
    wchar_t *name;
} Marker;
typedef struct {
    HWND window, credit_link;
    int credit_height;
    MapPick pick;
    void *context;
    double x, y;
    int zoom, selected;
    Marker *points;
    size_t count;
    BOOL lines, labels, drag;
    POINT anchor;
    Source *source;
    ULONG_PTR gdiplus;
    CRITICAL_SECTION lock;
    HANDLE wake, worker;
    BOOL stopping, busy;
    Tile active, jobs[CACHE_COUNT], cache[CACHE_COUNT];
    int pending;
} Canvas;

static Canvas *canvas(HWND window) { return (Canvas *)GetWindowLongPtrW(window, GWLP_USERDATA); }
static int available_zoom(unsigned levels, int wanted, int step, int fallback) {
    for (int z = wanted; z >= 0 && z <= 20; z += step)
        if ((levels >> z) & 1u)
            return z;
    return fallback;
}
static int fit_zoom(unsigned levels, int wanted) {
    if (!levels || ((levels >> wanted) & 1u))
        return wanted;
    int lower = available_zoom(levels, wanted - 1, -1, -1);
    return lower >= 0 ? lower : available_zoom(levels, wanted + 1, 1, wanted);
}
static void layout_credit(Canvas *c) {
    RECT r;
    SIZE size = {0};
    if (!c || !c->credit_link)
        return;
    HDC dc = GetDC(c->credit_link);
    HGDIOBJ old = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    GetTextExtentPoint32W(dc, ORTHOFOTO_CREDIT, ARRAYSIZE(ORTHOFOTO_CREDIT) - 1, &size);
    SelectObject(dc, old);
    ReleaseDC(c->credit_link, dc);
    GetClientRect(c->window, &r);
    int width = min(r.right, size.cx + 12);
    c->credit_height = max(20, size.cy + 3);
    MoveWindow(c->credit_link, r.right - width, r.bottom - c->credit_height, width, c->credit_height, TRUE);
}
static void release(Source *s) {
    if (s && !InterlockedDecrement(&s->refs)) {
        map_vector_close(s->vector);
        free(s);
    }
}
static void color_fill(HDC dc, RECT rect, COLORREF col) {
    SetDCBrushColor(dc, col);
    FillRect(dc, &rect, GetStockObject(DC_BRUSH));
}
static HBITMAP draw_tile(Source *s, Tile t) {
    BITMAPINFO info = {.bmiHeader = {.biSize = sizeof(BITMAPINFOHEADER),
                                     .biWidth = 256,
                                     .biHeight = -256,
                                     .biPlanes = 1,
                                     .biBitCount = 32,
                                     .biCompression = BI_RGB}};
    HDC dc = CreateCompatibleDC(NULL);
    void *pixels = NULL;
    HBITMAP bmp = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
    if (!dc || !bmp) {
        if (dc)
            DeleteDC(dc);
        if (bmp)
            DeleteObject(bmp);
        return NULL;
    }
    HGDIOBJ old = SelectObject(dc, bmp);
    RECT box = {0, 0, 256, 256};
    color_fill(dc, box, RGB(241, 240, 231));
    if (s->vector) {
        if (!map_vector_tile(s->vector, dc, t.z, t.x, t.y)) {
            color_fill(dc, box, GetSysColor(COLOR_WINDOW));
            SetBkMode(dc, TRANSPARENT);
            SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
            DrawTextW(dc, L"Kartendaten nicht lesbar", -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    } else {
        wchar_t path[MAP_PATH + 80];
        GpImage *im = NULL;
        const wchar_t *exts[] = {L"png", L"jpg", L"jpeg"};
        for (int i = 0; i < 3 && !im; i++) {
            StringCchPrintfW(path, ARRAYSIZE(path), L"%ls\\%d\\%d\\%d.%ls", s->path, t.z, t.x, t.y, exts[i]);
            im = map_tile_image(path);
        }
        if (im) {
            GpGraphics *g = NULL;
            if (GdipCreateFromHDC(dc, &g) == Ok) {
                GdipDrawImageRectI(g, im, 0, 0, 256, 256);
                GdipDeleteGraphics(g);
            }
            GdipDisposeImage(im);
        } else {
            FrameRect(dc, &box, GetSysColorBrush(COLOR_3DFACE));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(132, 136, 130));
            SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
            DrawTextW(dc, L"Keine Kachel", -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    GdiFlush();
    SelectObject(dc, old);
    DeleteDC(dc);
    return bmp;
}
static BOOL same(Tile a, Tile b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
static DWORD WINAPI render_worker(void *arg) {
    Canvas *c = arg;
    for (;;) {
        WaitForSingleObject(c->wake, INFINITE);
        EnterCriticalSection(&c->lock);
        if (c->stopping) {
            LeaveCriticalSection(&c->lock);
            break;
        }
        if (!c->source || !c->pending) {
            ResetEvent(c->wake);
            LeaveCriticalSection(&c->lock);
            continue;
        }
        Tile t = c->jobs[--c->pending];
        Source *s = c->source;
        InterlockedIncrement(&s->refs);
        c->busy = TRUE;
        c->active = t;
        LeaveCriticalSection(&c->lock);
        HBITMAP bitmap = draw_tile(s, t);
        EnterCriticalSection(&c->lock);
        if (c->source == s && !c->stopping) {
            int slot = 0;
            for (int i = 0; i < CACHE_COUNT; i++) {
                if (!c->cache[i].valid) {
                    slot = i;
                    break;
                }
                if (c->cache[i].used < c->cache[slot].used)
                    slot = i;
            }
            if (c->cache[slot].bitmap)
                DeleteObject(c->cache[slot].bitmap);
            t.valid = TRUE;
            t.used = GetTickCount64();
            t.bitmap = bitmap;
            c->cache[slot] = t;
            bitmap = NULL;
            PostMessageW(c->window, TILE_READY, 0, 0);
        }
        c->busy = FALSE;
        LeaveCriticalSection(&c->lock);
        if (bitmap)
            DeleteObject(bitmap);
        release(s);
    }
    return 0;
}
static void clear_cache(Canvas *c) {
    c->pending = 0;
    for (int i = 0; i < CACHE_COUNT; i++) {
        if (c->cache[i].bitmap)
            DeleteObject(c->cache[i].bitmap);
        c->cache[i] = (Tile){0};
    }
}
static void source(Canvas *c, Source *s, double lat, double lon, int z) {
    EnterCriticalSection(&c->lock);
    Source *old = c->source;
    if (old)
        map_vector_cancel(old->vector);
    c->source = s;
    c->busy = FALSE;
    clear_cache(c);
    LeaveCriticalSection(&c->lock);
    release(old);
    c->x = map_x(lon);
    c->y = map_y(lat);
    c->zoom = z;
    ShowWindow(c->credit_link, wcscmp(s->credit, ORTHOFOTO_CREDIT) == 0 ? SW_SHOWNOACTIVATE : SW_HIDE);
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, MAP_REG, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL) ==
        ERROR_SUCCESS) {
        RegSetValueExW(key, L"Path", 0, REG_SZ, (BYTE *)s->path,
                       (DWORD)((wcslen(s->path) + 1) * sizeof(wchar_t)));
        RegSetValueExW(key, L"Credit", 0, REG_SZ, (BYTE *)s->credit,
                       (DWORD)((wcslen(s->credit) + 1) * sizeof(wchar_t)));
        double view[] = {lat, lon, (double)z};
        RegSetValueExW(key, L"View", 0, REG_BINARY, (BYTE *)view, sizeof view);
        RegCloseKey(key);
    }
    InvalidateRect(c->window, NULL, FALSE);
}
BOOL map_use_file(HWND window, const wchar_t *path) {
    Canvas *c = canvas(window);
    if (!c)
        return FALSE;
    Source *s = calloc(1, sizeof *s);
    if (!s)
        return FALSE;
    s->refs = 1;
    s->vector = map_vector_open(path);
    if (!s->vector || FAILED(StringCchCopyW(s->path, MAP_PATH, path))) {
        release(s);
        return FALSE;
    }
    StringCchCopyW(s->credit, ARRAYSIZE(s->credit), MAP_CREDIT);
    double lat, lon;
    int z;
    map_vector_home(s->vector, &lat, &lon, &z);
    source(c, s, lat, lon, z);
    return TRUE;
}
BOOL map_use_directory(HWND window, const wchar_t *path, const wchar_t *credit, double lat, double lon,
                       int z) {
    Canvas *c = canvas(window);
    DWORD attrs = GetFileAttributesW(path);
    if (!c || attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        return FALSE;
    Source *s = calloc(1, sizeof *s);
    if (!s)
        return FALSE;
    s->refs = 1;
    if (FAILED(StringCchCopyW(s->path, MAP_PATH, path))) {
        release(s);
        return FALSE;
    }
    StringCchCopyW(s->credit, ARRAYSIZE(s->credit), credit);
    if (wcscmp(credit, ORTHOFOTO_CREDIT) == 0) {
        for (int level = 0; level <= 20; ++level) {
            wchar_t dir[MAP_PATH];
            if (FAILED(StringCchPrintfW(dir, MAP_PATH, L"%ls\\%d", path, level)))
                break;
            DWORD attr = GetFileAttributesW(dir);
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) &&
                !(attr & FILE_ATTRIBUTE_REPARSE_POINT))
                s->zoom_levels |= 1u << level;
        }
    }
    source(c, s, lat, lon, fit_zoom(s->zoom_levels, (int)fmax(0, fmin(20, z))));
    return TRUE;
}
void map_current(HWND window, wchar_t *path, size_t cap, double *lat, double *lon) {
    Canvas *c = canvas(window);
    if (!c)
        return;
    StringCchCopyW(path, cap, c->source ? c->source->path : L"");
    *lat = map_lat(c->y);
    *lon = map_lon(c->x);
}
static POINT screen_point(const Canvas *c, size_t i, RECT view, double scale) {
    double dx = c->points[i].x - c->x;
    dx -= round(dx);
    return (POINT){(LONG)round(view.right / 2. + scale * dx),
                   (LONG)round(view.bottom / 2. + scale * (c->points[i].y - c->y))};
}

static void route_line(const Canvas *c, HDC dc, RECT view, double scale) {
    POINT segment[512];
    int n = 0;
    for (size_t i = 0; i < c->count; i++) {
        POINT p = screen_point(c, i, view, scale);
        if (n && p.x == segment[n - 1].x && p.y == segment[n - 1].y)
            continue;
        segment[n++] = p;
        if (n == ARRAYSIZE(segment)) {
            Polyline(dc, segment, n);
            segment[0] = p;
            n = 1;
        }
    }
    if (n > 1)
        Polyline(dc, segment, n);
}

enum { CELL = 12, MARK = 1, LABEL = 2 };
typedef struct {
    int cols, rows;
    BYTE *cells;
} Coverage;

/* An overview shows one marker per cell and only as many names as can fit.
   Reserve the entire text rectangle so long names do not cover adjacent labels. */
static BOOL reserve_box(Coverage *grid, RECT box, BYTE mask) {
    if (!grid->cells)
        return TRUE;
    int left = max(0, box.left / CELL), top = max(0, box.top / CELL);
    int right = min(grid->cols - 1, (box.right - 1) / CELL);
    int bottom = min(grid->rows - 1, (box.bottom - 1) / CELL);
    for (int y = top; y <= bottom; y++)
        for (int x = left; x <= right; x++)
            if (grid->cells[(size_t)y * grid->cols + x] & mask)
                return FALSE;
    for (int y = top; y <= bottom; y++)
        for (int x = left; x <= right; x++)
            grid->cells[(size_t)y * grid->cols + x] |= mask;
    return TRUE;
}

static void route_marker(const Canvas *c, HDC dc, RECT view, double scale, size_t i, BOOL selected,
                         Coverage *grid) {
    POINT p = screen_point(c, i, view, scale);
    if (p.x < -8 || p.y < -8 || p.x > view.right + 8 || p.y > view.bottom + 8)
        return;
    int col = max(0, min(view.right - 1, p.x)), row = max(0, min(view.bottom - 1, p.y));
    RECT cell = {col, row, col + 1, row + 1};
    if (!selected && !reserve_box(grid, cell, MARK))
        return;
    int radius = selected ? 7 : 5;
    SetDCBrushColor(dc, selected ? RGB(248, 181, 63) : RGB(255, 255, 255));
    Ellipse(dc, p.x - radius, p.y - radius, p.x + radius + 1, p.y + radius + 1);
    if (!c->labels)
        return;
    wchar_t number[32];
    const wchar_t *label = c->points[i].name;
    if (!label || !*label) {
        StringCchPrintfW(number, ARRAYSIZE(number), L"%zu", i + 1);
        label = number;
    }
    RECT box = {p.x + 8, p.y - 9, view.right - 2, p.y + 11};
    if (grid->cells || selected) {
        SIZE size;
        int length = (int)wcslen(label);
        if (!GetTextExtentPoint32W(dc, label, length, &size))
            return;
        if (selected && box.right - box.left < size.cx && p.x - 8 > box.right - box.left) {
            box.right = p.x - 8;
            box.left = max(2, box.right - size.cx - 1);
        }
        box.right = min(box.right, box.left + size.cx + 1);
        box.top = max(0, box.top);
        box.bottom = min(view.bottom, box.top + 20);
        if (box.right <= box.left || box.bottom <= box.top)
            return;
        if (!selected && !reserve_box(grid, box, LABEL))
            return;
    }
    SetBkMode(dc, OPAQUE);
    SetBkColor(dc, RGB(250, 249, 244));
    SetTextColor(dc, RGB(30, 70, 73));
    DrawTextW(dc, label, -1, &box, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SetBkMode(dc, TRANSPARENT);
}

static void route_overlay(Canvas *c, HDC dc, RECT view, double scale) {
    if (!c->count)
        return;
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(4, 109, 108));
    HGDIOBJ old = SelectObject(dc, pen);
    SelectObject(dc, GetStockObject(DC_BRUSH));
    if (c->lines)
        route_line(c, dc, view, scale);
    Coverage grid = {.cols = (view.right + CELL - 1) / CELL, .rows = (view.bottom + CELL - 1) / CELL};
    if (c->count >= 64 && grid.cols && grid.rows)
        grid.cells = calloc((size_t)grid.cols, (size_t)grid.rows);
    for (size_t i = 0; i < c->count; i++)
        if (i != (size_t)c->selected)
            route_marker(c, dc, view, scale, i, FALSE, &grid);
    if (c->selected >= 0 && (size_t)c->selected < c->count)
        route_marker(c, dc, view, scale, (size_t)c->selected, TRUE, &grid);
    free(grid.cells);
    SelectObject(dc, old);
    DeleteObject(pen);
}
static void render(Canvas *c, HDC dc, RECT r) {
    color_fill(dc, r, RGB(240, 242, 237));
    double scale = ldexp(256., c->zoom), left = c->x * scale - r.right / 2.,
           top = c->y * scale - r.bottom / 2.;
    int tiles = 1 << c->zoom;
    HDC memory = CreateCompatibleDC(dc);
    EnterCriticalSection(&c->lock);
    c->pending = 0;
    for (int y = (int)floor(top / 256.); y <= (int)floor((top + r.bottom) / 256.); y++) {
        if (y < 0 || y >= tiles)
            continue;
        for (int x = (int)floor(left / 256.); x <= (int)floor((left + r.right) / 256.); x++) {
            int sx = (int)round(x * 256. - left), sy = (int)round(y * 256. - top);
            Tile t = {.x = (x % tiles + tiles) % tiles, .y = y, .z = c->zoom};
            BOOL found = FALSE;
            if (c->source) {
                for (int i = 0; i < CACHE_COUNT; i++)
                    if (c->cache[i].valid && same(c->cache[i], t)) {
                        c->cache[i].used = GetTickCount64();
                        if (c->cache[i].bitmap) {
                            HGDIOBJ old = SelectObject(memory, c->cache[i].bitmap);
                            BitBlt(dc, sx, sy, 256, 256, memory, 0, 0, SRCCOPY);
                            SelectObject(memory, old);
                        }
                        found = TRUE;
                        break;
                    }
                if (!found && c->pending < CACHE_COUNT && !(c->busy && same(t, c->active))) {
                    BOOL queued = FALSE;
                    for (int i = 0; i < c->pending; i++)
                        if (same(t, c->jobs[i])) {
                            queued = TRUE;
                            break;
                        }
                    if (!queued)
                        c->jobs[c->pending++] = t;
                }
            }
            if (!found) {
                RECT tile = {sx, sy, sx + 256, sy + 256};
                FrameRect(dc, &tile, GetSysColorBrush(COLOR_3DFACE));
            }
        }
    }
    if (c->pending)
        SetEvent(c->wake);
    LeaveCriticalSection(&c->lock);
    DeleteDC(memory);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    if (!c->source) {
        SetTextColor(dc, RGB(102, 113, 108));
        RECT hint = {8, 8, r.right - 8, r.bottom};
        DrawTextW(dc, L"Offlinekarte über „Offlinekarten…“ öffnen", -1, &hint, DT_LEFT | DT_TOP | DT_SINGLELINE);
    }
    route_overlay(c, dc, r, scale);
    BOOL linked = c->credit_link && c->source && wcscmp(c->source->credit, ORTHOFOTO_CREDIT) == 0;
    wchar_t status[720];
    StringCchPrintfW(status, ARRAYSIZE(status), L"%.5f°, %.5f°   ·   Zoom %d   %ls", map_lat(c->y),
                     map_lon(c->x), c->zoom, c->source && !linked ? c->source->credit : L"");
    RECT strip = {0, r.bottom - (linked ? c->credit_height : 20), r.right, r.bottom};
    color_fill(dc, strip, RGB(251, 251, 248));
    if (linked) {
        RECT link;
        GetWindowRect(c->credit_link, &link);
        strip.right -= link.right - link.left;
    }
    strip.left += 5;
    SetTextColor(dc, RGB(43, 58, 54));
    DrawTextW(dc, status, -1, &strip, DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | DT_END_ELLIPSIS);
}
static void clear_points(Canvas *c) {
    for (size_t i = 0; i < c->count; i++)
        free(c->points[i].name);
    free(c->points);
    c->points = NULL;
    c->count = 0;
}
void map_points(HWND window, const MapPoint *p, size_t n, int selected) {
    Canvas *c = canvas(window);
    if (!c)
        return;
    Marker *copy = calloc(n, sizeof *copy);
    if (n && !copy)
        return;
    for (size_t i = 0; i < n; i++) {
        copy[i].x = map_x(p[i].lon);
        copy[i].y = map_y(p[i].lat);
        copy[i].name = p[i].name ? _wcsdup(p[i].name) : NULL;
    }
    clear_points(c);
    c->points = copy;
    c->count = n;
    c->selected = selected;
    InvalidateRect(window, NULL, FALSE);
}
void map_select(HWND window, int index) {
    Canvas *c = canvas(window);
    if (c && c->selected != index) {
        c->selected = index;
        InvalidateRect(window, NULL, FALSE);
    }
}
void map_center(HWND window, double lat, double lon) {
    Canvas *c = canvas(window);
    if (!c || !isfinite(lat) || !isfinite(lon))
        return;
    c->x = map_x(lon);
    c->y = map_y(lat);
    InvalidateRect(window, NULL, FALSE);
}
void map_fit(HWND window) {
    Canvas *c = canvas(window);
    if (!c || !c->count)
        return;
    double loX = DBL_MAX, hiX = -DBL_MAX, loY = DBL_MAX, hiY = -DBL_MAX, origin = c->points[0].x;
    for (size_t i = 0; i < c->count; i++) {
        double dx = c->points[i].x - origin, x = origin + dx - round(dx), y = c->points[i].y;
        loX = fmin(loX, x);
        hiX = fmax(hiX, x);
        loY = fmin(loY, y);
        hiY = fmax(hiY, y);
    }
    RECT r;
    GetClientRect(window, &r);
    double xScale = fmax(40., r.right - 80.) / fmax(1e-8, hiX - loX),
           yScale = fmax(40., r.bottom - 65.) / fmax(1e-8, hiY - loY);
    unsigned levels = c->source ? c->source->zoom_levels : 0;
    int wanted = (int)fmax(0., fmin(levels ? 20. : 16., floor(log2(fmin(xScale, yScale) / 256.))));
    c->zoom = fit_zoom(levels, wanted);
    c->x = (loX + hiX) / 2.;
    c->x -= floor(c->x);
    c->y = (loY + hiY) / 2.;
    InvalidateRect(window, NULL, FALSE);
}
void map_zoom(HWND window, int delta) {
    Canvas *c = canvas(window);
    if (c) {
        int wanted = (int)fmax(0., fmin(20., c->zoom + delta));
        unsigned levels = c->source ? c->source->zoom_levels : 0;
        c->zoom = levels ? available_zoom(levels, wanted, delta >= 0 ? 1 : -1, c->zoom) : wanted;
        InvalidateRect(window, NULL, FALSE);
    }
}
void map_overlay(HWND window, BOOL lines, BOOL labels) {
    Canvas *c = canvas(window);
    if (c) {
        c->lines = lines;
        c->labels = labels;
        InvalidateRect(window, NULL, FALSE);
    }
}
void map_manage(HWND window, const GvRoute *route) {
    if (canvas(window))
        map_dialog(window, route);
}
static void pan(Canvas *c, int dx, int dy) {
    double scale = ldexp(256., c->zoom);
    c->x -= dx / scale;
    c->x -= floor(c->x);
    c->y = fmax(0., fmin(1., c->y - dy / scale));
    InvalidateRect(c->window, NULL, FALSE);
}
static LRESULT CALLBACK window_proc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    Canvas *c = canvas(window);
    switch (msg) {
    case WM_NCCREATE:
        c = ((CREATESTRUCTW *)lp)->lpCreateParams;
        c->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)c);
        return TRUE;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        layout_credit(c);
        return 0;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == c->credit_link) {
            SetBkColor((HDC)wp, RGB(251, 251, 248));
            SetDCBrushColor((HDC)wp, RGB(251, 251, 248));
            return (LRESULT)GetStockObject(DC_BRUSH);
        }
        break;
    case WM_NOTIFY: {
        const NMLINK *link = (const NMLINK *)lp;
        if (link->hdr.hwndFrom == c->credit_link && (link->hdr.code == NM_CLICK || link->hdr.code == NM_RETURN)) {
            const wchar_t *url = link->item.iLink == 0 ? ORTHOFOTO_PROVIDER : ORTHOFOTO_LICENCE;
            if ((INT_PTR)ShellExecuteW(window, L"open", url, NULL, NULL, SW_SHOWNORMAL) <= 32)
                MessageBoxW(window, url, L"Link im Browser öffnen", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        break;
    }
    case TILE_READY:
        InvalidateRect(window, NULL, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window, &ps);
        RECT r;
        GetClientRect(window, &r);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
        if (bmp && mem) {
            HGDIOBJ old = SelectObject(mem, bmp);
            render(c, mem, r);
            BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
        }
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(window);
        SetCapture(window);
        c->drag = TRUE;
        c->anchor = (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        return 0;
    case WM_LBUTTONUP:
        if (c->drag)
            ReleaseCapture();
        c->drag = FALSE;
        return 0;
    case WM_CAPTURECHANGED:
        c->drag = FALSE;
        return 0;
    case WM_MOUSEMOVE:
        if (c->drag) {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            pan(c, pt.x - c->anchor.x, pt.y - c->anchor.y);
            c->anchor = pt;
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (c->pick) {
            RECT r;
            GetClientRect(window, &r);
            double d = ldexp(256., c->zoom);
            double y = c->y + (GET_Y_LPARAM(lp) - r.bottom / 2.) / d,
                   x = c->x + (GET_X_LPARAM(lp) - r.right / 2.) / d;
            c->pick(map_lat(fmax(0., fmin(1., y))), map_lon(x), c->context);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        RECT r;
        GetClientRect(window, &r);
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(window, &pt);
        int step = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1;
        double before = ldexp(256., c->zoom);
        map_zoom(window, step);
        double after = ldexp(256., c->zoom);
        c->x += (pt.x - r.right / 2.) * (1. / before - 1. / after);
        c->x -= floor(c->x);
        c->y = fmax(0., fmin(1., c->y + (pt.y - r.bottom / 2.) * (1. / before - 1. / after)));
        return 0;
    }
    case WM_KEYDOWN:
        switch (wp) {
        case VK_LEFT:
            pan(c, 70, 0);
            break;
        case VK_RIGHT:
            pan(c, -70, 0);
            break;
        case VK_UP:
            pan(c, 0, 70);
            break;
        case VK_DOWN:
            pan(c, 0, -70);
            break;
        case VK_ADD:
        case VK_OEM_PLUS:
            map_zoom(window, 1);
            break;
        case VK_SUBTRACT:
        case VK_OEM_MINUS:
            map_zoom(window, -1);
            break;
        case VK_HOME:
            map_fit(window);
            break;
        }
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_DESTROY:
        EnterCriticalSection(&c->lock);
        c->stopping = TRUE;
        if (c->source)
            map_vector_cancel(c->source->vector);
        SetEvent(c->wake);
        LeaveCriticalSection(&c->lock);
        if (c->worker) {
            CancelSynchronousIo(c->worker);
            WaitForSingleObject(c->worker, INFINITE);
            CloseHandle(c->worker);
        }
        CloseHandle(c->wake);
        clear_cache(c);
        release(c->source);
        DeleteCriticalSection(&c->lock);
        clear_points(c);
        if (c->gdiplus)
            GdiplusShutdown(c->gdiplus);
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        free(c);
        return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}
HWND map_create(HWND parent, int id, MapPick pick, void *context) {
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW wc = {.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW,
                    .lpfnWndProc = window_proc,
                    .hInstance = instance,
                    .hCursor = LoadCursorW(NULL, IDC_CROSS),
                    .lpszClassName = L"GeoVoiceMapCanvas"};
    RegisterClassW(&wc);
    Canvas *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->pick = pick;
    c->context = context;
    c->x = map_x(16.3738);
    c->y = map_y(48.2082);
    c->zoom = 14;
    c->lines = TRUE;
    c->labels = TRUE;
    InitializeCriticalSection(&c->lock);
    c->wake = CreateEventW(NULL, TRUE, FALSE, NULL);
    GdiplusStartupInput input = {.GdiplusVersion = 1};
    if (!c->wake || GdiplusStartup(&c->gdiplus, &input, NULL) != Ok) {
        if (c->wake)
            CloseHandle(c->wake);
        DeleteCriticalSection(&c->lock);
        free(c);
        return NULL;
    }
    HWND window = CreateWindowExW(WS_EX_CLIENTEDGE, wc.lpszClassName, L"Kartenansicht",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN, 0, 0, 100, 100, parent,
                                  (HMENU)(INT_PTR)id, instance, c);
    if (!window) {
        GdiplusShutdown(c->gdiplus);
        CloseHandle(c->wake);
        DeleteCriticalSection(&c->lock);
        free(c);
        return NULL;
    }
    c->credit_link = CreateWindowExW(0, WC_LINK,
                                     L"Datenquelle: <a href=\"" ORTHOFOTO_PROVIDER L"\">basemap.at</a>"
                                     L" · <a href=\"" ORTHOFOTO_LICENCE L"\">CC BY 4.0</a>",
                                     WS_CHILD | WS_TABSTOP, 0, 0, 1, 1, window, NULL, instance, NULL);
    if (c->credit_link) {
        SendMessageW(c->credit_link, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), FALSE);
        layout_credit(c);
    }
    c->worker = CreateThread(NULL, 0, render_worker, c, 0, NULL);
    if (!c->worker) {
        DestroyWindow(window);
        return NULL;
    }
    wchar_t path[MAP_PATH] = {0}, credit[512] = {0};
    DWORD bytes = sizeof path;
    if (RegGetValueW(HKEY_CURRENT_USER, MAP_REG, L"Path", RRF_RT_REG_SZ, NULL, path, &bytes) ==
        ERROR_SUCCESS) {
        DWORD attrs = GetFileAttributesW(path);
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            bytes = sizeof credit;
            RegGetValueW(HKEY_CURRENT_USER, MAP_REG, L"Credit", RRF_RT_REG_SZ, NULL, credit, &bytes);
            double view[] = {48.2082, 16.3738, 14.};
            bytes = sizeof view;
            RegGetValueW(HKEY_CURRENT_USER, MAP_REG, L"View", RRF_RT_REG_BINARY, NULL, view, &bytes);
            if (isfinite(view[0]) && fabs(view[0]) <= 90 && isfinite(view[1]) && fabs(view[1]) <= 180 &&
                isfinite(view[2]) && view[2] >= 0 && view[2] <= 20)
                map_use_directory(window, path, credit, view[0], view[1], (int)view[2]);
        } else if (attrs != INVALID_FILE_ATTRIBUTES)
            map_use_file(window, path);
    }
    return window;
}
