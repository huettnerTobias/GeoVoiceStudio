#ifndef GEOVOICE_MBTILES_H
#define GEOVOICE_MBTILES_H
#include "map.h"

typedef struct {
    int written, min_zoom, max_zoom;
    double center_lat, center_lon;
    wchar_t source_attribution[2048], error[512];
} MbtilesResult;

/* The caller owns the empty stage and discards it on failure or cancellation. */
BOOL mbtiles_extract(const wchar_t *source, const wchar_t *stage, const MapPoint *points, size_t count,
                     volatile LONG *cancel, void (*progress)(void *context, int checked, int written),
                     void *context, MbtilesResult *result);
#endif
