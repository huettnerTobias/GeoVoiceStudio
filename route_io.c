#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <share.h>
#include <wctype.h>
#include <io.h>
#include <fcntl.h>
#include "route_io.h"
#include "vendor/miniz/miniz.h"
#include "vendor/yyjson/yyjson.h"

#define JSON_MAX (4U * 1024U * 1024U)
#define MEDIA_MAX (UINT64_C(4) * 1024 * 1024 * 1024)
#define TOTAL_MAX (2 * MEDIA_MAX)
#define MEDIA_COUNT 9999
#define MODEL_MAX (256U * 1024U * 1024U)
#define ZIP_ALLOCATION_MAX (16U * 1024U * 1024U)
#define N(a) (sizeof(a) / sizeof((a)[0]))

static INIT_ONCE error_once = INIT_ONCE_STATIC_INIT;
static DWORD error_key = FLS_OUT_OF_INDEXES;
static VOID CALLBACK release_error(PVOID buffer) { free(buffer); }
static BOOL CALLBACK initialize_error(PINIT_ONCE once, PVOID arg, PVOID *ctx) {
    (void)once;
    (void)arg;
    (void)ctx;
    error_key = FlsAlloc(release_error);
    return error_key != FLS_OUT_OF_INDEXES;
}
static wchar_t *error_buffer(void) {
    if (!InitOnceExecuteOnce(&error_once, initialize_error, NULL, NULL))
        return NULL;
    wchar_t *p = FlsGetValue(error_key);
    if (!p) {
        p = calloc(1024, sizeof(*p));
        if (!p || !FlsSetValue(error_key, p)) {
            free(p);
            return NULL;
        }
    }
    return p;
}
const wchar_t *gv_error(void) {
    const wchar_t *p = error_buffer();
    return p ? p : L"Nicht genügend Arbeitsspeicher.";
}
static int fail(const wchar_t *format, ...) {
    wchar_t *p = error_buffer();
    if (p) {
        va_list args;
        va_start(args, format);
        _vsnwprintf(p, 1023, format, args);
        va_end(args);
        p[1023] = 0;
    }
    return 0;
}
static int memory_error(void) { return fail(L"Nicht genügend Arbeitsspeicher."); }
static int join(wchar_t dest[GV_AUDIO_PATH], const wchar_t *parent, const wchar_t *name) {
    if (!parent || !name || wcslen(parent) + wcslen(name) + 2 > GV_AUDIO_PATH)
        return fail(L"Der Dateipfad ist zu lang.");
    swprintf(dest, GV_AUDIO_PATH, L"%ls\\%ls", parent, name);
    return 1;
}
static int regular(const wchar_t *path) {
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           !(attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
static int make_dir(const wchar_t *path) {
    if (CreateDirectoryW(path, NULL))
        return 1;
    DWORD attr = GetFileAttributesW(path);
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) &&
            !(attr & FILE_ATTRIBUTE_REPARSE_POINT)) ||
           fail(L"Verzeichnis kann nicht verwendet werden: %ls", path);
}
static int copy_text(wchar_t *dest, size_t cap, const wchar_t *src) {
    if (!src || wcslen(src) >= cap)
        return fail(L"Ein Textfeld ist zu lang oder fehlt.");
    wcscpy(dest, src);
    return 1;
}
static int uuid(wchar_t *out) {
    GUID id;
    wchar_t braces[40];
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(&id, braces, 40))
        return fail(L"Keine neue Kennung verfügbar.");
    wmemcpy(out, braces + 1, 36);
    out[36] = 0;
    return 1;
}
static int text_present(const wchar_t *s) {
    if (s)
        while (*s && iswspace(*s))
            ++s;
    return s && *s;
}
static int coords(double lat, double lon) {
    return isfinite(lat) && isfinite(lon) && fabs(lat) <= 90 && fabs(lon) <= 180;
}
static int wide_field(yyjson_val *obj, const char *field, wchar_t *out, size_t cap) {
    yyjson_val *v = yyjson_obj_get(obj, field);
    const char *str = yyjson_get_str(v);
    if (!str || strlen(str) != yyjson_get_len(v) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str, -1, out, (int)cap))
        return fail(L"Textfeld fehlt, ist ungültig oder zu lang (%hs).", field);
    return 1;
}
static int to_utf8(const wchar_t *s, char *buf, size_t cap) {
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1, buf, (int)cap, NULL, NULL) ||
           fail(L"Text ist zu lang oder enthält ungültiges Unicode.");
}
static int string_property(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key, const wchar_t *s) {
    char bytes[4096];
    return to_utf8(s, bytes, sizeof(bytes)) && yyjson_mut_obj_add_strcpy(doc, obj, key, bytes);
}
static int audio_name(const wchar_t *name) {
    char utf8[201];
    const wchar_t *ext = name ? wcsrchr(name, L'.') : NULL;
    if (!ext || (_wcsicmp(ext, L".mp3") && _wcsicmp(ext, L".wav")) || wcspbrk(name, L":/\\<>\"|?*") ||
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name, -1, utf8, sizeof(utf8), NULL, NULL))
        return fail(L"Ein übertragbarer WAV- oder MP3-Dateiname wird benötigt.");
    for (const wchar_t *p = name; *p; ++p)
        if (*p < 32)
            return fail(L"Steuerzeichen im Audionamen.");
    size_t stem = wcscspn(name, L".");
    while (stem && name[stem - 1] == L' ')
        stem--;
    wchar_t suffix = stem == 4 ? name[3] : 0;
    int device_number =
        (suffix >= L'1' && suffix <= L'9') || suffix == L'¹' || suffix == L'²' || suffix == L'³';
    if ((stem == 3 && (!_wcsnicmp(name, L"CON", 3) || !_wcsnicmp(name, L"NUL", 3) ||
                       !_wcsnicmp(name, L"PRN", 3) || !_wcsnicmp(name, L"AUX", 3))) ||
        (stem == 6 && !_wcsnicmp(name, L"CONIN$", 6)) || (stem == 7 && !_wcsnicmp(name, L"CONOUT$", 7)) ||
        (device_number && (!_wcsnicmp(name, L"COM", 3) || !_wcsnicmp(name, L"LPT", 3))))
        return fail(L"Reservierter Windows-Dateiname: %ls", name);
    return 1;
}
int gv_audio_path(const GvLibrary *lib, const wchar_t *name, wchar_t out[GV_AUDIO_PATH]) {
    wchar_t directory[GV_AUDIO_PATH];
    return audio_name(name) && join(directory, lib->dir, L"audio") && join(out, directory, name);
}
static void *append_slot(void *arr, size_t count, size_t width) {
    return count < SIZE_MAX / width ? realloc(arr, (count + 1) * width) : NULL;
}
int gv_add_route(GvLibrary *lib, const wchar_t *name) {
    GvRoute r = {0};
    if (!text_present(name))
        return fail(L"Bitte einen Routennamen eingeben.");
    if (!copy_text(r.name, N(r.name), name) || !uuid(r.id))
        return 0;
    GvRoute *routes = append_slot(lib->routes, lib->count, sizeof(r));
    if (!routes)
        return memory_error();
    lib->routes = routes;
    routes[lib->count++] = r;
    return 1;
}
int gv_add_point(GvRoute *r, const wchar_t *name, double lat, double lon, const wchar_t *audio) {
    GvPoint p = {0};
    if (!coords(lat, lon))
        return fail(L"Koordinaten außerhalb des gültigen Bereichs.");
    if (!copy_text(p.name, N(p.name), name) || !copy_text(p.audio, N(p.audio), audio ? audio : L"") ||
        !uuid(p.id))
        return 0;
    if (*p.audio && !audio_name(p.audio))
        return 0;
    p.lat = lat;
    p.lon = lon;
    GvPoint *points = append_slot(r->points, r->count, sizeof(p));
    if (!points)
        return memory_error();
    r->points = points;
    points[r->count++] = p;
    return 1;
}
void gv_remove_point(GvRoute *r, size_t i) {
    if (i >= r->count)
        return;
    memmove(r->points + i, r->points + i + 1, (--r->count - i) * sizeof(*r->points));
}
void gv_remove_route(GvLibrary *lib, size_t i) {
    if (i >= lib->count)
        return;
    free(lib->routes[i].points);
    memmove(lib->routes + i, lib->routes + i + 1, (--lib->count - i) * sizeof(*lib->routes));
}
void gv_free(GvLibrary *lib) {
    if (!lib)
        return;
    for (size_t i = 0; i < lib->count; i++)
        free(lib->routes[i].points);
    free(lib->routes);
    memset(lib, 0, sizeof(*lib));
}
static int compare_id(const void *a, const void *b) {
    return wcscmp(*(const wchar_t *const *)a, *(const wchar_t *const *)b);
}
static int unique_ids(const wchar_t **ids, size_t count) {
    if (count < 2)
        return 1;
    qsort(ids, count, sizeof(*ids), compare_id);
    for (size_t i = 1; i < count; i++)
        if (!wcscmp(ids[i - 1], ids[i]))
            return fail(L"Doppelte Routen- oder Punktkennung.");
    return 1;
}
static int valid_routes(const GvRoute *routes, size_t count, int portable) {
    if (portable && !count)
        return fail(L"Bitte mindestens eine Route auswählen.");
    if (count > MODEL_MAX / sizeof(*routes))
        return fail(L"Die Routenliste ist zu groß.");
    size_t remaining = MODEL_MAX - count * sizeof(*routes);
    const wchar_t **ids = calloc(count ? count : 1, sizeof(*ids));
    if (!ids)
        return memory_error();
    int ok = 1;
    for (size_t i = 0; i < count && ok; i++) {
        const GvRoute *r = routes + i;
        if (r->count > remaining / sizeof(*r->points)) {
            ok = fail(L"Die Routenliste ist zu groß.");
            break;
        }
        remaining -= r->count * sizeof(*r->points);
        ids[i] = r->id;
        if (!text_present(r->name) || !text_present(r->id) || (portable && !r->count)) {
            ok = fail(L"Die Route „%ls“ braucht einen Namen und zum Export mindestens einen Punkt.",
                      r->name);
            break;
        }
        const wchar_t **pids = calloc(r->count ? r->count : 1, sizeof(*pids));
        if (!pids) {
            ok = memory_error();
            break;
        }
        for (size_t j = 0; j < r->count && ok; j++) {
            const GvPoint *p = r->points + j;
            pids[j] = p->id;
            if (!text_present(p->id) || !coords(p->lat, p->lon) ||
                (portable && (!text_present(p->name) || !*p->audio)))
                ok = fail(L"Punkt %zu der Route „%ls“ benötigt gültige Koordinaten, Namen und Audio.",
                          j + 1, r->name);
            if (ok && *p->audio)
                ok = audio_name(p->audio);
        }
        if (ok)
            ok = unique_ids(pids, r->count);
        free(pids);
    }
    if (ok)
        ok = unique_ids(ids, count);
    free(ids);
    return ok;
}
static int parse_routes(yyjson_val *arr, GvLibrary *lib, int portable) {
    if (!yyjson_is_arr(arr))
        return fail(L"Die Routenliste fehlt.");
    size_t count = yyjson_arr_size(arr);
    if (count > MODEL_MAX / sizeof(*lib->routes))
        return fail(L"Die Routenliste ist zu groß.");
    size_t remaining = MODEL_MAX - count * sizeof(*lib->routes);
    size_t j0, n0;
    yyjson_val *entry;
    yyjson_arr_foreach(arr, j0, n0, entry) if (!yyjson_is_str(yyjson_obj_get(entry, "id"))) return fail(
        L"Routenkennung fehlt.");
    if (count && !(lib->routes = calloc(count, sizeof(*lib->routes))))
        return memory_error();
    lib->count = count;
    size_t i, n;
    yyjson_val *v;
    yyjson_arr_foreach(arr, i, n, v) {
        GvRoute *r = lib->routes + i;
        yyjson_val *points = yyjson_obj_get(v, "points");
        if (!wide_field(v, "id", r->id, N(r->id)) || !wide_field(v, "name", r->name, N(r->name)))
            return 0;
        if (!yyjson_is_arr(points))
            return fail(L"Punktliste fehlt.");
        r->count = yyjson_arr_size(points);
        if (r->count > remaining / sizeof(*r->points))
            return fail(L"Die Routenliste ist zu groß.");
        remaining -= r->count * sizeof(*r->points);
        yyjson_arr_foreach(
            points, j0, n0,
            entry) if (!yyjson_is_str(yyjson_obj_get(entry, "id"))) return fail(L"Punktkennung fehlt.");
        if (r->count && !(r->points = calloc(r->count, sizeof(*r->points))))
            return memory_error();
        size_t j, k;
        yyjson_val *pv;
        yyjson_arr_foreach(points, j, k, pv) {
            GvPoint *p = r->points + j;
            yyjson_val *lat = yyjson_obj_get(pv, "latitude"), *lon = yyjson_obj_get(pv, "longitude");
            if (!yyjson_is_num(lat) || !yyjson_is_num(lon))
                return fail(L"Numerische Koordinaten erwartet.");
            p->lat = yyjson_get_num(lat);
            p->lon = yyjson_get_num(lon);
            if (!wide_field(pv, "id", p->id, N(p->id)) || !wide_field(pv, "name", p->name, N(p->name)) ||
                !wide_field(pv, "audioFileName", p->audio, N(p->audio)))
                return 0;
        }
    }
    return valid_routes(lib->routes, lib->count, portable);
}
static int write_routes(yyjson_mut_doc *doc, yyjson_mut_val *arr, const GvRoute *routes, size_t count) {
    for (size_t i = 0; i < count; i++) {
        const GvRoute *r = routes + i;
        yyjson_mut_val *obj = yyjson_mut_arr_add_obj(doc, arr);
        if (!obj || !string_property(doc, obj, "id", r->id) || !string_property(doc, obj, "name", r->name))
            return 0;
        yyjson_mut_val *points = yyjson_mut_obj_add_arr(doc, obj, "points");
        if (!points)
            return 0;
        for (size_t j = 0; j < r->count; j++) {
            const GvPoint *p = r->points + j;
            yyjson_mut_val *item = yyjson_mut_arr_add_obj(doc, points);
            if (!item || !string_property(doc, item, "id", p->id) ||
                !string_property(doc, item, "name", p->name) ||
                !string_property(doc, item, "audioFileName", p->audio) ||
                !yyjson_mut_obj_add_real(doc, item, "latitude", p->lat) ||
                !yyjson_mut_obj_add_real(doc, item, "longitude", p->lon))
                return 0;
        }
    }
    return 1;
}
static FILE *open_input(const wchar_t *path) {
    HANDLE handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return NULL;
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        CloseHandle(handle);
        return NULL;
    }
    int fd = _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(handle);
        return NULL;
    }
    FILE *f = _fdopen(fd, "rb");
    if (!f)
        _close(fd);
    return f;
}
static int temporary(const wchar_t *target, wchar_t temp[GV_AUDIO_PATH]) {
    wchar_t id[40];
    if (!uuid(id))
        return 0;
    if (wcslen(target) + 42 >= GV_AUDIO_PATH)
        return fail(L"Zielpfad ist zu lang.");
    swprintf(temp, GV_AUDIO_PATH, L"%ls.%ls.tmp", target, id);
    return 1;
}
static FILE *open_new(const wchar_t *path, HANDLE *retained) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE | (retained ? DELETE : 0), 0, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    int fd = _open_osfhandle((intptr_t)h, _O_WRONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(h);
        DeleteFileW(path);
        return NULL;
    }
    FILE *f = _fdopen(fd, "wb");
    if (!f) {
        _close(fd);
        DeleteFileW(path);
        return NULL;
    }
    if (retained) {
        /* The stage stays locked across the confirmation dialog. If the process
           exits, Windows removes it when its last handle closes. */
        FILE_DISPOSITION_INFO discard = {TRUE};
        HANDLE process = GetCurrentProcess();
        if (!SetFileInformationByHandle(h, FileDispositionInfo, &discard, sizeof(discard)) ||
            !DuplicateHandle(process, h, process, retained, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            fclose(f);
            DeleteFileW(path);
            *retained = NULL;
            return NULL;
        }
    }
    return f;
}
static int publish(const wchar_t *temp, const wchar_t *target) {
    return MoveFileExW(temp, target, MOVEFILE_WRITE_THROUGH | MOVEFILE_REPLACE_EXISTING) ||
           fail(L"Die fertige Datei konnte nicht gespeichert werden: %ls", target);
}
static int flush_close(FILE *f) {
    int ok = !fflush(f) && !_commit(_fileno(f));
    if (fclose(f))
        ok = 0;
    return ok;
}
int gv_save(const GvLibrary *lib) {
    wchar_t target[GV_AUDIO_PATH], temp[GV_AUDIO_PATH];
    if (!valid_routes(lib->routes, lib->count, 0) || !join(target, lib->dir, L"studio-routes.json") ||
        !temporary(target, temp))
        return 0;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_arr(doc) : NULL;
    if (!root) {
        yyjson_mut_doc_free(doc);
        return memory_error();
    }
    yyjson_mut_doc_set_root(doc, root);
    size_t n;
    char *s = write_routes(doc, root, lib->routes, lib->count) ? yyjson_mut_write(doc, 0, &n) : NULL;
    yyjson_mut_doc_free(doc);
    if (!s)
        return memory_error();
    if (n > JSON_MAX) {
        free(s);
        return fail(L"Die Bibliothek überschreitet 4 MiB Routendaten.");
    }
    FILE *f = open_new(temp, NULL);
    int ok = 0;
    if (f) {
        int written = fwrite(s, 1, n, f) == n;
        ok = flush_close(f) && written && publish(temp, target);
        if (!ok)
            DeleteFileW(temp);
    }
    free(s);
    return ok || fail(L"Routen konnten nicht gespeichert werden: %ls", target);
}
static char *read_small(const wchar_t *path, size_t *len) {
    FILE *f = open_input(path);
    if (!f) {
        fail(L"Datei kann nicht geöffnet werden: %ls", path);
        return NULL;
    }
    char *buf = NULL;
    if (!_fseeki64(f, 0, SEEK_END)) {
        int64_t n = _ftelli64(f);
        if (n >= 0 && n <= JSON_MAX && !_fseeki64(f, 0, SEEK_SET)) {
            buf = malloc((size_t)n + 1);
            if (buf) {
                if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
                    free(buf);
                    buf = NULL;
                } else {
                    buf[n] = 0;
                    *len = (size_t)n;
                }
            }
        }
    }
    fclose(f);
    if (!buf)
        fail(L"Datei unlesbar oder größer als 4 MiB: %ls", path);
    return buf;
}
static int cmp_keys(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
static int check_keys(yyjson_val *v, unsigned depth) {
    if (depth > 64)
        return fail(L"Die JSON-Datei ist zu tief verschachtelt.");
    size_t i, n;
    if (yyjson_is_arr(v)) {
        yyjson_val *item;
        yyjson_arr_foreach(v, i, n, item) if (!check_keys(item, depth + 1)) return 0;
    } else if (yyjson_is_obj(v)) {
        size_t count = yyjson_obj_size(v);
        const char **keys = count ? malloc(count * sizeof(*keys)) : NULL;
        if (count && !keys)
            return memory_error();
        int ok = 1;
        yyjson_val *k, *value;
        yyjson_obj_foreach(v, i, n, k, value) {
            keys[i] = yyjson_get_str(k);
            if (strlen(keys[i]) != yyjson_get_len(k)) {
                ok = fail(L"Ungültiger JSON-Schlüssel.");
                break;
            }
            if (!check_keys(value, depth + 1)) {
                ok = 0;
                break;
            }
        }
        if (ok && count > 1) {
            qsort(keys, count, sizeof(*keys), cmp_keys);
            for (i = 1; i < count; i++)
                if (!strcmp(keys[i - 1], keys[i])) {
                    ok = fail(L"Doppelter Schlüssel in der JSON-Datei.");
                    break;
                }
        }
        free(keys);
        return ok;
    }
    return 1;
}
static yyjson_doc *parse_json(const char *s, size_t n) {
    yyjson_doc *doc = yyjson_read(s, n, 0);
    if (!doc)
        fail(L"Ungültiges JSON in den Routendaten.");
    else if (!check_keys(yyjson_doc_get_root(doc), 0)) {
        yyjson_doc_free(doc);
        doc = NULL;
    }
    return doc;
}
int gv_open(GvLibrary *lib, const wchar_t *dir) {
    wchar_t audio[GV_AUDIO_PATH], path[GV_AUDIO_PATH];
    if (!copy_text(lib->dir, N(lib->dir), dir) || !make_dir(dir) || !join(audio, dir, L"audio") ||
        !make_dir(audio) || !join(path, dir, L"studio-routes.json"))
        return 0;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND)
        return 1;
    size_t n;
    char *bytes = read_small(path, &n);
    if (!bytes)
        return 0;
    yyjson_doc *doc = parse_json(bytes, n);
    free(bytes);
    int ok = doc && parse_routes(yyjson_doc_get_root(doc), lib, 0);
    yyjson_doc_free(doc);
    if (!ok)
        gv_free(lib);
    return ok;
}

typedef struct {
    wchar_t name[256], installed[256], stage[GV_AUDIO_PATH];
    char sha[65];
    uint64_t size;
    int descriptor, archive_entry, created;
    HANDLE stage_handle;
} Media;
typedef struct {
    Media *files;
    size_t count;
} MediaList;
static int name_compare(const wchar_t *a, const wchar_t *b) {
    return CompareStringOrdinal(a, -1, b, -1, TRUE) - CSTR_EQUAL;
}
static int cmp_audio(const void *a, const void *b) {
    return name_compare(*(const wchar_t *const *)a, *(const wchar_t *const *)b);
}
static int collect_media(const GvRoute *routes, size_t count, MediaList *list) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++)
        total += routes[i].count;
    const wchar_t **names = calloc(total ? total : 1, sizeof(*names));
    if (!names)
        return memory_error();
    size_t n = 0;
    for (size_t i = 0; i < count; i++)
        for (size_t j = 0; j < routes[i].count; j++)
            if (*routes[i].points[j].audio)
                names[n++] = routes[i].points[j].audio;
    qsort(names, n, sizeof(*names), cmp_audio);
    size_t groups = 0;
    for (size_t i = 0; i < n; i++) {
        if (i && !name_compare(names[i], names[i - 1])) {
            if (wcscmp(names[i], names[i - 1])) {
                free(names);
                return fail(L"Audionamen unterscheiden sich nur in der Großschreibung.");
            }
        } else
            names[groups++] = names[i];
    }
    if (groups > MEDIA_COUNT) {
        free(names);
        return fail(L"Zu viele Audiodateien.");
    }
    list->files = calloc(groups ? groups : 1, sizeof(*list->files));
    if (!list->files) {
        free(names);
        return memory_error();
    }
    list->count = groups;
    for (size_t i = 0; i < groups; i++) {
        wcscpy(list->files[i].name, names[i]);
        list->files[i].archive_entry = -1;
    }
    free(names);
    return 1;
}
static Media *lookup(MediaList *list, const wchar_t *name) {
    size_t lo = 0, hi = list->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = name_compare(name, list->files[mid].name);
        if (cmp < 0)
            hi = mid;
        else if (cmp > 0)
            lo = mid + 1;
        else
            return wcscmp(name, list->files[mid].name) ? NULL : list->files + mid;
    }
    return NULL;
}
/* Read the size from the same locked handle used for hashing and transfer. */
static int input_size(FILE *f, uint64_t limit, uint64_t *size) {
    LARGE_INTEGER length;
    if (!GetFileSizeEx((HANDLE)_get_osfhandle(_fileno(f)), &length) || length.QuadPart < 0 ||
        (uint64_t)length.QuadPart > limit)
        return fail(L"Die Audiodatei ist zu groß oder ihre Größe konnte nicht bestimmt werden.");
    *size = (uint64_t)length.QuadPart;
    return 1;
}

typedef struct {
    FILE *file;
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hash;
    uint64_t expected, transferred;
} AudioStream;

static int start_stream(AudioStream *s) {
    if (BCryptOpenAlgorithmProvider(&s->alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0)
        return fail(L"SHA-256 steht nicht zur Verfügung.");
    if (BCryptCreateHash(s->alg, &s->hash, NULL, 0, NULL, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(s->alg, 0);
        return fail(L"Die Audio-Prüfsumme konnte nicht initialisiert werden.");
    }
    return 1;
}

/* Called by miniz on export and by the ordinary file-copy loop on import. */
static size_t read_hashed(void *opaque, mz_uint64 offset, void *data, size_t capacity) {
    AudioStream *s = opaque;
    if (offset != s->transferred || s->transferred >= s->expected)
        return 0;
    uint64_t remaining = s->expected - s->transferred;
    size_t want = remaining < capacity ? (size_t)remaining : capacity;
    size_t n = fread(data, 1, want, s->file);
    if (n && BCryptHashData(s->hash, data, (ULONG)n, 0) >= 0) {
        s->transferred += n;
        return n;
    }
    return 0;
}

/* Extraction checks the declared size before writing each block. */
static size_t write_hashed(void *opaque, mz_uint64 offset, const void *data, size_t n) {
    AudioStream *s = opaque;
    if (offset != s->transferred || n > s->expected - s->transferred || n > ULONG_MAX ||
        fwrite(data, 1, n, s->file) != n || BCryptHashData(s->hash, (PUCHAR)data, (ULONG)n, 0) < 0)
        return 0;
    s->transferred += n;
    return n;
}

static int end_stream(AudioStream *s, char hex[65]) {
    unsigned char sum[32];
    int ok = s->transferred == s->expected && !ferror(s->file) &&
             BCryptFinishHash(s->hash, sum, sizeof(sum), 0) >= 0;
    BCryptDestroyHash(s->hash);
    BCryptCloseAlgorithmProvider(s->alg, 0);
    if (!ok)
        return fail(L"Audiodatei konnte nicht vollständig verarbeitet werden.");
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(sum); i++) {
        hex[i * 2] = digits[sum[i] >> 4];
        hex[i * 2 + 1] = digits[sum[i] & 15];
    }
    hex[64] = 0;
    return 1;
}

static int copy_and_hash(FILE *src, FILE *out, uint64_t size, char hex[65]) {
    AudioStream s = {.file = src, .expected = size};
    if (!start_stream(&s))
        return 0;
    unsigned char block[65536];
    size_t n;
    int ok = 1;
    while ((n = read_hashed(&s, s.transferred, block, sizeof(block))) != 0) {
        if (out && fwrite(block, 1, n, out) != n) {
            ok = fail(L"Audiodatei konnte nicht vollständig geschrieben werden.");
            break;
        }
    }
    return end_stream(&s, hex) && ok;
}

static int add_media_zip(mz_zip_archive *zip, const GvLibrary *lib, Media *media, yyjson_mut_doc *doc,
                         yyjson_mut_val *descriptors, uint64_t remaining) {
    wchar_t path[GV_AUDIO_PATH];
    char entry[256] = "audio/";
    if (!gv_audio_path(lib, media->name, path) || !to_utf8(media->name, entry + 6, sizeof(entry) - 6))
        return 0;
    FILE *f = open_input(path);
    if (!f)
        return fail(L"Audiodatei fehlt oder ist gesperrt: %ls", path);
    AudioStream s = {.file = f};
    int ok = input_size(f, remaining < MEDIA_MAX ? remaining : MEDIA_MAX, &s.expected) && start_stream(&s);
    if (ok) {
        media->size = s.expected;
        int added = mz_zip_writer_add_read_buf_callback(zip, entry, read_hashed, &s, s.expected, NULL, NULL,
                                                        0, 0, NULL, 0, NULL, 0);
        ok = end_stream(&s, media->sha) && added;
        if (!ok)
            fail(L"Audiodatei kann nicht in das Paket geschrieben werden: %ls", media->name);
    }
    fclose(f);
    if (!ok)
        return 0;
    yyjson_mut_val *item = yyjson_mut_arr_add_obj(doc, descriptors);
    return (item && string_property(doc, item, "name", media->name) &&
            yyjson_mut_obj_add_uint(doc, item, "size", media->size) &&
            yyjson_mut_obj_add_strcpy(doc, item, "sha256", media->sha)) ||
           memory_error();
}
static int export_routes(const GvLibrary *lib, const GvRoute *routes, size_t count, const wchar_t *target) {
    const wchar_t *ext = target ? wcsrchr(target, L'.') : NULL;
    if (!ext || _wcsicmp(ext, L".geovoice"))
        return fail(L"Routenpakete benötigen die Dateiendung .geovoice.");
    wchar_t temp[GV_AUDIO_PATH] = {0};
    MediaList media = {0};
    if (!valid_routes(routes, count, 1) || !temporary(target, temp) ||
        !collect_media(routes, count, &media))
        return 0;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL, *audio = NULL, *entries = NULL;
    int ok = root && yyjson_mut_obj_add_str(doc, root, "format", "geovoice-route-package") &&
             yyjson_mut_obj_add_uint(doc, root, "version", 1) &&
             (entries = yyjson_mut_obj_add_arr(doc, root, "routes")) &&
             write_routes(doc, entries, routes, count) &&
             (audio = yyjson_mut_obj_add_arr(doc, root, "audio"));
    if (!ok)
        memory_error();
    else
        yyjson_mut_doc_set_root(doc, root);
    FILE *f = ok ? open_new(temp, NULL) : NULL;
    if (ok && !f)
        ok = fail(L"Exportziel kann nicht geschrieben werden: %ls", target);
    mz_zip_archive zip = {0};
    int initialized = ok && mz_zip_writer_init_cfile(&zip, f, MZ_ZIP_FLAG_WRITE_ZIP64);
    if (ok && !initialized)
        ok = fail(L"ZIP-Datei kann nicht erstellt werden.");
    uint64_t total = 0;
    for (size_t i = 0; ok && i < media.count; i++) {
        ok = add_media_zip(&zip, lib, media.files + i, doc, audio, TOTAL_MAX - total);
        total += media.files[i].size;
    }
    size_t bytes = 0;
    char *s = ok ? yyjson_mut_write(doc, 0, &bytes) : NULL;
    if (ok && (!s || bytes > JSON_MAX))
        ok = fail(L"Paketmanifest zu groß oder nicht verfügbar.");
    if (ok && (!mz_zip_writer_add_mem(&zip, "manifest.json", s, bytes, MZ_BEST_SPEED) ||
               !mz_zip_writer_finalize_archive(&zip)))
        ok = fail(L"Paket konnte nicht fertiggestellt werden.");
    free(s);
    yyjson_mut_doc_free(doc);
    free(media.files);
    if (initialized && !mz_zip_writer_end(&zip) && ok)
        ok = fail(L"ZIP-Datei konnte nicht geschlossen werden.");
    if (f && !flush_close(f) && ok)
        ok = fail(L"Exportdatei konnte nicht vollständig geschrieben werden.");
    if (ok)
        ok = publish(temp, target);
    if (!ok && f)
        DeleteFileW(temp);
    return ok;
}
int gv_export(const GvLibrary *lib, size_t index, const wchar_t *path) {
    if (index >= lib->count)
        return fail(L"Bitte eine Route auswählen.");
    return export_routes(lib, lib->routes + index, 1, path);
}
int gv_export_all(const GvLibrary *lib, const wchar_t *path) {
    return export_routes(lib, lib->routes, lib->count, path);
}
static int make_stage(const GvLibrary *lib, Media *m) {
    wchar_t staging[GV_AUDIO_PATH];
    return join(staging, lib->dir, L"media") && temporary(staging, m->stage);
}
static int staging_space(const GvLibrary *lib, uint64_t bytes) {
    ULARGE_INTEGER available;
    return (GetDiskFreeSpaceExW(lib->dir, &available, NULL, NULL) &&
            available.QuadPart >= bytes + JSON_MAX) ||
           fail(L"Zu wenig freier Speicher für Audiodateien und Routendaten.");
}
static int descriptors(yyjson_val *audio, MediaList *media) {
    if (!yyjson_is_arr(audio) || yyjson_arr_size(audio) != media->count)
        return fail(L"Audioverzeichnis des Pakets ist unvollständig.");
    uint64_t total = 0;
    size_t i, n;
    yyjson_val *v;
    yyjson_arr_foreach(audio, i, n, v) {
        wchar_t name[256];
        if (!wide_field(v, "name", name, N(name)))
            return 0;
        Media *m = lookup(media, name);
        yyjson_val *size = yyjson_obj_get(v, "size"), *hash = yyjson_obj_get(v, "sha256");
        const char *s = yyjson_get_str(hash);
        if (!m || m->descriptor || !yyjson_is_uint(size) || !s || yyjson_get_len(hash) != 64 ||
            strspn(s, "0123456789abcdef") != 64)
            return fail(L"Ungültiger Audioeintrag im Manifest.");
        m->size = yyjson_get_uint(size);
        if (m->size > MEDIA_MAX || (total += m->size) > TOTAL_MAX)
            return fail(L"Paket überschreitet die unterstützte Audiogröße.");
        strcpy(m->sha, s);
        m->descriptor = 1;
    }
    return 1;
}
/* The ZIP library loads its central directory before the application can check
   the entry count. GeoVoice entries have short names and need no large ZIP metadata. */
static void *zip_realloc(void *ctx, void *address, size_t n, size_t size) {
    (void)ctx;
    return size && n <= ZIP_ALLOCATION_MAX / size ? realloc(address, n * size) : NULL;
}
static void *zip_alloc(void *ctx, size_t n, size_t size) { return zip_realloc(ctx, NULL, n, size); }
static void zip_free(void *ctx, void *address) {
    (void)ctx;
    free(address);
}

static int read_package(const GvLibrary *lib, const wchar_t *path, GvLibrary *incoming, MediaList *media) {
    FILE *f = open_input(path);
    mz_zip_archive zip = {.m_pAlloc = zip_alloc, .m_pRealloc = zip_realloc, .m_pFree = zip_free};
    if (!f || !mz_zip_reader_init_cfile(&zip, f, 0, MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY)) {
        if (f)
            fclose(f);
        return fail(L"Kein lesbares GeoVoice-ZIP-Paket.");
    }
    mz_uint files = mz_zip_reader_get_num_files(&zip);
    int idx = mz_zip_reader_locate_file(&zip, "manifest.json", NULL, MZ_ZIP_FLAG_CASE_SENSITIVE);
    mz_zip_archive_file_stat stat;
    int ok = files > 0 && files <= MEDIA_COUNT + 1 && idx >= 0 &&
             mz_zip_reader_file_stat(&zip, (mz_uint)idx, &stat) && stat.m_uncomp_size > 0 &&
             stat.m_uncomp_size <= JSON_MAX;
    if (!ok)
        fail(L"Paketmanifest fehlt, ist zu groß oder das Paket enthält zu viele Dateien.");
    size_t len = 0;
    char *s = ok ? mz_zip_reader_extract_to_heap(&zip, (mz_uint)idx, &len, 0) : NULL;
    yyjson_doc *doc = s ? parse_json(s, len) : NULL;
    if (ok && !doc)
        ok = fail(L"Paketmanifest ist unlesbar.");
    mz_free(s);
    if (ok) {
        yyjson_val *root = yyjson_doc_get_root(doc), *version = yyjson_obj_get(root, "version");
        if (!yyjson_equals_str(yyjson_obj_get(root, "format"), "geovoice-route-package") ||
            !yyjson_is_uint(version) || yyjson_get_uint(version) != 1)
            ok = fail(L"Unbekanntes GeoVoice-Paketformat.");
        if (ok)
            ok = parse_routes(yyjson_obj_get(root, "routes"), incoming, 1) &&
                 collect_media(incoming->routes, incoming->count, media) &&
                 descriptors(yyjson_obj_get(root, "audio"), media);
        if (ok && files != media->count + 1)
            ok = fail(L"Das Paket enthält fehlende oder unerwartete Dateien.");
    }
    yyjson_doc_free(doc);
    for (mz_uint i = 0; ok && i < files; i++) {
        if ((int)i == idx)
            continue;
        wchar_t name[256];
        if (!mz_zip_reader_file_stat(&zip, i, &stat) || stat.m_is_directory || stat.m_is_encrypted ||
            mz_zip_reader_get_filename(&zip, i, NULL, 0) != strlen(stat.m_filename) + 1 ||
            strncmp(stat.m_filename, "audio/", 6) ||
            !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, stat.m_filename + 6, -1, name,
                                 (int)N(name))) {
            ok = fail(L"Unerwarteter ZIP-Eintrag.");
            break;
        }
        Media *m = lookup(media, name);
        if (!m || m->archive_entry != -1 || m->size != stat.m_uncomp_size) {
            ok = fail(L"Dateigröße oder Audioeintrag im ZIP ist ungültig.");
            break;
        }
        m->archive_entry = (int)i;
    }
    if (ok && media->count) {
        uint64_t bytes = 0;
        for (size_t i = 0; i < media->count; i++)
            bytes += media->files[i].size;
        ok = staging_space(lib, bytes);
    }
    for (size_t i = 0; ok && i < media->count; i++) {
        Media *m = media->files + i;
        if (!make_stage(lib, m)) {
            ok = 0;
            break;
        }
        FILE *out = open_new(m->stage, &m->stage_handle);
        if (!out) {
            m->stage[0] = 0;
            ok = fail(L"Temporäre Audiodatei kann nicht erstellt werden.");
            break;
        }
        AudioStream s = {.file = out, .expected = m->size};
        char sha[65];
        int extracted = start_stream(&s);
        if (extracted) {
            int read =
                mz_zip_reader_extract_to_callback(&zip, (mz_uint)m->archive_entry, write_hashed, &s, 0);
            extracted = end_stream(&s, sha) && read;
        }
        if (!flush_close(out) || !extracted) {
            ok = fail(L"Audiodatei im Paket beschädigt: %ls", m->name);
            break;
        }
        if (strcmp(sha, m->sha))
            ok = fail(L"Audio-Prüfsumme stimmt nicht: %ls", m->name);
    }
    mz_zip_reader_end(&zip);
    fclose(f);
    return ok;
}

/* Unmarked Java libraries used quote-only escaping. Preserve their literal backslashes. */
static char *legacy_strings(const char *src, size_t n, size_t *out_len) {
    char *out = malloc(n * 6 + 1);
    if (!out) {
        memory_error();
        return NULL;
    }
    size_t j = 0;
    int quoted = 0;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        if (quoted && c == '\\') {
            out[j++] = '\\';
            if (i + 1 < n && src[i + 1] == '"') {
                out[j++] = '"';
                ++i;
            } else
                out[j++] = '\\';
        } else if (quoted && c < 32) {
            memcpy(out + j, "\\u00", 4);
            j += 4;
            out[j++] = hex[c >> 4];
            out[j++] = hex[c & 15];
        } else {
            if (c == '"')
                quoted = !quoted;
            out[j++] = (char)c;
        }
    }
    out[j] = 0;
    *out_len = j;
    return out;
}
static int java_routes(const wchar_t *dir, const wchar_t *suffix, GvLibrary *imports) {
    wchar_t path[GV_AUDIO_PATH], marker[GV_AUDIO_PATH], basename[64];
    swprintf(basename, N(basename), L"routes.json%ls", suffix);
    if (!join(path, dir, basename))
        return 0;
    swprintf(basename, N(basename), L"routes.json.format.json%ls", suffix);
    if (!join(marker, dir, basename))
        return 0;
    int standard = GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES;
    if (standard) {
        size_t n;
        char *json = read_small(marker, &n);
        yyjson_doc *doc = json ? parse_json(json, n) : NULL;
        free(json);
        yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL,
                   *v = yyjson_obj_get(root, "formatVersion");
        int ok = yyjson_is_uint(v) && yyjson_get_uint(v) == 1 &&
                 yyjson_equals_str(yyjson_obj_get(root, "stringEncoding"), "standard-json");
        yyjson_doc_free(doc);
        if (!ok)
            return fail(L"Ungültiger Formatmarker der Java-Bibliothek.");
    }
    size_t n = 0;
    char *s = read_small(path, &n);
    if (!s)
        return 0;
    if (!standard) {
        char *canonical = legacy_strings(s, n, &n);
        free(s);
        s = canonical;
        if (!s)
            return 0;
    }
    yyjson_doc *doc = parse_json(s, n);
    free(s);
    int ok = doc && parse_routes(yyjson_doc_get_root(doc), imports, 0);
    yyjson_doc_free(doc);
    return ok;
}
static int copy_stage(const GvLibrary *lib, const wchar_t *src, Media *m, uint64_t remaining) {
    FILE *in = open_input(src);
    if (!in)
        return fail(L"Audiodatei fehlt oder ist gesperrt: %ls", src);
    int ok = input_size(in, remaining < MEDIA_MAX ? remaining : MEDIA_MAX, &m->size) &&
             staging_space(lib, m->size) && make_stage(lib, m);
    if (ok) {
        FILE *out = open_new(m->stage, &m->stage_handle);
        if (out) {
            ok = copy_and_hash(in, out, m->size, m->sha);
            if (!flush_close(out))
                ok = fail(L"Audiodatei konnte nicht gespeichert werden: %ls", m->name);
        } else {
            m->stage[0] = 0; /* No temporary file owned by us. */
            ok = fail(L"Temporäre Audiodatei kann nicht erstellt werden.");
        }
    }
    fclose(in);
    return ok;
}
static int read_java(const GvLibrary *lib, const wchar_t *dir, GvLibrary *incoming, MediaList *media) {
    wchar_t path[GV_AUDIO_PATH];
    if (!join(path, dir, L".geovoice.lock"))
        return 0;
    HANDLE lock = INVALID_HANDLE_VALUE;
    OVERLAPPED ov = {0};
    if (regular(path)) {
        lock = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
                           NULL);
        if (lock == INVALID_HANDLE_VALUE ||
            !LockFileEx(lock, LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, 0x7fffffff, &ov)) {
            if (lock != INVALID_HANDLE_VALUE)
                CloseHandle(lock);
            return fail(L"Bitte die Java-Anwendung vor dem Import schließen.");
        }
    }
    int ok = java_routes(dir, L"", incoming);
    if (!ok && join(path, dir, L"routes.json.bak") && regular(path)) {
        gv_free(incoming);
        ok = java_routes(dir, L".bak", incoming);
    }
    if (ok)
        ok = collect_media(incoming->routes, incoming->count, media);
    wchar_t audio[GV_AUDIO_PATH];
    uint64_t total = 0;
    if (ok)
        ok = join(audio, dir, L"audio");
    if (ok && media->count) {
        DWORD attr = GetFileAttributesW(audio);
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY) ||
            (attr & FILE_ATTRIBUTE_REPARSE_POINT))
            ok = fail(L"Der Audioordner fehlt oder verweist außerhalb der Bibliothek.");
    }
    for (size_t i = 0; ok && i < media->count; i++) {
        Media *m = media->files + i;
        ok = join(path, audio, m->name) && copy_stage(lib, path, m, TOTAL_MAX - total);
        total += m->size;
    }
    if (lock != INVALID_HANDLE_VALUE)
        CloseHandle(lock);
    return ok;
}
static int same_digest(const wchar_t *path, const Media *m, int *same) {
    FILE *f = open_input(path);
    char sha[65];
    uint64_t size;
    if (!f)
        return fail(L"Audiodatei fehlt oder ist gesperrt: %ls", path);
    int ok = input_size(f, UINT64_MAX, &size);
    *same = 0;
    if (ok && size == m->size) {
        ok = copy_and_hash(f, NULL, size, sha);
        *same = ok && !strcmp(sha, m->sha);
    }
    fclose(f);
    return ok;
}
static BOOL publish_stage(Media *m, const wchar_t *target) {
    wchar_t path[GV_AUDIO_PATH];
    DWORD length = GetFullPathNameW(target, N(path), path, NULL);
    if (!length || length >= N(path))
        return FALSE;
    DWORD bytes = length * sizeof(wchar_t);
    DWORD info_bytes = (DWORD)sizeof(FILE_RENAME_INFO) + bytes;
    FILE_RENAME_INFO *rename = calloc(1, info_bytes);
    if (!rename) {
        SetLastError(ERROR_OUTOFMEMORY);
        return FALSE;
    }
    rename->FileNameLength = bytes;
    memcpy(rename->FileName, path, bytes);
    FILE_DISPOSITION_INFO keep = {FALSE};
    BOOL ok = SetFileInformationByHandle(m->stage_handle, FileDispositionInfo, &keep, sizeof(keep)) &&
              SetFileInformationByHandle(m->stage_handle, FileRenameInfo, rename, info_bytes);
    DWORD reason = GetLastError();
    free(rename);
    if (ok) {
        CloseHandle(m->stage_handle);
        m->stage_handle = NULL;
    } else {
        FILE_DISPOSITION_INFO discard = {TRUE};
        SetFileInformationByHandle(m->stage_handle, FileDispositionInfo, &discard, sizeof(discard));
        SetLastError(reason);
    }
    return ok;
}
static int install_media(const GvLibrary *lib, Media *m) {
    const wchar_t *ext = wcsrchr(m->name, L'.');
    wchar_t target[GV_AUDIO_PATH], candidate[256];
    if (!copy_text(candidate, N(candidate), m->name))
        return 0;
    for (unsigned attempt = 0; attempt < 10000; attempt++) {
        if (!gv_audio_path(lib, candidate, target))
            return 0;
        WIN32_FIND_DATAW info;
        HANDLE found = FindFirstFileW(target, &info);
        if (found == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND)
                return fail(L"Audioziel ist nicht zugänglich: %ls", target);
            if (publish_stage(m, target)) {
                wcscpy(m->installed, candidate);
                *m->stage = 0;
                m->created = 1;
                return 1;
            }
            if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS)
                return fail(L"Audiodatei kann nicht übernommen werden: %ls", candidate);
        } else {
            FindClose(found);
            int same = 0;
            if (!(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
                if (!same_digest(target, m, &same))
                    return 0;
                if (same) {
                    if (!audio_name(info.cFileName))
                        return 0;
                    return copy_text(m->installed, N(m->installed), info.cFileName);
                }
            }
        }
        /* Shorten long names by UTF-8 length before adding the collision suffix. */
        size_t chars = (size_t)(ext - m->name);
        for (;;) {
            swprintf(candidate, N(candidate), L"%.*ls-%.10hs-%u%ls", (int)chars, m->name, m->sha,
                     attempt + 1, ext);
            if (audio_name(candidate))
                break;
            if (!chars)
                return fail(L"Es konnte kein freier Audioname bestimmt werden.");
            --chars;
        }
    }
    return fail(L"Zu viele gleichnamige Audiodateien.");
}
static void cleanup_media(const GvLibrary *lib, MediaList *media, int undo) {
    for (size_t i = 0; i < media->count; i++) {
        Media *m = media->files + i;
        wchar_t path[GV_AUDIO_PATH];
        if (m->stage_handle)
            CloseHandle(m->stage_handle);
        if (undo && m->created && gv_audio_path(lib, m->installed, path))
            DeleteFileW(path);
    }
    free(media->files);
}
int gv_import_audio(const GvLibrary *lib, const wchar_t *src, wchar_t outName[256]) {
    Media m = {0};
    const wchar_t *name = src;
    for (const wchar_t *p = src; *p; p++)
        if (*p == '\\' || *p == '/')
            name = p + 1;
    if (!audio_name(name) || !copy_text(m.name, N(m.name), name))
        return 0;
    int ok = copy_stage(lib, src, &m, MEDIA_MAX) && install_media(lib, &m);
    if (m.stage_handle)
        CloseHandle(m.stage_handle);
    if (ok)
        wcscpy(outName, m.installed);
    return ok;
}
typedef struct {
    const wchar_t *id;
    size_t index;
} RouteSlot;

static int compare_slot(const void *a, const void *b) {
    return wcscmp(((const RouteSlot *)a)->id, ((const RouteSlot *)b)->id);
}

struct GvImportPlan {
    GvLibrary *target;
    GvLibrary incoming;
    MediaList media;
    size_t *slots, merged_count;
};

static void free_plan(GvImportPlan *plan, int undo_media) {
    cleanup_media(plan->target, &plan->media, undo_media);
    gv_free(&plan->incoming);
    free(plan->slots);
    free(plan);
}

static int map_import_slots(GvImportPlan *plan) {
    GvLibrary *target = plan->target;
    size_t count = plan->incoming.count;
    RouteSlot *index = target->count ? calloc(target->count, sizeof(*index)) : NULL;
    plan->slots = count ? calloc(count, sizeof(*plan->slots)) : NULL;
    if ((target->count && !index) || (count && !plan->slots)) {
        free(index);
        return memory_error();
    }
    for (size_t i = 0; i < target->count; i++)
        index[i] = (RouteSlot){target->routes[i].id, i};
    if (target->count > 1)
        qsort(index, target->count, sizeof(*index), compare_slot);
    size_t overlaps = 0;
    plan->merged_count = target->count;
    for (size_t i = 0; i < count; i++) {
        RouteSlot key = {.id = plan->incoming.routes[i].id};
        const RouteSlot *existing =
            index ? bsearch(&key, index, target->count, sizeof(*index), compare_slot) : NULL;
        if (existing) {
            plan->slots[i] = existing->index;
            overlaps++;
        } else
            plan->slots[i] = plan->merged_count++;
    }
    free(index);
    if (overlaps) {
        fail(L"%zu vorhandene Route(n) werden ersetzt. Fortfahren?", overlaps);
        return -1;
    }
    return 1;
}

int gv_prepare_import(GvLibrary *lib, const wchar_t *source, GvImportPlan **out) {
    *out = NULL;
    DWORD attr = GetFileAttributesW(source);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_REPARSE_POINT))
        return fail(L"Die gewählte Paketdatei oder Bibliothek ist nicht verfügbar.");
    GvImportPlan *plan = calloc(1, sizeof(*plan));
    if (!plan)
        return memory_error();
    plan->target = lib;
    int ok = (attr & FILE_ATTRIBUTE_DIRECTORY) ? read_java(lib, source, &plan->incoming, &plan->media)
                                               : read_package(lib, source, &plan->incoming, &plan->media);
    if (ok)
        ok = map_import_slots(plan);
    if (ok) /* Includes the result requiring confirmation. */
        *out = plan;
    else
        free_plan(plan, 0);
    return ok;
}

int gv_finish_import(GvImportPlan *plan, int install) {
    if (!plan)
        return 0;
    if (!install) {
        free_plan(plan, 0);
        return 1;
    }
    GvLibrary *lib = plan->target;
    GvLibrary merged = {.count = plan->merged_count};
    wcscpy(merged.dir, lib->dir);
    merged.routes = calloc(merged.count ? merged.count : 1, sizeof(*merged.routes));
    int ok = merged.routes != NULL;
    if (!ok)
        memory_error();
    else if (lib->count)
        memcpy(merged.routes, lib->routes, lib->count * sizeof(*merged.routes));

    for (size_t i = 0; ok && i < plan->media.count; i++)
        ok = install_media(lib, plan->media.files + i);
    if (ok) {
        for (size_t i = 0; i < plan->incoming.count; i++) {
            GvRoute *r = plan->incoming.routes + i;
            for (size_t j = 0; j < r->count; j++) {
                GvPoint *p = r->points + j;
                if (*p->audio)
                    wcscpy(p->audio, lookup(&plan->media, p->audio)->installed);
            }
            merged.routes[plan->slots[i]] = *r;
        }
        /* The new and old arrays share point buffers until the replacement succeeds. */
        ok = gv_save(&merged);
        if (ok) {
            for (size_t i = 0; i < lib->count; i++)
                if (lib->routes[i].points != merged.routes[i].points)
                    free(lib->routes[i].points);
            free(lib->routes);
            *lib = merged;
            merged.routes = NULL;
            for (size_t i = 0; i < plan->incoming.count; i++)
                plan->incoming.routes[i].points = NULL;
        }
    }
    free(merged.routes);
    free_plan(plan, !ok);
    return ok;
}

int gv_import(GvLibrary *lib, const wchar_t *source, int replace) {
    GvImportPlan *plan = NULL;
    int result = gv_prepare_import(lib, source, &plan);
    if (result == -1 && !replace) {
        gv_finish_import(plan, 0);
        return -1;
    }
    return plan && gv_finish_import(plan, 1);
}
static int references(const GvLibrary *lib, const wchar_t *name) {
    for (size_t i = 0; i < lib->count; i++)
        for (size_t j = 0; j < lib->routes[i].count; j++)
            if (!name_compare(lib->routes[i].points[j].audio, name))
                return 1;
    return 0;
}
int gv_delete_audio(GvLibrary *lib, const wchar_t *name) {
    wchar_t path[GV_AUDIO_PATH];
    if (!gv_audio_path(lib, name, path))
        return 0;
    if (references(lib, name))
        return fail(L"„%ls“ wird in einer Route verwendet. Bitte zuerst die Audiozuordnung ändern.", name);
    return (regular(path) && DeleteFileW(path)) ||
           fail(L"Audiodatei kann nicht gelöscht werden: %ls", name);
}
int gv_rename_audio(GvLibrary *lib, const wchar_t *oldName, const wchar_t *newName) {
    wchar_t src[GV_AUDIO_PATH], dest[GV_AUDIO_PATH];
    if (!gv_audio_path(lib, oldName, src) || !gv_audio_path(lib, newName, dest))
        return 0;
    if (_wcsicmp(wcsrchr(oldName, L'.'), wcsrchr(newName, L'.')))
        return fail(L"Die Dateiendung muss beibehalten werden.");
    if (!regular(src))
        return fail(L"Die Audiodatei ist nicht verfügbar.");
    if (!wcscmp(oldName, newName))
        return 1;
    int case_only = !name_compare(oldName, newName);
    if (!case_only && (references(lib, newName) || !CopyFileW(src, dest, TRUE)))
        return fail(L"Der neue Dateiname ist bereits vergeben oder nicht verfügbar.");
    if (case_only && !MoveFileExW(src, dest, MOVEFILE_WRITE_THROUGH))
        return fail(L"Umbenennen fehlgeschlagen.");
    size_t n = 0;
    for (size_t i = 0; i < lib->count; i++)
        for (size_t j = 0; j < lib->routes[i].count; j++) {
            GvPoint *p = lib->routes[i].points + j;
            if (!name_compare(p->audio, oldName)) {
                wcscpy(p->audio, newName);
                ++n;
            }
        }
    if (n && !gv_save(lib)) {
        for (size_t i = 0; i < lib->count; i++)
            for (size_t j = 0; j < lib->routes[i].count; j++) {
                GvPoint *p = lib->routes[i].points + j;
                if (!wcscmp(p->audio, newName))
                    wcscpy(p->audio, oldName);
            }
        if (case_only)
            MoveFileW(dest, src);
        else
            DeleteFileW(dest);
        return 0;
    }
    if (!case_only && !DeleteFileW(src))
        return fail(L"Zuordnungen wurden aktualisiert. Die alte Datei ist noch in der Audioliste.");
    return 1;
}
