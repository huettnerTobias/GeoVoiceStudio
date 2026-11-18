#include "../map_orthofoto.h"
#include <objbase.h>
#include <shellapi.h>
#include <stdio.h>
#include <strsafe.h>

typedef struct {
    volatile LONG cancel;
    BOOL cancel_after_write;
    wchar_t staging[MAP_PATH];
} Fixture;

static BOOL make_fixture(void *context, const wchar_t *stage, MbtilesResult *result) {
    Fixture *f = context;
    StringCchCopyW(f->staging, MAP_PATH, stage);
    wchar_t path[MAP_PATH];
    if (FAILED(StringCchPrintfW(path, MAP_PATH, L"%ls\\fixture", stage)))
        return FALSE;
    FILE *file = _wfopen(path, L"wb");
    if (!file || fclose(file))
        return FALSE;
    result->written = 1;
    result->min_zoom = result->max_zoom = 19;
    result->center_lat = 48.25;
    result->center_lon = 16.125;
    StringCchCopyW(result->source_attribution, ARRAYSIZE(result->source_attribution), L"Originalhinweis");
    if (f->cancel_after_write)
        InterlockedExchange(&f->cancel, 1);
    return TRUE;
}

static BOOL contains_text(const wchar_t *directory, const wchar_t *name, const char *text) {
    wchar_t path[MAP_PATH];
    char buffer[4096];
    if (FAILED(StringCchPrintfW(path, MAP_PATH, L"%ls\\%ls", directory, name)))
        return FALSE;
    FILE *file = _wfopen(path, L"rb");
    if (!file)
        return FALSE;
    size_t n = fread(buffer, 1, sizeof(buffer) - 1, file);
    BOOL ok = feof(file) && !ferror(file);
    fclose(file);
    buffer[n] = 0;
    return ok && strstr(buffer, text) != NULL;
}

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "Line %d: %s\n", __LINE__, #expr); goto cleanup; } } while (0)

int main(void) {
    Fixture f = {0};
    MbtilesResult result;
    wchar_t parent[MAP_PATH + 1] = {0}, temp[MAX_PATH], guid[40], first[MAP_PATH], next[MAP_PATH];
    GUID id;
    BOOL own_directory = FALSE;
    int rc = 1;
    DWORD n = GetTempPathW(MAX_PATH, temp);
    CHECK(n && n < MAX_PATH && SUCCEEDED(CoCreateGuid(&id)) && StringFromGUID2(&id, guid, 40));
    CHECK(SUCCEEDED(StringCchPrintfW(parent, MAP_PATH, L"%lsGeoVoice-ortho-test-%ls", temp, guid)));
    CHECK(CreateDirectoryW(parent, NULL));
    own_directory = TRUE;
    CHECK(orthofoto_export(parent, L"Wien/West", TRUE, &f.cancel, make_fixture, &f, first, &result));
    CHECK(contains_text(first, L"zoom.txt", "19"));
    CHECK(contains_text(first, L"center.txt", "48.25000000,16.12500000"));
    CHECK(contains_text(first, L"attribution.txt", "Datenquelle: basemap.at · CC BY 4.0"));
    CHECK(contains_text(first, L"LICENSE.txt", "https://creativecommons.org/licenses/by/4.0/"));
    CHECK(contains_text(first, L"LICENSE.txt", "https://basemap.at/orthofoto-offline/"));
    CHECK(contains_text(first, L"LICENSE.txt", "Originalhinweis"));
    CHECK(orthofoto_export(parent, L"Wien/West", FALSE, &f.cancel, make_fixture, &f, next, &result));
    CHECK(wcscmp(next, first) != 0);
    CHECK(contains_text(next, L"LICENSE.txt", "https://basemap.at/orthofoto/"));
    CHECK(contains_text(first, L"LICENSE.txt", "https://basemap.at/orthofoto-offline/"));
    f.cancel_after_write = TRUE;
    CHECK(!orthofoto_export(parent, L"Wien/West", FALSE, &f.cancel, make_fixture, &f, next, &result));
    CHECK(!*next && *result.error && GetFileAttributesW(f.staging) == INVALID_FILE_ATTRIBUTES);
    rc = 0;
cleanup:
    if (own_directory) {
        SHFILEOPSTRUCTW remove = {.wFunc = FO_DELETE, .pFrom = parent,
                                  .fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI};
        if (SHFileOperationW(&remove) || remove.fAnyOperationsAborted)
            rc = 1;
    }
    return rc;
}
