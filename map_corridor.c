#include "map_corridor.h"
#include "map_internal.h"
#include <stdlib.h>
#include <strsafe.h>

#define TILE_LIMIT 50000
#define HASH_SIZE 131072
typedef struct {
    MapCorridorPlan plan;
    int *slots;
    unsigned char *expanded;
} Builder;

/* Open addressing; a slot stores the index plus one (zero denotes an empty slot). */
static int add(Builder *b, int zoom, int x, int y) {
    uint32_t hash = (uint32_t)x * 0x9e3779b1u ^ (uint32_t)y * 0x85ebca6bu ^ (uint32_t)zoom * 0xc2b2ae35u;
    unsigned at = (hash ^ (hash >> 16)) & (HASH_SIZE - 1);
    while (b->slots[at]) {
        int i = b->slots[at] - 1;
        CorridorTile t = b->plan.tiles[i];
        if (t.x == x && t.y == y && t.zoom == zoom)
            return i;
        at = (at + 1) & (HASH_SIZE - 1);
    }
    if (b->plan.count == TILE_LIMIT)
        return -1;
    int i = (int)b->plan.count++;
    b->plan.tiles[i] = (CorridorTile){zoom, x, y};
    b->slots[at] = i + 1;
    return i;
}

static BOOL add_strip(Builder *b, int zoom, int x, int y) {
    int width = 1 << zoom;
    int center = add(b, zoom, (unsigned)x & (width - 1), y);
    if (center < 0)
        return FALSE;
    if (b->expanded[center])
        return TRUE;
    b->expanded[center] = 1;
    for (int row = y - 1; row <= y + 1; row++) {
        if (row < 0 || row >= width)
            continue;
        for (int col = x - 1; col <= x + 1; col++)
            if (add(b, zoom, (unsigned)col & (width - 1), row) < 0)
                return FALSE;
    }
    return TRUE;
}

/* Visit crossed cells in projection space, choosing the shorter path across the date line. */
static BOOL segment(Builder *b, int zoom, MapPoint a, MapPoint c, volatile LONG *cancel) {
    int width = 1 << zoom;
    double ax = map_x(a.lon) * width, bx = map_x(c.lon) * width;
    double last = nextafter((double)width, 0.);
    double ay = fmax(0., fmin(last, map_y(a.lat) * width));
    double by = fmax(0., fmin(last, map_y(c.lat) * width));
    if (bx - ax > width / 2.)
        bx -= width;
    else if (bx - ax < -width / 2.)
        bx += width;
    int x = (int)floor(ax), y = (int)floor(ay), end_x = (int)floor(bx), end_y = (int)floor(by);
    int step_x = bx > ax ? 1 : -1, step_y = by > ay ? 1 : -1;
    double dx = bx - ax, dy = by - ay;
    double tx = dx ? ((step_x > 0 ? x + 1. : x) - ax) / dx : INFINITY;
    double ty = dy ? ((step_y > 0 ? y + 1. : y) - ay) / dy : INFINITY;
    double jump_x = dx ? fabs(1. / dx) : INFINITY, jump_y = dy ? fabs(1. / dy) : INFINITY;
    for (;;) {
        if (InterlockedCompareExchange(cancel, 0, 0) || !add_strip(b, zoom, x, y))
            return FALSE;
        if (x == end_x && y == end_y)
            return TRUE;
        if (y == end_y || (x != end_x && tx <= ty)) {
            x += step_x;
            tx += jump_x;
        } else {
            y += step_y;
            ty += jump_y;
        }
    }
}

BOOL map_corridor_plan(const MapPoint *points, size_t count, unsigned zoom_mask, volatile LONG *cancel,
                       MapCorridorPlan *out, wchar_t error[512]) {
    *out = (MapCorridorPlan){0};
    LONG local_cancel = 0;
    if (!cancel)
        cancel = &local_cancel;
    if (!points || !count || !(zoom_mask & 0x1fffffu)) {
        StringCchCopyW(error, 512, L"Routenpunkte und unterstützte Zoomstufen erforderlich.");
        return FALSE;
    }
    for (size_t i = 0; i < count; i++) {
        if (!isfinite(points[i].lat) || !isfinite(points[i].lon) ||
            fabs(points[i].lat) > 90. || fabs(points[i].lon) > 180.) {
            StringCchCopyW(error, 512, L"Ungültige Koordinaten in der Route.");
            return FALSE;
        }
    }
    Builder b = {.slots = calloc(HASH_SIZE, sizeof(int)), .expanded = calloc(TILE_LIMIT, 1)};
    b.plan.tiles = malloc(TILE_LIMIT * sizeof *b.plan.tiles);
    BOOL ok = b.plan.tiles && b.slots && b.expanded;
    if (!ok)
        StringCchCopyW(error, 512, L"Nicht genügend Arbeitsspeicher.");
    for (int z = 0; ok && z <= 20; z++) {
        if (!(zoom_mask & (1u << z)))
            continue;
        for (size_t i = 0; i < count; i++) {
            if (!segment(&b, z, points[i ? i - 1 : 0], points[i], cancel)) {
                StringCchCopyW(error, 512, InterlockedCompareExchange(cancel, 0, 0) ? L"Abgebrochen." :
                               L"Der Routenkorridor überschreitet 50.000 Kacheln. Bitte die Route aufteilen.");
                ok = FALSE;
                break;
            }
        }
    }
    free(b.slots);
    free(b.expanded);
    if (ok)
        *out = b.plan;
    else
        free(b.plan.tiles);
    return ok;
}
