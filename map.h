#ifndef GEOVOICE_MAP_H
#define GEOVOICE_MAP_H
#include <windows.h>
#include <stddef.h>
#include "route_io.h"

typedef struct {
    double lat, lon;
    const wchar_t *name;
} MapPoint;
typedef void (*MapPick)(double lat, double lon, void *context);
HWND map_create(HWND parent, int id, MapPick pick, void *context);
void map_points(HWND canvas, const MapPoint *points, size_t count, int selected);
void map_select(HWND canvas, int index);
void map_center(HWND canvas, double lat, double lon);
void map_get_center(HWND canvas, double *lat, double *lon);
void map_fit(HWND canvas);
void map_zoom(HWND canvas, int delta);
void map_overlay(HWND canvas, BOOL lines, BOOL labels);
void map_manage(HWND canvas, const GvRoute *route);
/* A background update preserves the viewport and respects a manually selected map. */
void map_show_auto_directory(HWND canvas, const wchar_t *path, BOOL explicit_selection,
                             double latitude, double longitude);
void map_leave_auto_directory(HWND canvas);
#endif
