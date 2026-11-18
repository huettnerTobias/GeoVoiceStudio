#ifndef GEOVOICE_MAP_IMAGE_H
#define GEOVOICE_MAP_IMAGE_H
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

GpImage *map_tile_image(const wchar_t *path);
BOOL map_valid_tile(const wchar_t *path);
#endif
