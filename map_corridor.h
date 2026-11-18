#ifndef GEOVOICE_MAP_CORRIDOR_H
#define GEOVOICE_MAP_CORRIDOR_H
#include "map.h"

typedef struct {
    int zoom, x, y;
} CorridorTile;
typedef struct {
    CorridorTile *tiles;
    size_t count;
} MapCorridorPlan;

/* Select levels 0..20 with bits of zoom_mask. The caller frees out->tiles on success. */
BOOL map_corridor_plan(const MapPoint *points, size_t count, unsigned zoom_mask, volatile LONG *cancel,
                       MapCorridorPlan *out, wchar_t error[512]);
#endif
