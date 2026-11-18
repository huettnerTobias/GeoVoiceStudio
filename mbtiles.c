#include "mbtiles.h"
#include "map_corridor.h"
#include "map_internal.h"
#include <stdlib.h>
#include <strsafe.h>

/* The SQLite C ABI shipped by Windows. Resolve it only for an MBTiles import so
   other Studio features also work on systems without WinSQLite. */
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
enum {
    SQL_OK = 0, SQL_ROW = 100, SQL_DONE = 101, SQL_BLOB = 4,
    SQL_READONLY = 1, SQL_READ = 20, SQL_SELECT = 21, SQL_DENY = 1
};
typedef struct {
    HMODULE module;
    int (*open_v2)(const char *, sqlite3 **, int, const char *);
    int (*close)(sqlite3 *);
    int (*prepare_v2)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
    int (*step)(sqlite3_stmt *);
    int (*reset)(sqlite3_stmt *);
    int (*finalize)(sqlite3_stmt *);
    int (*bind_int)(sqlite3_stmt *, int, int);
    int (*column_type)(sqlite3_stmt *, int);
    const void *(*column_blob)(sqlite3_stmt *, int);
    const unsigned char *(*column_text)(sqlite3_stmt *, int);
    int (*column_bytes)(sqlite3_stmt *, int);
    int (*limit)(sqlite3 *, int, int);
    void (*progress_handler)(sqlite3 *, int, int (*)(void *), void *);
    int (*set_authorizer)(sqlite3 *, int (*)(void *, int, const char *, const char *, const char *, const char *), void *);
} Sql;

static BOOL load_sql(Sql *s) {
    s->module = LoadLibraryExW(L"winsqlite3.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!s->module)
        return FALSE;
#define SQL_API(name) do { \
    FARPROC proc = GetProcAddress(s->module, "sqlite3_" #name); \
    if (!proc) return FALSE; \
    memcpy(&s->name, &proc, sizeof proc); \
} while (0)
    SQL_API(open_v2);
    SQL_API(close);
    SQL_API(prepare_v2);
    SQL_API(step);
    SQL_API(reset);
    SQL_API(finalize);
    SQL_API(bind_int);
    SQL_API(column_type);
    SQL_API(column_blob);
    SQL_API(column_text);
    SQL_API(column_bytes);
    SQL_API(limit);
    SQL_API(progress_handler);
    SQL_API(set_authorizer);
#undef SQL_API
    return TRUE;
}

#define MAX_TILE (2 * 1024 * 1024)
#define MAX_OUTPUT (2ULL << 30)
typedef struct {
    Sql sql;
    sqlite3 *db;
    volatile LONG *cancel;
    unsigned operations;
    ULONGLONG bytes;
    HANDLE zoom_dir, column_dir;
    int zoom, column;
    MbtilesResult *result;
} Import;

static BOOL fail(Import *i, const wchar_t *message) {
    StringCchCopyW(i->result->error, ARRAYSIZE(i->result->error),
                   InterlockedCompareExchange(i->cancel, 0, 0) ? L"Abgebrochen." : message);
    return FALSE;
}
static int interrupt_query(void *ctx) {
    Import *i = ctx;
    /* A normally indexed route needs a few dozen instructions per tile. */
    return InterlockedCompareExchange(i->cancel, 0, 0) || ++i->operations > 250000;
}
static int allow_read(void *ctx, int action, const char *a, const char *b, const char *db, const char *view) {
    (void)ctx; (void)a; (void)b; (void)db; (void)view;
    return action == SQL_SELECT || action == SQL_READ ? SQL_OK : SQL_DENY;
}
static BOOL prepare(Import *i, const char *query, sqlite3_stmt **s) {
    return i->sql.prepare_v2(i->db, query, -1, s, NULL) == SQL_OK;
}
static BOOL regular_file(HANDLE file) {
    BY_HANDLE_FILE_INFORMATION info;
    return file != INVALID_HANDLE_VALUE && GetFileType(file) == FILE_TYPE_DISK &&
           GetFileInformationByHandle(file, &info) &&
           !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
static BOOL open_database(Import *i, const wchar_t *source) {
    char path[4 * MAP_PATH];
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, source, -1, path, sizeof path, NULL, NULL) ||
        i->sql.open_v2(path, &i->db, SQL_READONLY, NULL) != SQL_OK)
        return fail(i, L"MBTiles-Datei konnte nicht schreibgeschützt geöffnet werden.");
    i->sql.limit(i->db, 0, MAX_TILE + 4096); /* SQLITE_LIMIT_LENGTH, including encoded row overhead */
    i->sql.limit(i->db, 1, 65536); /* SQLITE_LIMIT_SQL_LENGTH */
    i->sql.limit(i->db, 2, 64);    /* SQLITE_LIMIT_COLUMN */
    i->sql.limit(i->db, 7, 0);     /* SQLITE_LIMIT_ATTACHED */
    i->sql.progress_handler(i->db, 1000, interrupt_query, i);
    sqlite3_stmt *setting = NULL;
    BOOL ok = prepare(i, "PRAGMA trusted_schema=OFF", &setting) && i->sql.step(setting) == SQL_DONE;
    i->sql.finalize(setting);
    if (!ok || i->sql.set_authorizer(i->db, allow_read, NULL) != SQL_OK)
        return fail(i, L"MBTiles-Datenbank konnte nicht sicher geöffnet werden.");
    return TRUE;
}

static BOOL contains(const char *text, const char *word) {
    size_t n = strlen(word);
    for (; *text; text++)
        if (!_strnicmp(text, word, n))
            return TRUE;
    return FALSE;
}

static BOOL metadata(Import *i, BOOL *xyz) {
    sqlite3_stmt *s = NULL;
    BOOL canonical = FALSE, provider = FALSE, orthophoto = FALSE;
    BOOL has_attribution = FALSE, provider_credit = FALSE;
    BOOL ok = prepare(i, "SELECT name,value FROM metadata WHERE name COLLATE NOCASE IN "
                           "('name','description','scheme','format','attribution','copyright')", &s);
    int rc = SQL_DONE;
    while (ok && (rc = i->sql.step(s)) == SQL_ROW) {
        const char *name = (const char *)i->sql.column_text(s, 0);
        const char *value = (const char *)i->sql.column_text(s, 1);
        if (!name || !value)
            continue;
        if (!_stricmp(name, "name") || !_stricmp(name, "description")) {
            /* The official offline archive identifies itself only as 'basemap-ortho'. */
            canonical |= !_stricmp(name, "name") && !_stricmp(value, "basemap-ortho");
            provider |= contains(value, "basemap.at");
            orthophoto |= contains(value, "orthofoto") || contains(value, "orthophoto");
        } else if (!_stricmp(name, "scheme")) {
            if (_stricmp(value, "tms") && _stricmp(value, "xyz"))
                ok = fail(i, L"Unbekanntes Kachelschema im MBTiles-Paket.");
            *xyz = !_stricmp(value, "xyz");
        } else if (!_stricmp(name, "format")) {
            if (!_stricmp(value, "pbf") || !_stricmp(value, "mvt") || !_stricmp(value, "webp"))
                ok = fail(i, L"Für diese Karte sind MBTiles mit PNG- oder JPEG-Rasterbildern erforderlich.");
        } else if (*value) {
            if (!_stricmp(name, "attribution")) {
                has_attribution = TRUE;
                provider_credit |= contains(value, "basemap.at");
            }
            wchar_t *out = i->result->source_attribution;
            size_t cap = ARRAYSIZE(i->result->source_attribution);
            int bytes = i->sql.column_bytes(s, 1);
            if ((size_t)bytes != strlen(value) || (out[0] && FAILED(StringCchCatW(out, cap, L"\r\n"))) ||
                !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1,
                                     out + wcslen(out), (int)(cap - wcslen(out))))
                ok = fail(i, L"Die ursprünglichen Quellenangaben sind zu lang oder enthalten ungültigen Text.");
        }
    }
    i->sql.finalize(s);
    if ((!ok || rc != SQL_DONE) && !i->result->error[0])
        fail(i, L"Die MBTiles-Metadaten konnten nicht gelesen werden.");
    if (!ok || rc != SQL_DONE)
        return FALSE;
    if ((!canonical && !(orthophoto && (provider || provider_credit))) || (has_attribution && !provider_credit))
        return fail(i, L"Das Paket ist nicht als basemap.at Orthofoto Österreich gekennzeichnet. "
                       L"Bitte das offizielle Orthofoto-MBTiles-Paket auswählen.");
    return TRUE;
}

/* Seek once per supported level. DISTINCT/MIN/MAX can scan the entire country archive. */
static BOOL levels(Import *i, unsigned *mask) {
    sqlite3_stmt *s = NULL;
    BOOL ok = prepare(i, "SELECT 1 FROM tiles WHERE zoom_level=?1 LIMIT 1", &s);
    for (int z = 0; ok && z <= 20; z++) {
        i->sql.bind_int(s, 1, z);
        int rc = i->sql.step(s);
        if (rc == SQL_ROW)
            *mask |= 1u << z;
        ok = (rc == SQL_ROW || rc == SQL_DONE) && i->sql.reset(s) == SQL_OK;
    }
    i->sql.finalize(s);
    if (!ok || !*mask)
        return fail(i, L"Keine lesbaren Raster-Zoomstufen zwischen 0 und 20 gefunden.");
    return TRUE;
}
static void close_directory(HANDLE *handle) {
    if (*handle != INVALID_HANDLE_VALUE) {
        CloseHandle(*handle);
        *handle = INVALID_HANDLE_VALUE;
    }
}
static BOOL hold_column(Import *i, const wchar_t *stage, CorridorTile t) {
    wchar_t path[MAP_PATH];
    if (i->zoom != t.zoom) {
        close_directory(&i->column_dir);
        close_directory(&i->zoom_dir);
        i->column = -1;
        if (FAILED(StringCchPrintfW(path, MAP_PATH, L"%ls\\%d", stage, t.zoom)) ||
            (i->zoom_dir = map_hold_directory(path, TRUE)) == INVALID_HANDLE_VALUE)
            return FALSE;
        i->zoom = t.zoom;
    }
    if (i->column != t.x) {
        close_directory(&i->column_dir);
        if (FAILED(StringCchPrintfW(path, MAP_PATH, L"%ls\\%d\\%d", stage, t.zoom, t.x)) ||
            (i->column_dir = map_hold_directory(path, TRUE)) == INVALID_HANDLE_VALUE)
            return FALSE;
        i->column = t.x;
    }
    return TRUE;
}
static BOOL write_tile(Import *i, const wchar_t *stage, CorridorTile t, sqlite3_stmt *s) {
    if (i->sql.column_type(s, 0) != SQL_BLOB)
        return fail(i, L"Ungültiger Bilddatensatz im MBTiles-Paket.");
    int size = i->sql.column_bytes(s, 0);
    const unsigned char *data = i->sql.column_blob(s, 0);
    if (!data || size < 8 || size > MAX_TILE)
        return fail(i, L"Ungültige oder zu große Bildkachel im MBTiles-Paket.");
    if (i->bytes + size > MAX_OUTPUT)
        return fail(i, L"Der Routenausschnitt überschreitet 2 GiB. Bitte die Route aufteilen.");
    const wchar_t *extension;
    if (!memcmp(data, "\x89PNG\r\n\x1a\n", 8))
        extension = L"png";
    else if (!memcmp(data, "\xff\xd8\xff", 3))
        extension = L"jpg";
    else
        return fail(i, L"Für diese Karte werden PNG- oder JPEG-Rasterbilder benötigt.");
    wchar_t path[MAP_PATH];
    if (!hold_column(i, stage, t) ||
        FAILED(StringCchPrintfW(path, MAP_PATH, L"%ls\\%d\\%d\\%d.%ls", stage, t.zoom, t.x, t.y, extension)))
        return fail(i, L"Der Zielordner für die Kacheln konnte nicht angelegt werden.");
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return fail(i, L"Eine Kacheldatei konnte nicht angelegt werden.");
    DWORD written = 0;
    BOOL ok = WriteFile(file, data, (DWORD)size, &written, NULL) && written == (DWORD)size;
    if (!CloseHandle(file) || !ok)
        return fail(i, L"Eine Kachel konnte nicht vollständig gespeichert werden.");
    if (!map_valid_tile(path))
        return fail(i, L"Beschädigtes Bild oder unerwartete Kachelgröße (erwartet: 256 × 256 Pixel).");
    i->bytes += size;
    MbtilesResult *r = i->result;
    if (t.zoom < r->min_zoom)
        r->min_zoom = t.zoom;
    if (t.zoom > r->max_zoom) {
        r->max_zoom = t.zoom;
        r->center_lat = map_lat((t.y + .5) / (1 << t.zoom));
        r->center_lon = map_lon((t.x + .5) / (1 << t.zoom));
    }
    r->written++;
    return TRUE;
}

BOOL mbtiles_extract(const wchar_t *source, const wchar_t *stage, const MapPoint *points, size_t count,
                     volatile LONG *cancel, void (*progress)(void *, int, int), void *context, MbtilesResult *r) {
    *r = (MbtilesResult){.min_zoom = 21, .max_zoom = -1};
    LONG local_cancel = 0;
    if (!cancel)
        cancel = &local_cancel;
    Import i = {.cancel = cancel, .result = r, .zoom = -1, .column = -1,
                .zoom_dir = INVALID_HANDLE_VALUE, .column_dir = INVALID_HANDLE_VALUE};
    MapCorridorPlan plan = {0};
    sqlite3_stmt *tiles = NULL;
    HANDLE file = CreateFileW(source, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    HANDLE stage_dir = map_hold_directory(stage, FALSE);
    BOOL ok = FALSE, xyz = FALSE;
    unsigned mask = 0;
    if (!regular_file(file)) {
        fail(&i, L"Bitte eine lokale MBTiles-Datei auswählen.");
        goto done;
    }
    if (stage_dir == INVALID_HANDLE_VALUE) {
        fail(&i, L"Der Arbeitsordner konnte nicht geöffnet werden.");
        goto done;
    }
    if (!load_sql(&i.sql)) {
        fail(&i, L"Für den MBTiles-Import wird die Windows-Komponente WinSQLite3 benötigt.");
        goto done;
    }
    if (!open_database(&i, source) || !metadata(&i, &xyz) || !levels(&i, &mask) ||
        !map_corridor_plan(points, count, mask, cancel, &plan, r->error))
        goto done;
    if (!prepare(&i, "SELECT tile_data FROM tiles WHERE zoom_level=?1 AND tile_column=?2 AND tile_row=?3 LIMIT 1", &tiles)) {
        fail(&i, L"Die Rasterkacheln können nicht gelesen werden.");
        goto done;
    }
    for (size_t n = 0; n < plan.count; n++) {
        if (InterlockedCompareExchange(cancel, 0, 0)) {
            fail(&i, L"Abgebrochen.");
            goto done;
        }
        CorridorTile t = plan.tiles[n];
        i.sql.bind_int(tiles, 1, t.zoom);
        i.sql.bind_int(tiles, 2, t.x);
        i.sql.bind_int(tiles, 3, xyz ? t.y : (1 << t.zoom) - 1 - t.y);
        int rc = i.sql.step(tiles);
        if (rc == SQL_ROW && !write_tile(&i, stage, t, tiles))
            goto done;
        if ((rc != SQL_DONE && rc != SQL_ROW) || i.sql.reset(tiles) != SQL_OK) {
            fail(&i, L"Eine Rasterkachel konnte nicht aus dem MBTiles-Paket gelesen werden.");
            goto done;
        }
        if (progress)
            progress(context, (int)n + 1, r->written);
    }
    ok = !InterlockedCompareExchange(cancel, 0, 0) && r->written > 0;
    if (!ok)
        fail(&i, L"Das MBTiles-Paket enthält keine Kacheln entlang dieser Route.");
done:
    close_directory(&i.column_dir);
    close_directory(&i.zoom_dir);
    close_directory(&stage_dir);
    if (tiles) i.sql.finalize(tiles);
    free(plan.tiles);
    if (i.db) i.sql.close(i.db);
    if (i.sql.module) FreeLibrary(i.sql.module);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return ok;
}
