#include "../studio_auto.h"
#include "../map_orthofoto.h"
#include <objbase.h>
#include <stdio.h>
#include <strsafe.h>

/* This test replaces the synchronous cache writer, so it never contacts a map service. */
static HANDLE started, proceed;
static volatile LONG calls, fail_next;
static size_t received;
static double first_lat, last_lat;

BOOL orthofoto_cache_update(const wchar_t *directory, const MapPoint *points, size_t count,
                            volatile LONG *cancel, MbtilesResult *result) {
    if (!CreateDirectoryW(directory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return FALSE;
    InterlockedIncrement(&calls);
    received = count;
    first_lat = points[0].lat;
    last_lat = points[count - 1].lat;
    SetEvent(started);
    while (!InterlockedCompareExchange(cancel, 0, 0) && WaitForSingleObject(proceed, 10) == WAIT_TIMEOUT) {}
    if (InterlockedCompareExchange(cancel, 0, 0))
        return FALSE;
    if (InterlockedExchange(&fail_next, 0)) {
        StringCchCopyW(result->error, ARRAYSIZE(result->error), L"Test: offline");
        return FALSE;
    }
    result->written = 7;
    result->center_lat = points[0].lat;
    result->center_lon = points[0].lon;
    result->max_zoom = 19;
    return TRUE;
}

static BOOL wait_state(StudioAuto *worker, const wchar_t *id, StudioAutoState state, StudioAutoUpdate *update) {
    ULONGLONG start = GetTickCount64();
    do {
        if (studio_auto_status(worker, id, update) && update->state == state)
            return TRUE;
        Sleep(5);
    } while (GetTickCount64() - start < 5000);
    return FALSE;
}

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "auto_worker:%d: %s\n", __LINE__, #expr); goto cleanup; } } while (0)

int main(void) {
    int rc = 1;
    wchar_t temp[MAX_PATH], root[MAX_PATH] = L"", group[MAX_PATH] = L"", directory[MAP_PATH] = L"", other[MAP_PATH] = L"";
    BOOL created = FALSE;
    StudioAuto *worker = NULL;
    StudioAutoUpdate u;
    GUID guid;
    wchar_t id[40];
    GvPoint stations[5] = {0};
    for (size_t i = 0; i < ARRAYSIZE(stations); i++) {
        stations[i].lat = 48.20 + (double)i / 100.;
        stations[i].lon = 16.37;
    }
    GvRoute a = {.id = L"A/West", .points = stations, .count = 1};
    GvRoute b = {.id = L"B", .points = stations + 3, .count = 1};
    DWORD n = GetTempPathW(MAX_PATH, temp);
    CHECK(n && n < MAX_PATH && SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(&guid, id, ARRAYSIZE(id)));
    CHECK(SUCCEEDED(StringCchPrintfW(root, MAX_PATH, L"%lsGeoVoice-Worker-%ls", temp, id)) && CreateDirectoryW(root, NULL));
    created = TRUE;
    CHECK(SUCCEEDED(StringCchPrintfW(group, MAX_PATH, L"%ls\\Luftbilder", root)));
    started = CreateEventW(NULL, FALSE, FALSE, NULL);
    proceed = CreateEventW(NULL, FALSE, FALSE, NULL);
    CHECK(started && proceed && (worker = studio_auto_create(NULL, root)));
    CHECK(studio_auto_directory(worker, a.id, directory) && studio_auto_directory(worker, b.id, other));
    CHECK(!wcsncmp(directory, group, wcslen(group)) && !wcsstr(directory, a.id) && wcscmp(other, directory));
    CHECK(studio_auto_submit(worker, &a) && GetFileAttributesW(group) == INVALID_FILE_ATTRIBUTES);
    CHECK(!studio_auto_status(worker, a.id, &u));
    studio_auto_enable(worker, TRUE);
    CHECK(studio_auto_submit(worker, &a) && WaitForSingleObject(started, 5000) == WAIT_OBJECT_0);
    CHECK(received == 1 && first_lat == stations[0].lat);
    a.count = 2;
    CHECK(studio_auto_submit(worker, &a));
    a.count = 3;
    CHECK(studio_auto_submit(worker, &a));
    SetEvent(proceed);
    CHECK(WaitForSingleObject(started, 5000) == WAIT_OBJECT_0 && received == 3 && last_lat == stations[2].lat);
    SetEvent(proceed);
    CHECK(wait_state(worker, a.id, AUTO_READY, &u) && !wcscmp(u.directory, directory) && u.zoom == 19 && calls == 2);
    ULONGLONG prior = u.revision;
    CHECK(studio_auto_submit(worker, &a) && studio_auto_status(worker, a.id, &u) && u.revision == prior);
    a.count = 4;
    CHECK(studio_auto_submit(worker, &a) && WaitForSingleObject(started, 5000) == WAIT_OBJECT_0);
    CHECK(received == 2 && first_lat == stations[2].lat && last_lat == stations[3].lat);
    SetEvent(proceed);
    CHECK(wait_state(worker, a.id, AUTO_READY, &u) && calls == 3);
    stations[1].lat += .003;
    InterlockedExchange(&fail_next, 1);
    CHECK(studio_auto_submit(worker, &a) && WaitForSingleObject(started, 5000) == WAIT_OBJECT_0 && received == a.count);
    SetEvent(proceed);
    CHECK(wait_state(worker, a.id, AUTO_FAILED, &u) && !wcscmp(u.message, L"Test: offline"));
    CHECK(studio_auto_submit(worker, &a) && WaitForSingleObject(started, 5000) == WAIT_OBJECT_0 && received == a.count);
    CHECK(studio_auto_submit(worker, &b));
    studio_auto_enable(worker, FALSE);
    CHECK(!studio_auto_status(worker, a.id, &u) && !studio_auto_status(worker, b.id, &u));
    studio_auto_enable(worker, TRUE);
    CHECK(studio_auto_submit(worker, &a) && WaitForSingleObject(started, 5000) == WAIT_OBJECT_0 && calls == 6);
    SetEvent(proceed);
    CHECK(wait_state(worker, a.id, AUTO_READY, &u));
    a.count = 0;
    CHECK(studio_auto_submit(worker, &a) && !studio_auto_status(worker, a.id, &u));
    CHECK(studio_auto_submit(worker, &b) && WaitForSingleObject(started, 5000) == WAIT_OBJECT_0);
    studio_auto_stop(worker);
    CHECK(!studio_auto_status(worker, b.id, &u));
    rc = 0;
cleanup:
    studio_auto_destroy(worker);
    if (started) CloseHandle(started);
    if (proceed) CloseHandle(proceed);
    if (created) {
        if (*directory) RemoveDirectoryW(directory);
        if (*other) RemoveDirectoryW(other);
        if (*group) RemoveDirectoryW(group);
        if (!RemoveDirectoryW(root)) rc = 1;
    }
    return rc;
}
