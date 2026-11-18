/* Native reader for Mapsforge's public binary map format, versions 3–5.
 * https://github.com/mapsforge/mapsforge/blob/master/docs/Specification-Binary-Map-File.md */
#include "map_internal.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const unsigned char *p, *end;
    BOOL ok;
} Reader;
typedef struct {
    const unsigned char *bytes;
    unsigned len;
} Slice;
typedef enum { OTHER, LAND, FOREST, WATER, BUILDING, WATERWAY, PATH, ROAD, MAJOR, RAIL } Kind;
typedef struct {
    Kind kind;
    char variable;
} Tag;
typedef struct {
    int base, low, high, left, top, width, height;
    uint64_t start, size;
} Band;
struct MapVector {
    HANDLE file;
    volatile LONG cancelled;
    uint64_t size;
    unsigned char index[4096], *block;
    uint64_t index_start, block_start;
    size_t index_length, block_length, block_capacity;
    BOOL debug;
    double south, west, north, east;
    Tag *tags, *poi_tags;
    unsigned tag_count, poi_count;
    double start_lat, start_lon;
    int start_zoom;
    BOOL has_start;
    int bands;
    Band band[32];
};

static const unsigned char *take(Reader *r, size_t n) {
    if (!r->ok || n > (size_t)(r->end - r->p)) {
        r->ok = FALSE;
        return NULL;
    }
    const unsigned char *p = r->p;
    r->p += n;
    return p;
}
static uint64_t number(Reader *r, unsigned n) {
    const unsigned char *p = take(r, n);
    uint64_t v = 0;
    if (p)
        for (unsigned i = 0; i < n; i++)
            v = (v << 8) | p[i];
    return v;
}
static int64_t variable(Reader *r, BOOL sign) {
    uint64_t v = 0;
    for (unsigned shift = 0; shift < 35 && r->ok; shift += 7) {
        unsigned b = (unsigned)number(r, 1);
        if (b & 128)
            v |= (uint64_t)(b & 127) << shift;
        else {
            v |= (uint64_t)(b & (sign ? 63 : 127)) << shift;
            if (v > UINT32_MAX)
                break;
            return sign && (b & 64) ? -(int64_t)v : (int64_t)v;
        }
    }
    r->ok = FALSE;
    return 0;
}
static unsigned vu(Reader *r) { return (unsigned)variable(r, FALSE); }
static Slice string(Reader *r) {
    unsigned n = vu(r);
    const unsigned char *p = take(r, n);
    Slice s = {p, p ? n : 0};
    return s;
}
static BOOL starts(Slice s, const char *prefix) {
    size_t n = strlen(prefix);
    return n <= s.len && !memcmp(s.bytes, prefix, n);
}
static Tag tag_info(Slice s) {
    Tag t = {OTHER, 0};
    if (starts(s, "highway=")) {
        t.kind = ROAD;
        if (starts(s, "highway=motorway") || starts(s, "highway=trunk") || starts(s, "highway=primary") ||
            starts(s, "highway=secondary"))
            t.kind = MAJOR;
        if (starts(s, "highway=path") || starts(s, "highway=track") || starts(s, "highway=foot") ||
            starts(s, "highway=cycle") || starts(s, "highway=steps"))
            t.kind = PATH;
    } else if (starts(s, "building="))
        t.kind = BUILDING;
    else if (starts(s, "natural=water") || starts(s, "landuse=reservoir") || starts(s, "waterway=riverbank"))
        t.kind = WATER;
    else if (starts(s, "landuse=forest") || starts(s, "natural=wood") || starts(s, "leisure=park"))
        t.kind = FOREST;
    else if (starts(s, "landuse=") || starts(s, "natural=grass") || starts(s, "leisure="))
        t.kind = LAND;
    else if (starts(s, "waterway=") || starts(s, "natural=coastline"))
        t.kind = WATERWAY;
    else if (starts(s, "railway=rail"))
        t.kind = RAIL;
    if (s.len >= 3 && s.bytes[s.len - 3] == '=' && s.bytes[s.len - 2] == '%')
        t.variable = (char)s.bytes[s.len - 1];
    return t;
}
static int index_at(double f, int z) { return (int)fmax(0., fmin((1 << z) - 1., floor(f * (1 << z)))); }
/* Use explicit reads so an unavailable USB or network drive is a normal I/O error. */
static BOOL read_at(MapVector *v, uint64_t offset, unsigned char *out, DWORD count) {
    LARGE_INTEGER pos = {.QuadPart = (LONGLONG)offset};
    DWORD got;
    return offset <= v->size && count <= v->size - offset &&
           SetFilePointerEx(v->file, pos, NULL, FILE_BEGIN) &&
           ReadFile(v->file, out, count, &got, NULL) && got == count;
}
MapVector *map_vector_open(const wchar_t *path) {
    unsigned char *header_bytes = NULL;
    MapVector *v = calloc(1, sizeof *v);
    if (!v)
        return NULL;
    v->file =
        CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    BY_HANDLE_FILE_INFORMATION info;
    LARGE_INTEGER size;
    if (v->file == INVALID_HANDLE_VALUE || GetFileType(v->file) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(v->file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !GetFileSizeEx(v->file, &size) || size.QuadPart < 80 ||
        size.QuadPart > 8LL * 1024 * 1024 * 1024)
        goto fail;
    v->size = (uint64_t)size.QuadPart;
    unsigned char prefix[24];
    if (!read_at(v, 0, prefix, sizeof prefix) || memcmp(prefix, "mapsforge binary OSM", 20))
        goto fail;
    Reader r = {prefix + 20, prefix + sizeof prefix, TRUE};
    uint64_t header = number(&r, 4);
    if (!header || header > v->size - 24 || header > 4 * 1024 * 1024)
        goto fail;
    header_bytes = malloc((size_t)header);
    if (!header_bytes || !read_at(v, 24, header_bytes, (DWORD)header))
        goto fail;
    r = (Reader){header_bytes, header_bytes + header, TRUE};
    unsigned version = (unsigned)number(&r, 4);
    if (version < 3 || version > 5 || number(&r, 8) != v->size)
        goto fail;
    take(&r, 8);
    v->south = (int32_t)number(&r, 4) * 1e-6;
    v->west = (int32_t)number(&r, 4) * 1e-6;
    v->north = (int32_t)number(&r, 4) * 1e-6;
    v->east = (int32_t)number(&r, 4) * 1e-6;
    if (v->south < -90 || v->north > 90 || v->west < -180 || v->east > 180 || v->north < v->south ||
        v->east < v->west || number(&r, 2) != 256)
        goto fail;
    Slice proj = string(&r);
    if (proj.len != 8 || !starts(proj, "Mercator"))
        goto fail;
    unsigned flags = (unsigned)number(&r, 1);
    v->debug = (flags & 128) != 0;
    if (flags & 64) {
        v->start_lat = (int32_t)number(&r, 4) * 1e-6;
        v->start_lon = (int32_t)number(&r, 4) * 1e-6;
        v->has_start = v->start_lat >= v->south && v->start_lat <= v->north && v->start_lon >= v->west &&
                       v->start_lon <= v->east;
    }
    v->start_zoom = (flags & 32) ? (int)number(&r, 1) : -1;
    if (flags & 16)
        string(&r);
    if (flags & 8)
        string(&r);
    if (flags & 4)
        string(&r);
    v->poi_count = (unsigned)number(&r, 2);
    v->poi_tags = calloc(v->poi_count + 1, sizeof(Tag));
    if (!v->poi_tags)
        goto fail;
    for (unsigned i = 0; i < v->poi_count && r.ok; i++) {
        Slice s = string(&r);
        v->poi_tags[i] = tag_info(s);
        /* A named settlement gets a map label. Other POIs still need their variable tags decoded. */
        v->poi_tags[i].kind = starts(s, "place=") ? LAND : OTHER;
    }
    v->tag_count = (unsigned)number(&r, 2);
    v->tags = calloc(v->tag_count + 1, sizeof(Tag));
    if (!v->tags)
        goto fail;
    for (unsigned i = 0; i < v->tag_count && r.ok; i++)
        v->tags[i] = tag_info(string(&r));
    v->bands = (int)number(&r, 1);
    if (!v->bands || v->bands > 32)
        goto fail;
    for (int i = 0; i < v->bands; i++) {
        Band *b = &v->band[i];
        b->base = (int)number(&r, 1);
        b->low = (int)number(&r, 1);
        b->high = (int)number(&r, 1);
        b->start = number(&r, 8);
        b->size = number(&r, 8);
        if (b->base > 20 || b->high > 24 || b->high < b->low || b->start < 24 + header ||
            b->start >= v->size || b->size > v->size - b->start)
            goto fail;
        b->left = index_at(map_x(v->west), b->base);
        b->width = index_at(map_x(v->east), b->base) - b->left + 1;
        b->top = index_at(map_y(v->north), b->base);
        b->height = index_at(map_y(v->south), b->base) - b->top + 1;
        if ((uint64_t)b->width * b->height * 5 + (v->debug ? 16 : 0) > b->size)
            goto fail;
    }
    if (!r.ok)
        goto fail;
    free(header_bytes);
    return v;
fail:
    free(header_bytes);
    map_vector_close(v);
    return NULL;
}
void map_vector_close(MapVector *v) {
    if (!v)
        return;
    free(v->block);
    if (v->file && v->file != INVALID_HANDLE_VALUE)
        CloseHandle(v->file);
    free(v->poi_tags);
    free(v->tags);
    free(v);
}
void map_vector_cancel(MapVector *v) {
    if (v)
        InterlockedExchange(&v->cancelled, 1);
}
static BOOL render_cancelled(MapVector *v) { return InterlockedCompareExchange(&v->cancelled, 0, 0) != 0; }
void map_vector_home(MapVector *v, double *lat, double *lon, int *z) {
    *lat = v->has_start ? v->start_lat : (v->north + v->south) / 2.;
    *lon = v->has_start ? v->start_lon : (v->west + v->east) / 2.;
    double extent = fmax(map_x(v->east) - map_x(v->west), map_y(v->south) - map_y(v->north));
    *z = v->start_zoom >= 0 && v->start_zoom <= 20
             ? v->start_zoom
             : (int)fmax(3., fmin(16., floor(log2(1. / fmax(1e-8, extent)))));
}
static Kind read_tags(Reader *r, const Tag *dictionary, unsigned entries, unsigned count) {
    Tag tags[16];
    Kind k = OTHER;
    for (unsigned i = 0; i < count; i++) {
        unsigned id = vu(r);
        if (id >= entries) {
            r->ok = FALSE;
            return OTHER;
        }
        tags[i] = dictionary[id];
        if (tags[i].kind > k)
            k = tags[i].kind;
    }
    for (unsigned i = 0; i < count; i++)
        switch (tags[i].variable) {
        case 'b':
            take(r, 1);
            break;
        case 'h':
            take(r, 2);
            break;
        case 'f':
        case 'i':
            take(r, 4);
            break;
        case 's':
            string(r);
            break;
        }
    return k;
}
static const COLORREF colors[] = {
    RGB(209, 205, 195), RGB(224, 230, 211), RGB(182, 209, 171), RGB(147, 196, 217), RGB(209, 197, 187),
    RGB(97, 166, 198),  RGB(177, 154, 127), RGB(180, 170, 151), RGB(213, 150, 86),  RGB(135, 130, 134)};
typedef struct {
    HDC dc;
    int z, tx, ty;
    double scale;
    HPEN pen[10];
    int labels;
    RECT used[30];
} Paint;
static int pixel(double n) { return (int)fmax(-100000., fmin(100000., n)); }
static void draw_name(Paint *p, Slice name, POINT pt) {
    if (p->labels >= 30 || !name.len || pt.x < 0 || pt.x > 250 || pt.y < 8 || pt.y > 247)
        return;
    unsigned n = 0;
    while (n < name.len && n < 120 && name.bytes[n] >= 32)
        n++;
    wchar_t label[128];
    int len = MultiByteToWideChar(CP_UTF8, 0, (const char *)name.bytes, (int)n, label, 127);
    if (!len)
        return;
    SIZE s;
    GetTextExtentPoint32W(p->dc, label, len, &s);
    RECT box = {pt.x - s.cx / 2, pt.y - s.cy / 2, pt.x + (s.cx + 1) / 2, pt.y + (s.cy + 1) / 2};
    for (int i = 0; i < p->labels; i++) {
        RECT common;
        if (IntersectRect(&common, &box, &p->used[i]))
            return;
    }
    TextOutW(p->dc, box.left, box.top, label, len);
    p->used[p->labels++] = box;
}
static void way(Reader *r, MapVector *v, Paint *p, double originLat, double originLon, int pass) {
    take(r, 2);
    unsigned layer = (unsigned)number(r, 1);
    Kind k = read_tags(r, v->tags, v->tag_count, layer & 15);
    BOOL area = k == WATER || k == LAND || k == FOREST || k == BUILDING;
    if (!r->ok || k == OTHER || ((pass == 0) != area && pass < 2) ||
        (pass == 2 && (p->z < 13 || (k != ROAD && k != MAJOR))))
        return;
    unsigned f = (unsigned)number(r, 1);
    Slice name = {0};
    if (f & 128)
        name = string(r);
    if (pass == 2 && !name.len)
        return;
    if (f & 64)
        string(r);
    if (f & 32)
        string(r);
    if (f & 16) {
        variable(r, TRUE);
        variable(r, TRUE);
    }
    unsigned blocks = (f & 8) ? vu(r) : 1;
    SelectObject(p->dc, p->pen[k]);
    SelectObject(p->dc, GetStockObject(area ? DC_BRUSH : NULL_BRUSH));
    SetDCBrushColor(p->dc, colors[k]);
    for (unsigned b = 0; b < blocks && r->ok; b++) {
        unsigned rings = vu(r);
        if (!rings || rings > 1024)
            return;
        if (area)
            BeginPath(p->dc);
        for (unsigned ring = 0; ring < rings && r->ok; ring++) {
            unsigned count = vu(r);
            if (count < 2 || count > 65536 || count > (unsigned)(r->end - r->p) / 2) {
                r->ok = FALSE;
                break;
            }
            POINT *points = malloc(count * sizeof *points);
            if (!points) {
                r->ok = FALSE;
                break;
            }
            double lat = originLat, lon = originLon, dy = 0., dx = 0.;
            for (unsigned i = 0; i < count; i++) {
                if ((i & 1023) == 0 && render_cancelled(v)) {
                    r->ok = FALSE;
                    break;
                }
                double a = variable(r, TRUE) * .000001, o = variable(r, TRUE) * .000001;
                if (i > 1 && (f & 4)) {
                    a += dy;
                    o += dx;
                }
                lat += a;
                lon += o;
                dy = a;
                dx = o;
                points[i].x = pixel(map_x(lon) * p->scale - p->tx * 256.);
                points[i].y = pixel(map_y(lat) * p->scale - p->ty * 256.);
            }
            if (r->ok) {
                if (pass == 2)
                    draw_name(p, name, points[count / 2]);
                else if (area)
                    Polygon(p->dc, points, (int)count);
                else
                    Polyline(p->dc, points, (int)count);
            }
            free(points);
        }
        if (area) {
            EndPath(p->dc);
            if (r->ok)
                StrokeAndFillPath(p->dc);
            else
                AbortPath(p->dc);
        }
    }
}
static uint64_t index_value(const unsigned char *p) {
    uint64_t n = p[0];
    for (int i = 1; i < 5; i++)
        n = n * 256 + p[i];
    return n;
}
/* The renderer visits the same block in several paint passes. The index cache
   includes the following entry so that block lengths need no separate seek. */
static const unsigned char *index_bytes(MapVector *v, uint64_t offset, unsigned count) {
    if (offset < v->index_start || offset - v->index_start + count > v->index_length) {
        size_t length = (size_t)min(sizeof v->index, v->size - offset);
        v->index_length = 0;
        if (length < count || !read_at(v, offset, v->index, (DWORD)length))
            return NULL;
        v->index_start = offset;
        v->index_length = length;
    }
    return v->index + (size_t)(offset - v->index_start);
}
static const unsigned char *block_bytes(MapVector *v, uint64_t offset, size_t count) {
    if (v->block_length && v->block_start == offset && v->block_length == count)
        return v->block;
    v->block_length = 0;
    if (count > v->block_capacity) {
        unsigned char *data = realloc(v->block, count);
        if (!data)
            return NULL;
        v->block = data;
        v->block_capacity = count;
    }
    if (!read_at(v, offset, v->block, (DWORD)count))
        return NULL;
    v->block_start = offset;
    v->block_length = count;
    return v->block;
}
static BOOL block(MapVector *v, Band *b, Paint *p, int x, int y, int pass) {
    uint64_t idx = (uint64_t)(y - b->top) * b->width + x - b->left, total = (uint64_t)b->height * b->width;
    uint64_t first = v->debug ? 16 : 0;
    const unsigned char *entry = index_bytes(v, b->start + first + idx * 5, idx + 1 == total ? 5 : 10);
    if (!entry)
        return FALSE;
    uint64_t item = index_value(entry), start = item & 0x7fffffffffULL,
             end = idx + 1 == total ? b->size : index_value(entry + 5) & 0x7fffffffffULL;
    double d = (double)(1 << b->base), tile = p->scale / d;
    RECT rect = {pixel(x * tile - p->tx * 256.), pixel(y * tile - p->ty * 256.),
                 pixel((x + 1) * tile - p->tx * 256.), pixel((y + 1) * tile - p->ty * 256.)};
    if (pass == 0 && (item >> 39)) {
        SetDCBrushColor(p->dc, colors[WATER]);
        FillRect(p->dc, &rect, GetStockObject(DC_BRUSH));
    }
    if (start == end)
        return TRUE;
    if (start < first + total * 5 || start > end || end > b->size || end - start > 16 * 1024 * 1024)
        return FALSE;
    size_t length = (size_t)(end - start);
    const unsigned char *data = block_bytes(v, b->start + start, length);
    if (!data)
        return FALSE;
    Reader r = {data, data + length, TRUE};
    if (v->debug)
        take(&r, 32);
    uint64_t ways = 0, pois = 0;
    for (int zoom = b->low; zoom <= b->high; zoom++) {
        unsigned np = vu(&r), nw = vu(&r);
        if (zoom <= p->z || zoom == b->low) {
            ways += nw;
            pois += np;
        }
    }
    unsigned offset = vu(&r);
    const unsigned char *poi_data = take(&r, offset);
    double originLat = map_lat(y / d), originLon = 360. * x / d - 180.;
    if (pass == 2) {
        if (!poi_data)
            return FALSE;
        Reader pr = {poi_data, poi_data + offset, TRUE};
        for (uint64_t i = 0; i < pois && pr.ok && p->labels < 30; i++) {
            if ((i & 255) == 0 && render_cancelled(v))
                return FALSE;
            if (v->debug)
                take(&pr, 32);
            double lat = originLat + variable(&pr, TRUE) * 1e-6, lon = originLon + variable(&pr, TRUE) * 1e-6;
            unsigned count = (unsigned)number(&pr, 1) & 15;
            Kind kind = read_tags(&pr, v->poi_tags, v->poi_count, count);
            unsigned flags = (unsigned)number(&pr, 1);
            Slice name = {0};
            if (flags & 128)
                name = string(&pr);
            if (flags & 64)
                string(&pr);
            if (flags & 32)
                variable(&pr, TRUE);
            if (pr.ok && kind != OTHER && name.len) {
                POINT point = {pixel(map_x(lon) * p->scale - p->tx * 256.),
                               pixel(map_y(lat) * p->scale - p->ty * 256.)};
                draw_name(p, name, point);
            }
        }
        return pr.ok;
    }
    int state = SaveDC(p->dc);
    IntersectClipRect(p->dc, rect.left, rect.top, rect.right, rect.bottom);
    for (uint64_t i = 0; i < ways && r.ok; i++) {
        if ((i & 255) == 0 && render_cancelled(v)) {
            r.ok = FALSE;
            break;
        }
        if (v->debug)
            take(&r, 32);
        unsigned n = vu(&r);
        const unsigned char *record = take(&r, n);
        if (!record)
            break;
        Reader wr = {record, record + n, TRUE};
        way(&wr, v, p, originLat, originLon, pass == 3 ? 2 : pass);
    }
    RestoreDC(p->dc, state);
    return r.ok;
}
BOOL map_vector_tile(MapVector *v, HDC dc, int z, int tx, int ty) {
    Paint p = {.dc = dc, .z = z, .tx = tx, .ty = ty, .scale = ldexp(256., z)};
    Band *b = &v->band[0];
    for (int i = 0; i < v->bands; i++)
        if (z >= v->band[i].low)
            b = &v->band[i];
    double ratio = ldexp(1., b->base - z);
    int x1 = (int)fmax(b->left, floor(tx * ratio)), y1 = (int)fmax(b->top, floor(ty * ratio));
    int x2 = (int)fmin(b->left + b->width - 1, ceil((tx + 1) * ratio) - 1),
        y2 = (int)fmin(b->top + b->height - 1, ceil((ty + 1) * ratio) - 1);
    if (x1 > x2 || y1 > y2 || (int64_t)(x2 - x1 + 1) * (y2 - y1 + 1) > 4096)
        return TRUE;
    int state = SaveDC(dc);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(73, 68, 62));
    SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    for (int k = 0; k < 10; k++)
        p.pen[k] = CreatePen(PS_SOLID, k == MAJOR ? 3 : (k == ROAD && z >= 15 ? 2 : 1), colors[k]);
    BOOL ok = TRUE;
    for (int pass = 0; pass < (z >= 13 ? 4 : 3) && ok; pass++)
        for (int y = y1; y <= y2 && ok; y++)
            for (int x = x1; x <= x2 && ok; x++)
                ok = !render_cancelled(v) && block(v, b, &p, x, y, pass);
    RestoreDC(dc, state);
    for (int k = 0; k < 10; k++)
        DeleteObject(p.pen[k]);
    return ok;
}
