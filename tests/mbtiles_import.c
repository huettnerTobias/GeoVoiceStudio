#include "mbtiles.h"
#include "map_corridor.h"
#include "map_image.h"
#include "map_internal.h"
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <strsafe.h>

static int (*db_open)(const char *, void **);
static int (*db_exec)(void *, const char *, int (*)(void *, int, char **, char **), void *, char **);
static int (*db_close)(void *);
static BOOL update(const wchar_t *file, const char *sql) {
    char path[4 * MAX_PATH];
    void *db = NULL;
    if (!WideCharToMultiByte(CP_UTF8, 0, file, -1, path, sizeof path, NULL, NULL))
        return FALSE;
    BOOL ok = db_open(path, &db) == 0 && db_exec(db, sql, NULL, NULL, NULL) == 0;
    db_close(db);
    return ok;
}

/* A 256x256 monochrome PNG. The normalized schema exercises a common MBTiles tiles view. */
static const char fixture[] =
    "CREATE TABLE metadata(name,value);"
    "INSERT INTO metadata VALUES('name','basemap-ortho'),('format','png'),('maxzoom','16');"
    "CREATE TABLE images(id INTEGER PRIMARY KEY,data BLOB);"
    "INSERT INTO images VALUES(1,X'89504e470d0a1a0a0000000d4948445200000100000001000100000000740995cb"
    "0000001f4944415478daedc1010d000000c2a0f74f6d0e37a00000000000000000be0d2100000160e49d97"
    "0000000049454e44ae426082');"
    "CREATE TABLE map(zoom_level,tile_column,tile_row,id,PRIMARY KEY(zoom_level,tile_column,tile_row));"
    "INSERT INTO map VALUES(3,4,5,1),(19,285000,345287,1),(19,285004,345283,1),"
    "(19,285004,345284,1),(19,285008,345279,1),(19,285000,345279,1);"
    "CREATE VIEW tiles AS SELECT zoom_level,tile_column,tile_row,data AS tile_data FROM map JOIN images USING(id);";

static void cancel_after_write(void *ctx, int checked, int written) {
    (void)checked;
    if (written)
        InterlockedExchange((volatile LONG *)ctx, 1);
}

#define CHECK(condition) do { if (!(condition)) { \
    fwprintf(stderr, L"mbtiles_import.c:%d: %hs (%ls)\n", __LINE__, #condition, r.error); \
    goto cleanup; \
} } while (0)

int main(void) {
    MbtilesResult r = {0};
    MapCorridorPlan plan = {0};
    LONG cancel = 0;
    int result = 1;
    wchar_t temp[MAX_PATH], root[MAX_PATH + 1] = {0}, unique[40], source[MAX_PATH], stage[MAX_PATH], tile[MAX_PATH];
    BOOL own_root = FALSE;
    HMODULE sql = LoadLibraryExW(L"winsqlite3.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    ULONG_PTR token = 0;
    GdiplusStartupInput init = {.GdiplusVersion = 1};
    GUID guid;
    MapPoint points[2];
    for (int i = 0; i < 2; i++) {
        points[i] = (MapPoint){.lat = map_lat((179000.5 + 8 * i) / (1 << 19)),
                               .lon = map_lon((285000.5 + 8 * i) / (1 << 19))};
    }
    MapPoint across[] = {{.lon = 179.95}, {.lon = -179.95}};
    CHECK(map_corridor_plan(across, 2, 1u << 12, &cancel, &plan, r.error));
    CHECK(plan.count == 12 && plan.tiles[0].x == 4095);
    free(plan.tiles);
    plan.tiles = NULL;
    MapPoint huge[] = {{.lon = -75}, {.lon = 75}};
    CHECK(!map_corridor_plan(huge, 2, 1u << 20, &cancel, &plan, r.error) && !plan.tiles);
    CHECK(sql);
    db_open = (void *)GetProcAddress(sql, "sqlite3_open");
    db_exec = (void *)GetProcAddress(sql, "sqlite3_exec");
    db_close = (void *)GetProcAddress(sql, "sqlite3_close");
    CHECK(db_open && db_exec && db_close);
    CHECK(GdiplusStartup(&token, &init, NULL) == Ok);
    DWORD len = GetTempPathW(MAX_PATH, temp);
    CHECK(len > 0 && len < MAX_PATH && SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(&guid, unique, 40));
    CHECK(SUCCEEDED(StringCchPrintfW(root, MAX_PATH, L"%lsGeoVoice-mbtiles-%ls", temp, unique)));
    CHECK(CreateDirectoryW(root, NULL));
    own_root = TRUE;
    CHECK(SUCCEEDED(StringCchPrintfW(source, MAX_PATH, L"%ls\\Österreich.mbtiles", root)));
    CHECK(update(source, fixture));
    for (int mode = 0; mode < 2; mode++) {
        if (mode)
            CHECK(update(source, "UPDATE map SET tile_row=(1<<zoom_level)-1-tile_row;"
                                 "INSERT INTO metadata VALUES('scheme','xyz'),('attribution','basemap.at – Österreich');"));
        CHECK(SUCCEEDED(StringCchPrintfW(stage, MAX_PATH, L"%ls\\stage%d", root, mode)) && CreateDirectoryW(stage, NULL));
        CHECK(mbtiles_extract(source, stage, points, 2, &cancel, NULL, NULL, &r));
        CHECK(r.written == 5 && r.min_zoom == 3 && r.max_zoom == 19);
        CHECK(!wcscmp(r.source_attribution, mode ? L"basemap.at – Österreich" : L""));
        CHECK(fabs(r.center_lat - points[0].lat) < 1e-8 && fabs(r.center_lon - points[0].lon) < 1e-8);
        CHECK(SUCCEEDED(StringCchPrintfW(tile, MAX_PATH, L"%ls\\19\\285004\\179004.png", stage)) && map_valid_tile(tile));
        CHECK(SUCCEEDED(StringCchPrintfW(tile, MAX_PATH, L"%ls\\19\\285000\\179008.png", stage)) &&
              GetFileAttributesW(tile) == INVALID_FILE_ATTRIBUTES);
    }
    CHECK(SUCCEEDED(StringCchPrintfW(stage, MAX_PATH, L"%ls\\short", root)) && CreateDirectoryW(stage, NULL));
    CHECK(!mbtiles_extract(source, stage, points, 2, &cancel, cancel_after_write, &cancel, &r));
    CHECK(cancel && r.written == 1 && wcsstr(r.error, L"Abgebrochen"));
    cancel = 0;
    MapPoint outside = {.lat = -50., .lon = -130.};
    CHECK(!mbtiles_extract(source, stage, &outside, 1, &cancel, NULL, NULL, &r));
    CHECK(r.written == 0 && wcsstr(r.error, L"keine Kacheln"));
    CHECK(update(source, "UPDATE metadata SET value='Foreign imagery' WHERE name='attribution'"));
    CHECK(!mbtiles_extract(source, stage, points, 2, NULL, NULL, NULL, &r));
    CHECK(r.written == 0 && wcsstr(r.error, L"offizielle"));
    CHECK(update(source, "DELETE FROM metadata WHERE name='attribution';"
                         "UPDATE metadata SET value='Orthofoto' WHERE name='name'"));
    CHECK(!mbtiles_extract(source, stage, points, 2, NULL, NULL, NULL, &r) && wcsstr(r.error, L"offizielle"));
    CHECK(update(source, "UPDATE metadata SET value='basemap.at Orthofoto' WHERE name='name'"));
    CHECK(update(source, "UPDATE images SET data=X'89504e470d0a1a0a'"));
    CHECK(SUCCEEDED(StringCchPrintfW(stage, MAX_PATH, L"%ls\\invalid", root)) && CreateDirectoryW(stage, NULL));
    CHECK(!mbtiles_extract(source, stage, points, 2, &cancel, NULL, NULL, &r) && r.written == 0);
    CHECK(wcsstr(r.error, L"Bild"));
    result = 0;
cleanup:
    free(plan.tiles);
    if (token) GdiplusShutdown(token);
    if (sql) FreeLibrary(sql);
    if (own_root) {
        SHFILEOPSTRUCTW remove = {.wFunc = FO_DELETE, .pFrom = root,
                                  .fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI};
        if (SHFileOperationW(&remove) || remove.fAnyOperationsAborted)
            result = 1;
    }
    return result;
}
