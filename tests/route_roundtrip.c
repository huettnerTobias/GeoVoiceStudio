#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <strsafe.h>
#include <stdio.h>
#include <string.h>
#include "../route_io.h"

/* Two mono PCM samples, 8 kHz, 16 bit. */
static const unsigned char wav[] = {
    'R','I','F','F', 40,0,0,0, 'W','A','V','E','f','m','t',' ',
    16,0,0,0, 1,0,1,0, 0x40,0x1f,0,0, 0x80,0x3e,0,0, 2,0,16,0,
    'd','a','t','a', 4,0,0,0, 0,0,1,0
};

static int write_wav(const wchar_t *path) {
    FILE *file = _wfopen(path, L"wb");
    if (!file) return 0;
    size_t written = fwrite(wav, 1, sizeof(wav), file);
    return fclose(file) == 0 && written == sizeof(wav);
}

static int same_wav(const wchar_t *path) {
    unsigned char data[sizeof(wav) + 1];
    FILE *file = _wfopen(path, L"rb");
    if (!file) return 0;
    size_t count = fread(data, 1, sizeof(data), file);
    int ok = !ferror(file) && count == sizeof(wav) && memcmp(data, wav, count) == 0;
    fclose(file);
    return ok;
}

#define CHECK(condition) do { \
    if (!(condition)) { \
        fwprintf(stderr, L"route_roundtrip.c:%d: %hs (%ls)\n", __LINE__, #condition, gv_error()); \
        goto cleanup; \
    } \
} while (0)

int main(void) {
    GvLibrary source = {0}, player = {0};
    GvImportPlan *prepared = NULL;
    wchar_t temp[MAX_PATH], unique[40], root[MAX_PATH + 1] = {0};
    wchar_t source_dir[MAX_PATH], player_dir[MAX_PATH], input[MAX_PATH], package[MAX_PATH];
    wchar_t audio[256], installed[GV_AUDIO_PATH], route_id[256], point_id[256];
    GUID guid;
    BOOL own_directory = FALSE;
    int result = 1;

    DWORD length = GetTempPathW(MAX_PATH, temp);
    CHECK(length > 0 && length < MAX_PATH);
    CHECK(SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(&guid, unique, 40));
    CHECK(SUCCEEDED(StringCchPrintfW(root, MAX_PATH, L"%lsGeoVoice-test-%ls", temp, unique)));
    CHECK(CreateDirectoryW(root, NULL));
    own_directory = TRUE;
    CHECK(SUCCEEDED(StringCchPrintfW(source_dir, MAX_PATH, L"%ls\\studio", root)));
    CHECK(SUCCEEDED(StringCchPrintfW(player_dir, MAX_PATH, L"%ls\\import", root)));
    CHECK(SUCCEEDED(StringCchPrintfW(input, MAX_PATH, L"%ls\\Grüße.wav", root)));
    CHECK(SUCCEEDED(StringCchPrintfW(package, MAX_PATH, L"%ls\\Wien.geovoice", root)));

    CHECK(gv_open(&source, source_dir));
    CHECK(write_wav(input));
    CHECK(gv_import_audio(&source, input, audio) && wcscmp(audio, L"Grüße.wav") == 0);
    CHECK(gv_add_route(&source, L"Wien – Donau"));
    CHECK(gv_add_point(&source.routes[0], L"Südufer", 48.125, 16.375, audio));
    CHECK(gv_add_point(&source.routes[0], L"Brücke", 48.25, 16.5, audio));
    wcscpy(route_id, source.routes[0].id);
    wcscpy(point_id, source.routes[0].points[0].id);
    CHECK(gv_save(&source));
    gv_free(&source);
    CHECK(gv_open(&source, source_dir));
    CHECK(source.count == 1 && source.routes[0].count == 2);
    CHECK(gv_export(&source, 0, package));

    CHECK(gv_open(&player, player_dir));
    CHECK(gv_import(&player, package, 0) == 1);
    CHECK(player.count == 1 && player.routes[0].count == 2);
    CHECK(wcscmp(player.routes[0].id, route_id) == 0 && wcscmp(player.routes[0].name, L"Wien – Donau") == 0);
    GvPoint *first = &player.routes[0].points[0];
    CHECK(wcscmp(first->id, point_id) == 0 && wcscmp(first->name, L"Südufer") == 0);
    CHECK(first->lat == 48.125 && first->lon == 16.375 && wcscmp(first->audio, audio) == 0);
    CHECK(wcscmp(player.routes[0].points[1].name, L"Brücke") == 0);
    CHECK(!gv_delete_audio(&player, audio));
    CHECK(gv_audio_path(&player, audio, installed) && same_wav(installed));

    wcscpy(source.routes[0].name, L"Neue Fassung");
    CHECK(gv_export(&source, 0, package));
    CHECK(gv_import(&player, package, 0) == -1);
    CHECK(wcscmp(player.routes[0].name, L"Wien – Donau") == 0);
    CHECK(gv_prepare_import(&player, package, &prepared) == -1 && prepared);
    CHECK(DeleteFileW(package)); /* The USB source may disappear while the prompt is visible. */
    int applied = gv_finish_import(prepared, 1);
    prepared = NULL;
    CHECK(applied == 1);
    gv_free(&player);
    CHECK(gv_open(&player, player_dir));
    CHECK(player.count == 1 && wcscmp(player.routes[0].id, route_id) == 0);
    CHECK(wcscmp(player.routes[0].name, L"Neue Fassung") == 0 && same_wav(installed));
    result = 0;

cleanup:
    gv_finish_import(prepared, 0);
    gv_free(&player);
    gv_free(&source);
    if (own_directory) {
        /* Only the newly created GUID directory is passed; root has two terminating NULs. */
        SHFILEOPSTRUCTW remove = {0};
        remove.wFunc = FO_DELETE;
        remove.pFrom = root;
        remove.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        if (SHFileOperationW(&remove) != 0 || remove.fAnyOperationsAborted) {
            fwprintf(stderr, L"Could not remove test directory %ls\n", root);
            result = 1;
        }
    }
    return result;
}
