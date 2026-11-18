#include "studio_auto.h"
#include "map_orthofoto.h"
#include "map_image.h"
#include <bcrypt.h>
#include <stdint.h>
#include <stdlib.h>
#include <strsafe.h>

typedef struct {
    size_t count;
    MapPoint points[];
} Geometry;

typedef struct RouteWork {
    struct RouteWork *next;
    wchar_t id[256];
    Geometry *pending, *completed;
    StudioAutoUpdate update;
    BOOL discarded;
} RouteWork;

struct StudioAuto {
    HWND owner;
    HANDLE thread;
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE wake;
    volatile LONG cancel;
    BOOL enabled, stopping;
    wchar_t library[MAP_PATH], parent[MAP_PATH];
    RouteWork *routes, *active;
    Geometry *running;
    ULONGLONG sequence;
};

BOOL studio_auto_directory(const StudioAuto *w, const wchar_t *id, wchar_t out[MAP_PATH]) {
    if (!out)
        return FALSE;
    *out = 0;
    if (!w || !id || !*id)
        return FALSE;
    size_t length = wcsnlen(id, 256);
    if (length == 256)
        return FALSE;
    /* Imported IDs can contain arbitrary Unicode, including characters invalid in filenames. */
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    BYTE digest[32];
    BOOL ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) >= 0 &&
              BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) >= 0 &&
              BCryptHashData(hash, (PUCHAR)id, (ULONG)(length * sizeof(*id)), 0) >= 0 &&
              BCryptFinishHash(hash, digest, sizeof digest, 0) >= 0;
    if (hash)
        BCryptDestroyHash(hash);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok)
        return FALSE;
    static const wchar_t digits[] = L"0123456789abcdef";
    wchar_t hex[65];
    for (size_t i = 0; i < sizeof digest; i++) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    hex[64] = 0;
    return SUCCEEDED(StringCchPrintfW(out, MAP_PATH, L"%ls\\%ls", w->parent, hex));
}

static Geometry *snapshot(const GvRoute *r) {
    if (!r->points || r->count > (SIZE_MAX - sizeof(Geometry)) / sizeof(MapPoint))
        return NULL;
    Geometry *g = malloc(sizeof(*g) + sizeof(*g->points) * r->count);
    if (g) {
        g->count = r->count;
        for (size_t i = 0; i < r->count; i++)
            g->points[i] = (MapPoint){.lat = r->points[i].lat, .lon = r->points[i].lon};
    }
    return g;
}

static BOOL prefix(const Geometry *a, const Geometry *b) {
    if (!a || !b || a->count > b->count)
        return FALSE;
    for (size_t i = 0; i < a->count; i++)
        if (a->points[i].lat != b->points[i].lat || a->points[i].lon != b->points[i].lon)
            return FALSE;
    return TRUE;
}

static RouteWork **lookup(StudioAuto *w, const wchar_t *id) {
    RouteWork **link = &w->routes;
    while (*link && wcscmp((*link)->id, id))
        link = &(*link)->next;
    return link;
}

/* A posted message contains no pointers. Reading the latest state also filters stale notifications. */
static void notify(const StudioAuto *w) {
    if (w->owner)
        PostMessageW(w->owner, WM_STUDIO_AUTO, 0, 0);
}

static void remove_work(StudioAuto *w, RouteWork **link) {
    RouteWork *r = *link;
    *link = r->next;
    free(r->pending);
    free(r->completed);
    r->pending = r->completed = NULL;
    if (r == w->active) {
        r->discarded = TRUE;
        InterlockedExchange(&w->cancel, 1);
    } else
        free(r);
}

static RouteWork *new_work(StudioAuto *w, const wchar_t *id) {
    /* Keep a bounded history. Evict an idle record; its tiles remain on disk for later selections. */
    size_t count = 0;
    RouteWork **oldest = NULL;
    for (RouteWork **link = &w->routes; *link; link = &(*link)->next) {
        RouteWork *r = *link;
        count++;
        if (!r->pending && r != w->active && (!oldest || r->update.revision < (*oldest)->update.revision))
            oldest = link;
    }
    if (count >= 128) {
        if (!oldest)
            return NULL;
        remove_work(w, oldest);
    }
    RouteWork *r = calloc(1, sizeof(*r));
    if (!r || FAILED(StringCchCopyW(r->id, ARRAYSIZE(r->id), id)) ||
        !studio_auto_directory(w, id, r->update.directory)) {
        free(r);
        return NULL;
    }
    r->next = w->routes;
    w->routes = r;
    return r;
}

BOOL studio_auto_submit(StudioAuto *w, const GvRoute *route) {
    if (!w || !route || !*route->id)
        return FALSE;
    if (!route->count) {
        studio_auto_forget(w, route->id);
        return TRUE;
    }
    EnterCriticalSection(&w->lock);
    BOOL ok = TRUE;
    Geometry *g = NULL;
    if (!w->enabled || w->stopping)
        goto done;
    g = snapshot(route);
    if (!g) {
        ok = FALSE;
        goto done;
    }
    RouteWork *r = *lookup(w, route->id);
    if (!r)
        r = new_work(w, route->id);
    if (!r) {
        ok = FALSE;
        goto done;
    }
    Geometry *latest = r->pending ? r->pending : (r == w->active ? w->running : r->completed);
    if (r->update.state != AUTO_FAILED && latest && latest->count == g->count && prefix(latest, g)) {
        notify(w);
        goto done;
    }
    if (r == w->active && !prefix(w->running, g))
        InterlockedExchange(&w->cancel, 1);
    free(r->pending);
    r->pending = g;
    g = NULL;
    r->update.revision = ++w->sequence;
    r->update.state = AUTO_WAITING;
    StringCchCopyW(r->update.message, ARRAYSIZE(r->update.message), L"Luftbilder für diese Route vorgemerkt.");
    WakeConditionVariable(&w->wake);
    notify(w);
done:
    LeaveCriticalSection(&w->lock);
    free(g);
    return ok;
}

static RouteWork *next_request(StudioAuto *w) {
    RouteWork *next = NULL;
    for (RouteWork *r = w->routes; r; r = r->next)
        if (r->pending && (!next || r->update.revision < next->update.revision))
            next = r;
    return next;
}

static BOOL update_cache(StudioAuto *w, const wchar_t *directory, const Geometry *geometry, size_t from,
                         MbtilesResult *result) {
    HANDLE library = map_hold_directory(w->library, FALSE);
    HANDLE parent = library != INVALID_HANDLE_VALUE ? map_hold_directory(w->parent, TRUE) : INVALID_HANDLE_VALUE;
    BOOL ok = FALSE;
    if (parent != INVALID_HANDLE_VALUE) {
        /* The user can move a completed XYZ directory before adding the next point. */
        if (from && GetFileAttributesW(directory) == INVALID_FILE_ATTRIBUTES)
            from = 0;
        ok = orthofoto_cache_update(directory, geometry->points + from, geometry->count - from, &w->cancel, result);
        CloseHandle(parent);
    } else
        StringCchCopyW(result->error, ARRAYSIZE(result->error), L"Der lokale Luftbildordner ist nicht beschreibbar.");
    if (library != INVALID_HANDLE_VALUE)
        CloseHandle(library);
    return ok;
}

static DWORD WINAPI worker_proc(void *context) {
    StudioAuto *w = context;
    GdiplusStartupInput input = {.GdiplusVersion = 1};
    ULONG_PTR gdiplus = 0;
    BOOL images_ready = GdiplusStartup(&gdiplus, &input, NULL) == Ok;
    EnterCriticalSection(&w->lock);
    for (;;) {
        RouteWork *r = next_request(w);
        if (w->stopping)
            break;
        if (!r) {
            SleepConditionVariableCS(&w->wake, &w->lock, INFINITE);
            continue;
        }
        Geometry *g = r->pending;
        /* An append only needs its new segments, beginning at the previous endpoint. */
        size_t from = prefix(r->completed, g) && r->completed->count < g->count ? r->completed->count - 1 : 0;
        w->active = r;
        w->running = g;
        r->pending = NULL;
        r->update.state = AUTO_LOADING;
        StringCchCopyW(r->update.message, ARRAYSIZE(r->update.message), L"Luftbilder werden im Hintergrund geladen …");
        InterlockedExchange(&w->cancel, 0);
        notify(w);
        LeaveCriticalSection(&w->lock);
        MbtilesResult result = {0};
        BOOL ok = images_ready && update_cache(w, r->update.directory, g, from, &result);
        if (!images_ready)
            StringCchCopyW(result.error, ARRAYSIZE(result.error), L"Windows konnte die Bildverarbeitung nicht starten.");
        EnterCriticalSection(&w->lock);
        if (r->discarded)
            free(r);
        else {
            if (ok) {
                free(r->completed);
                r->completed = g;
                g = NULL;
            }
            if (!r->pending) {
                r->update.state = ok ? AUTO_READY : AUTO_FAILED;
                if (ok) {
                    r->update.latitude = result.center_lat;
                    r->update.longitude = result.center_lon;
                    r->update.zoom = result.max_zoom;
                    StringCchCopyW(r->update.message, ARRAYSIZE(r->update.message), L"Luftbilder für diese Route verfügbar.");
                } else
                    StringCchCopyW(r->update.message, ARRAYSIZE(r->update.message), result.error);
                notify(w);
            }
        }
        free(g);
        w->active = NULL;
        w->running = NULL;
    }
    LeaveCriticalSection(&w->lock);
    if (images_ready)
        GdiplusShutdown(gdiplus);
    return 0;
}

StudioAuto *studio_auto_create(HWND owner, const wchar_t *library_dir) {
    if (!library_dir || !*library_dir)
        return NULL;
    StudioAuto *w = calloc(1, sizeof(*w));
    if (!w || FAILED(StringCchCopyW(w->library, MAP_PATH, library_dir)) ||
        FAILED(StringCchPrintfW(w->parent, MAP_PATH, L"%ls\\Luftbilder", library_dir))) {
        free(w);
        return NULL;
    }
    w->owner = owner;
    InitializeCriticalSection(&w->lock);
    InitializeConditionVariable(&w->wake);
    w->thread = CreateThread(NULL, 0, worker_proc, w, 0, NULL);
    if (!w->thread) {
        DeleteCriticalSection(&w->lock);
        free(w);
        return NULL;
    }
    return w;
}

void studio_auto_enable(StudioAuto *w, BOOL enabled) {
    if (!w)
        return;
    EnterCriticalSection(&w->lock);
    w->enabled = enabled && !w->stopping;
    if (!w->enabled)
        while (w->routes)
            remove_work(w, &w->routes);
    LeaveCriticalSection(&w->lock);
}

void studio_auto_forget(StudioAuto *w, const wchar_t *id) {
    if (!w || !id)
        return;
    EnterCriticalSection(&w->lock);
    RouteWork **link = lookup(w, id);
    if (*link)
        remove_work(w, link);
    LeaveCriticalSection(&w->lock);
}

BOOL studio_auto_status(StudioAuto *w, const wchar_t *id, StudioAutoUpdate *out) {
    if (!w || !id || !out)
        return FALSE;
    EnterCriticalSection(&w->lock);
    RouteWork *r = *lookup(w, id);
    if (r)
        *out = r->update;
    LeaveCriticalSection(&w->lock);
    return r != NULL;
}

void studio_auto_stop(StudioAuto *w) {
    if (!w)
        return;
    EnterCriticalSection(&w->lock);
    w->owner = NULL;
    w->enabled = FALSE;
    w->stopping = TRUE;
    while (w->routes)
        remove_work(w, &w->routes);
    WakeConditionVariable(&w->wake);
    LeaveCriticalSection(&w->lock);
    CancelSynchronousIo(w->thread);
}

void studio_auto_destroy(StudioAuto *w) {
    if (w) {
        studio_auto_stop(w);
        WaitForSingleObject(w->thread, INFINITE);
        CloseHandle(w->thread);
        DeleteCriticalSection(&w->lock);
        free(w);
    }
}
