#ifndef GEOVOICE_MAP_ORTHOFOTO_H
#define GEOVOICE_MAP_ORTHOFOTO_H
#include "map_internal.h"
#include "mbtiles.h"

#define ORTHOFOTO_CREDIT L"Datenquelle: basemap.at · CC BY 4.0"
#define ORTHOFOTO_PROVIDER L"https://basemap.at/"
#define ORTHOFOTO_LICENCE L"https://creativecommons.org/licenses/by/4.0/"
#define ORTHOFOTO_PRODUCT L"https://basemap.at/orthofoto-offline/"

/* Publish a complete, uniquely named XYZ folder under the chosen parent. */
BOOL orthofoto_export(const wchar_t *parent_dir, const wchar_t *route_name, BOOL from_mbtiles,
                      volatile LONG *cancel, BOOL (*extract)(void *, const wchar_t *, MbtilesResult *), void *context,
                      wchar_t output[MAP_PATH], MbtilesResult *result);
#endif
