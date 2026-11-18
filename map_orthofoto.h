#ifndef GEOVOICE_MAP_ORTHOFOTO_H
#define GEOVOICE_MAP_ORTHOFOTO_H
#include "map_internal.h"
#include "mbtiles.h"

#define ORTHOFOTO_CREDIT L"Datenquelle: basemap.at · CC BY 4.0"
#define ORTHOFOTO_PROVIDER L"https://basemap.at/"
#define ORTHOFOTO_LICENCE L"https://creativecommons.org/licenses/by/4.0/"
#define ORTHOFOTO_PRODUCT L"https://basemap.at/orthofoto-offline/"

BOOL orthofoto_license_text(wchar_t *text, size_t capacity, BOOL from_mbtiles, const wchar_t *source_credit);
/* Extend the dedicated route directory in place. A cancelled or interrupted run keeps completed tiles. */
BOOL orthofoto_cache_update(const wchar_t *directory, const MapPoint *points, size_t count,
                            volatile LONG *cancel, MbtilesResult *result);

/* Publish a complete, uniquely named XYZ folder under the chosen parent. */
BOOL orthofoto_export(const wchar_t *parent_dir, const wchar_t *route_name, BOOL from_mbtiles,
                      volatile LONG *cancel, BOOL (*extract)(void *, const wchar_t *, MbtilesResult *), void *context,
                      wchar_t output[MAP_PATH], MbtilesResult *result);
#endif
