#include "map_orthofoto.h"
#include "map_corridor.h"
#include "map_image.h"
#include <shellapi.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>

static const unsigned char tile[] = {
    137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,1,0,0,0,1,0,1,0,0,0,0,116,9,149,203,
    0,0,0,31,73,68,65,84,120,218,237,193,1,13,0,0,0,194,160,247,79,109,14,55,160,0,0,0,0,0,
    0,0,0,190,13,33,0,0,1,96,228,157,151,0,0,0,0,73,69,78,68,174,66,96,130
};

static BOOL ensure_dir(const wchar_t *path) {
    return CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

/* Set a known timestamp to detect any attempted replacement of an existing tile. */
static BOOL seed(const wchar_t *dir, CorridorTile t, wchar_t path[MAP_PATH], FILETIME stamp) {
    if (FAILED(StringCchPrintfW(path, MAP_PATH, L"%ls\\%d", dir, t.zoom)) || !ensure_dir(path) ||
        FAILED(StringCchPrintfW(path, MAP_PATH, L"%ls\\%d\\%d", dir, t.zoom, t.x)) || !ensure_dir(path) ||
        FAILED(StringCchPrintfW(path, MAP_PATH, L"%ls\\%d\\%d\\%d.jpg", dir, t.zoom, t.x, t.y)))
        return FALSE;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return FALSE;
    DWORD size;
    BOOL ok = WriteFile(file, tile, sizeof tile, &size, NULL) && size == sizeof tile &&
              SetFileTime(file, NULL, NULL, &stamp);
    CloseHandle(file);
    return ok;
}
static BOOL contains_text(const wchar_t *dir, const wchar_t *name, const char *value) {
    wchar_t path[MAP_PATH];
    char content[2048];
    if (FAILED(StringCchPrintfW(path, MAP_PATH, L"%ls\\%ls", dir, name)))
        return FALSE;
    FILE *f = _wfopen(path, L"rb");
    if (!f)
        return FALSE;
    size_t size = fread(content, 1, sizeof content - 1, f);
    BOOL ok = !ferror(f);
    fclose(f);
    content[size] = 0;
    return ok && strstr(content, value);
}
#define CHECK(e) do { if (!(e)) { fwprintf(stderr, L"line %d: %hs (%ls)\n", __LINE__, #e, r.error); goto done; } } while (0)

int main(void) {
    MbtilesResult r = {0};
    MapCorridorPlan plan = {0};
    GUID guid;
    wchar_t temp[MAX_PATH], unique[40], root[MAX_PATH + 1] = {0}, dir[MAP_PATH], path[MAP_PATH];
    LONG cancel = 0;
    MapPoint outside = {.lat = 0, .lon = 0}, vienna = {.lat = 48.20835, .lon = 16.37309};
    int rc = 1;
    BOOL own_root = FALSE;
    HANDLE busy = INVALID_HANDLE_VALUE;
    ULONG_PTR token = 0;
    GdiplusStartupInput init = {.GdiplusVersion = 1};
    DWORD n = GetTempPathW(MAX_PATH, temp);
    FILETIME stamp = {.dwLowDateTime = 0x11110000, .dwHighDateTime = 0x01d40000};
    WIN32_FILE_ATTRIBUTE_DATA file_info;
    CHECK(n && n < MAX_PATH && SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(&guid, unique, 40));
    CHECK(SUCCEEDED(StringCchPrintfW(root, MAX_PATH, L"%lsGeoVoice-cache-test-%ls", temp, unique)));
    CHECK(CreateDirectoryW(root, NULL));
    own_root = TRUE;
    CHECK(GdiplusStartup(&token, &init, NULL) == Ok);
    CHECK(SUCCEEDED(StringCchPrintfW(dir, MAP_PATH, L"%ls\\route", root)));
    CHECK(!orthofoto_cache_update(dir, &outside, 1, &cancel, &r) && wcsstr(r.error, L"Abdeckung"));
    CHECK(contains_text(dir, L"attribution.txt", "Datenquelle: basemap.at"));
    CHECK(contains_text(dir, L"LICENSE.txt", "https://creativecommons.org/licenses/by/4.0/"));
    CHECK(map_corridor_plan(&vienna, 1, (1u << 20) - (1u << 12), &cancel, &plan, r.error));
    for (size_t i = 0; i < plan.count; ++i)
        CHECK(seed(dir, plan.tiles[i], path, stamp));
    CHECK(map_valid_tile(path));
    CHECK(orthofoto_cache_update(dir, &vienna, 1, &cancel, &r));
    CHECK(r.written == (int)plan.count && r.max_zoom == 19 && r.center_lat == vienna.lat);
    CHECK(contains_text(dir, L"zoom.txt", "19") && contains_text(dir, L"center.txt", "48.20835000,16.37309000"));
    CHECK(orthofoto_cache_update(dir, &vienna, 1, &cancel, &r));
    CHECK(GetFileAttributesExW(path, GetFileExInfoStandard, &file_info));
    CHECK(CompareFileTime(&stamp, &file_info.ftLastWriteTime) == 0);
    cancel = 1;
    CHECK(!orthofoto_cache_update(dir, &vienna, 1, &cancel, &r));
    cancel = 0;
    CHECK(SUCCEEDED(StringCchPrintfW(path, MAP_PATH, L"%ls\\.geovoice-download.lock", dir)));
    busy = CreateFileW(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(busy != INVALID_HANDLE_VALUE && !orthofoto_cache_update(dir, &vienna, 1, &cancel, &r));
    CHECK(wcsstr(r.error, L"belegt"));
    /* The caller's directory also contains 'route' and must not become a cache. */
    CHECK(!orthofoto_cache_update(root, &vienna, 1, &cancel, &r) && wcsstr(r.error, L"leeren Ordner"));
    rc = 0;
done:
    if (busy != INVALID_HANDLE_VALUE) CloseHandle(busy);
    free(plan.tiles);
    if (token) GdiplusShutdown(token);
    if (own_root) {
        SHFILEOPSTRUCTW remove = {.wFunc = FO_DELETE, .pFrom = root,
                                  .fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI};
        if (SHFileOperationW(&remove) || remove.fAnyOperationsAborted)
            rc = 1;
    }
    return rc;
}
