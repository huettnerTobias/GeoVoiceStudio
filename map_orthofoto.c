#include "map_orthofoto.h"
#include <objbase.h>
#include <strsafe.h>
#include <stdlib.h>

/* Only called on the new GUID staging folder. Do not traverse junctions or unexpected nesting. */
static BOOL remove_stage(const wchar_t *dir, int depth) {
    HANDLE guard = map_hold_directory(dir, FALSE);
    if (guard == INVALID_HANDLE_VALUE)
        return FALSE;
    wchar_t glob[MAP_PATH], path[MAP_PATH];
    WIN32_FIND_DATAW item;
    BOOL ok = SUCCEEDED(StringCchPrintfW(glob, MAP_PATH, L"%ls\\*", dir));
    HANDLE scan = ok ? FindFirstFileW(glob, &item) : INVALID_HANDLE_VALUE;
    if (scan != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(item.cFileName, L".") || !wcscmp(item.cFileName, L".."))
                continue;
            if (FAILED(StringCchPrintfW(path, MAP_PATH, L"%ls\\%ls", dir, item.cFileName))) {
                ok = FALSE;
                break;
            }
            if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    ok &= RemoveDirectoryW(path);
                else
                    ok &= depth < 2 && remove_stage(path, depth + 1);
            } else
                ok &= DeleteFileW(path);
        } while (FindNextFileW(scan, &item));
        FindClose(scan);
    }
    CloseHandle(guard);
    return ok && RemoveDirectoryW(dir);
}

static BOOL sidecar(const wchar_t *dir, const wchar_t *name, const wchar_t *text) {
    wchar_t path[MAP_PATH];
    if (FAILED(StringCchPrintfW(path, MAP_PATH, L"%ls\\%ls", dir, name)))
        return FALSE;
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, NULL, 0, NULL, NULL);
    char *utf8 = size > 0 ? malloc((size_t)size) : NULL;
    BOOL ok = utf8 && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, utf8, size, NULL, NULL);
    if (ok) {
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD written = 0;
        ok = file != INVALID_HANDLE_VALUE && WriteFile(file, utf8, (DWORD)size - 1, &written, NULL) &&
             written == (DWORD)size - 1 && FlushFileBuffers(file);
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
    }
    free(utf8);
    return ok;
}

BOOL orthofoto_license_text(wchar_t *text, size_t capacity, BOOL from_mbtiles, const wchar_t *source_credit) {
    return SUCCEEDED(StringCchPrintfW(text, capacity,
            L"Datenquelle: basemap.at\r\n" ORTHOFOTO_PROVIDER L"\r\n"
            L"Produkt: basemap.at Orthofoto Österreich (EPSG:3857)\r\n%ls\r\n"
            L"Lizenz: Creative Commons Namensnennung 4.0 International (CC BY 4.0)\r\n"
            ORTHOFOTO_LICENCE L"\r\nBearbeitung: Kartenausschnitt entlang der Route als XYZ-Kacheln exportiert.\r\n"
            L"%ls%ls", from_mbtiles ? ORTHOFOTO_PRODUCT : L"https://basemap.at/orthofoto/",
            *source_credit ? L"Quellenangabe im Originalpaket: " : L"", source_credit));
}

static BOOL write_credits(const wchar_t *stage, BOOL from_mbtiles, const MbtilesResult *result) {
    wchar_t license[3072], center[82], zoom[12];
    StringCchPrintfW(center, ARRAYSIZE(center), L"%.8f,%.8f", result->center_lat, result->center_lon);
    StringCchPrintfW(zoom, ARRAYSIZE(zoom), L"%d", result->max_zoom);
    if (!orthofoto_license_text(license, ARRAYSIZE(license), from_mbtiles, result->source_attribution))
        return FALSE;
    return sidecar(stage, L"LICENSE.txt", license) && sidecar(stage, L"attribution.txt", ORTHOFOTO_CREDIT) &&
           sidecar(stage, L"center.txt", center) && sidecar(stage, L"zoom.txt", zoom);
}

static BOOL publish(const wchar_t *stage, const wchar_t *parent, const wchar_t *route, wchar_t out[MAP_PATH]) {
    wchar_t name[81];
    size_t n = 0;
    while (route[n] && n < ARRAYSIZE(name) - 1) {
        wchar_t ch = route[n];
        name[n++] = (ch < 32 || wcschr(L"<>:\"/\\|?*", ch)) ? L'_' : ch;
    }
    while (n && (name[n - 1] == L'.' || name[n - 1] == L' ' ||
                 (name[n - 1] >= 0xd800 && name[n - 1] <= 0xdbff)))
        --n;
    name[n] = 0;
    for (unsigned i = 1; i <= 9999; ++i) {
        wchar_t suffix[20] = L"";
        if (i > 1)
            StringCchPrintfW(suffix, ARRAYSIZE(suffix), L" (%u)", i);
        if (FAILED(StringCchPrintfW(out, MAP_PATH, L"%ls\\Orthofoto-%ls%ls", parent,
                                   n ? name : L"Route", suffix)))
            break;
        /* MoveFileEx can report ACCESS_DENIED for an existing directory on some file systems. */
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES)
            continue;
        if (MoveFileExW(stage, out, MOVEFILE_WRITE_THROUGH))
            return TRUE;
        if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES)
            break;
    }
    *out = 0;
    return FALSE;
}

BOOL orthofoto_export(const wchar_t *parent_dir, const wchar_t *route_name, BOOL from_mbtiles,
                      volatile LONG *cancel, BOOL (*extract)(void *, const wchar_t *, MbtilesResult *), void *context,
                      wchar_t output[MAP_PATH], MbtilesResult *result) {
    *output = 0;
    *result = (MbtilesResult){0};
    wchar_t stage[MAP_PATH], id[40];
    GUID guid;
    HANDLE parent = map_hold_directory(parent_dir, FALSE);
    if (parent == INVALID_HANDLE_VALUE) {
        StringCchCopyW(result->error, ARRAYSIZE(result->error), L"Der Zielordner ist nicht zugänglich.");
        return FALSE;
    }
    BOOL created = SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(&guid, id, ARRAYSIZE(id)) &&
                   SUCCEEDED(StringCchPrintfW(stage, MAP_PATH, L"%ls\\.GeoVoice-%ls.tmp", parent_dir, id)) &&
                   CreateDirectoryW(stage, NULL);
    BOOL ok = FALSE;
    if (!created)
        StringCchCopyW(result->error, ARRAYSIZE(result->error), L"Der Arbeitsordner konnte nicht angelegt werden.");
    else {
        HANDLE guard = map_hold_directory(stage, FALSE);
        if (guard == INVALID_HANDLE_VALUE)
            StringCchCopyW(result->error, ARRAYSIZE(result->error), L"Der Arbeitsordner ist nicht zugänglich.");
        else {
            ok = extract(context, stage, result);
            if (ok && !write_credits(stage, from_mbtiles, result)) {
                ok = FALSE;
                StringCchCopyW(result->error, ARRAYSIZE(result->error), L"Kartenhinweise konnten nicht gespeichert werden.");
            }
            CloseHandle(guard);
        }
        if (ok && cancel && InterlockedCompareExchange(cancel, 0, 0)) {
            ok = FALSE;
            StringCchCopyW(result->error, ARRAYSIZE(result->error), L"Import abgebrochen.");
        }
        if (ok && !publish(stage, parent_dir, route_name, output)) {
            ok = FALSE;
            StringCchCopyW(result->error, ARRAYSIZE(result->error), L"Der Kartenordner konnte nicht veröffentlicht werden.");
        }
        if (!ok && !remove_stage(stage, 0))
            StringCchCatW(result->error, ARRAYSIZE(result->error), L" Der temporäre Ordner konnte nicht vollständig entfernt werden.");
    }
    CloseHandle(parent);
    return ok;
}
