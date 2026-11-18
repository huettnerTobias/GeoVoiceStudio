#ifndef GEOVOICE_MAP_INTERNAL_H
#define GEOVOICE_MAP_INTERNAL_H
#include "map.h"
#include <math.h>
#include <stdint.h>
#define MAP_PI 3.14159265358979323846
#define MAP_CREDIT L"© OpenStreetMap contributors · openstreetmap.org/copyright"
#define MAP_PATH 2048

/* Normalized Web Mercator coordinates, independent of pixel size and zoom. */
static inline double map_x(double lon) { return (lon + 180.) / 360.; }
static inline double map_y(double lat) {
    double r = fmax(-85.05112878, fmin(85.05112878,lat)) * MAP_PI / 180.;
    return (1. - asinh(tan(r)) / MAP_PI) / 2.;
}
static inline double map_lat(double y) { return atan(sinh(MAP_PI * (1.-2.*y))) * 180. / MAP_PI; }
static inline double map_lon(double x) { return (x-floor(x))*360.-180.; }

typedef struct MapVector MapVector;
MapVector *map_vector_open(const wchar_t *path);
void map_vector_close(MapVector *v);
void map_vector_home(MapVector *v, double *lat,double *lon,int *z);
void map_vector_tile(MapVector *v,HDC dc,int z,int x,int y);
BOOL map_use_file(HWND canvas,const wchar_t *path);
BOOL map_use_directory(HWND canvas,const wchar_t *path,const wchar_t *credit,double lat,double lon,int z);
void map_current(HWND canvas,wchar_t *path,size_t cap,double *lat,double *lon);
BOOL map_valid_tile(const wchar_t *path);
void map_dialog(HWND canvas);
#endif
