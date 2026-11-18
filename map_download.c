#include "map_internal.h"
#include "map_ids.h"
#include <commdlg.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <winhttp.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <wctype.h>

#define URL_CAP 4096
#define LIMIT_FILE (8ULL << 30)
#define LIMIT_XYZ (2ULL << 30)
#define LIMIT_TILE (2ULL << 20)
#define MIRROR L"https://ftp-stud.hs-esslingen.de/pub/Mirrors/download.mapsforge.org/maps/v5/europe/"
typedef enum { INFO_JOB, FILE_JOB, XYZ_JOB } Mode;
typedef struct {
    HWND dialog, canvas;
    HANDLE thread;
    volatile LONG cancel;
    CRITICAL_SECTION lock;
    wchar_t status[1024], result[512];
    wchar_t url[URL_CAP], target[MAP_PATH], credit[512], key[1536];
    double lat, lon;
    int radius, zmin, zmax, total, done, loaded;
    BOOL success, closing;
    Mode mode;
    HINTERNET session;
    uint64_t received;
    ULONGLONG last_update;
} Job;
static const wchar_t *countries[] = {L"Österreich", L"Deutschland", L"Schweiz",   L"Italien",
                                     L"Tschechien",  L"Slowakei",    L"Slowenien", L"Ungarn",
                                     L"Kroatien",    L"Frankreich",  L"Monaco",    L"Liechtenstein"};
static const wchar_t *slugs[] = {L"austria",        L"germany",  L"switzerland", L"italy",
                                 L"czech-republic", L"slovakia", L"slovenia",    L"hungary",
                                 L"croatia",        L"france",   L"monaco",      L"liechtenstein"};
static BOOL cancelled(Job *j) { return InterlockedCompareExchange(&j->cancel, 0, 0) != 0; }
static void report(Job *j, const wchar_t *s) {
    EnterCriticalSection(&j->lock);
    StringCchCopyW(j->status, ARRAYSIZE(j->status), s);
    LeaveCriticalSection(&j->lock);
}
static BOOL error(Job *j, const wchar_t *s) {
    StringCchCopyW(j->result, ARRAYSIZE(j->result), s);
    return FALSE;
}
static BOOL win_error(Job *j, const wchar_t *s) {
    DWORD code = GetLastError();
    StringCchPrintfW(j->result, ARRAYSIZE(j->result), L"%ls (Windows-Fehler %lu).", s, code);
    return FALSE;
}
static BOOL disk_space(const wchar_t *path, uint64_t need) {
    wchar_t dir[MAP_PATH];
    if (FAILED(StringCchCopyW(dir, MAP_PATH, path)) || !PathRemoveFileSpecW(dir))
        return FALSE;
    ULARGE_INTEGER available;
    return GetDiskFreeSpaceExW(dir, &available, NULL, NULL) && available.QuadPart >= need;
}
static BOOL split_url(const wchar_t *url, URL_COMPONENTSW *u, wchar_t *host, wchar_t *resource) {
    if (wcschr(url, L'#'))
        return FALSE;
    *u = (URL_COMPONENTSW){.dwStructSize = sizeof *u,
                           .dwHostNameLength = (DWORD)-1,
                           .dwUserNameLength = (DWORD)-1,
                           .dwPasswordLength = (DWORD)-1,
                           .dwUrlPathLength = (DWORD)-1,
                           .dwExtraInfoLength = (DWORD)-1};
    if (!WinHttpCrackUrl(url, 0, 0, u) ||
        (u->nScheme != INTERNET_SCHEME_HTTP && u->nScheme != INTERNET_SCHEME_HTTPS) || u->dwUserNameLength ||
        u->dwPasswordLength || !u->dwHostNameLength || u->dwHostNameLength >= 256)
        return FALSE;
    StringCchCopyNW(host, 256, u->lpszHostName, u->dwHostNameLength);
    size_t n = wcslen(host);
    while (n && host[n - 1] == L'.')
        host[--n] = 0;
    const wchar_t *blocked = L"openstreetmap.org";
    size_t b = wcslen(blocked);
    if (n >= b && !_wcsicmp(host + n - b, blocked) && (b == n || host[n - b - 1] == L'.'))
        return FALSE;
    if (!u->dwUrlPathLength)
        StringCchCopyW(resource, URL_CAP, L"/");
    else if (FAILED(StringCchCopyNW(resource, URL_CAP, u->lpszUrlPath, u->dwUrlPathLength)))
        return FALSE;
    if (u->dwExtraInfoLength &&
        FAILED(StringCchCatNW(resource, URL_CAP, u->lpszExtraInfo, u->dwExtraInfoLength)))
        return FALSE;
    return TRUE;
}
static void progress(Job *j, uint64_t bytes, uint64_t expected) {
    ULONGLONG now = GetTickCount64();
    if (now - j->last_update < 150)
        return;
    j->last_update = now;
    wchar_t line[200];
    if (j->mode == XYZ_JOB)
        StringCchPrintfW(line, ARRAYSIZE(line), L"%d / %d Kacheln geprüft · %.1f MiB übertragen", j->done,
                         j->total, j->received / 1048576.);
    else if (expected)
        StringCchPrintfW(line, ARRAYSIZE(line), L"Kartenpaket · %.1f von %.1f MiB", bytes / 1048576.,
                         expected / 1048576.);
    else
        StringCchPrintfW(line, ARRAYSIZE(line), L"Kartenpaket · %.1f MiB übertragen", bytes / 1048576.);
    report(j, line);
}
static BOOL download(Job *j, const wchar_t *first, const wchar_t *temp, uint64_t limit, BOOL head,
                     BOOL *retry) {
    wchar_t url[URL_CAP], host[256], path[URL_CAP];
    StringCchCopyW(url, URL_CAP, first);
    *retry = FALSE;
    for (int redirect = 0; redirect <= 5 && !cancelled(j); redirect++) {
        URL_COMPONENTSW u;
        if (!split_url(url, &u, host, path))
            return error(j, L"HTTP(S)-URL eines zugelassenen Anbieters ohne Zugangsdaten erforderlich. "
                            L"Öffentliche OSM-Tileserver sind gesperrt.");
        HINTERNET conn = WinHttpConnect(j->session, host, u.nPort, 0);
        HINTERNET req = conn
                            ? WinHttpOpenRequest(conn, head ? L"HEAD" : L"GET", path, NULL,
                                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                            : NULL;
        DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER, code = 0, size = sizeof code;
        BOOL again = FALSE, ok = FALSE;
        HANDLE file = INVALID_HANDLE_VALUE;
        if (!req || !WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof redirects) ||
            !WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(req, NULL) ||
            !WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code,
                                 &size, NULL)) {
            win_error(j, L"Verbindung zum Kartenanbieter fehlgeschlagen");
            *retry = TRUE;
            goto finish;
        }
        if (code >= 300 && code < 400) {
            wchar_t location[URL_CAP], next[URL_CAP];
            size = sizeof location;
            DWORD cap = URL_CAP;
            if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, NULL, location, &size, NULL) ||
                FAILED(UrlCombineW(url, location, next, &cap, 0))) {
                error(j, L"Ungültige Weiterleitung.");
                goto finish;
            }
            if (j->key[0]) {
                URL_COMPONENTSW target;
                wchar_t next_host[256], next_path[URL_CAP];
                if (!split_url(next, &target, next_host, next_path) || _wcsicmp(host, next_host) ||
                    target.nPort != u.nPort || target.nScheme != u.nScheme) {
                    error(j, L"Anbieterwechsel beim Download mit API-Key abgelehnt.");
                    goto finish;
                }
            }
            StringCchCopyW(url, URL_CAP, next);
            again = TRUE;
            goto finish;
        }
        if (head && (code == 405 || code == 501)) {
            StringCchCopyW(j->result, ARRAYSIZE(j->result),
                           L"Der Anbieter stellt keine Paketinformationen bereit.");
            ok = TRUE;
            goto finish;
        }
        if (code != 200) {
            StringCchPrintfW(j->result, ARRAYSIZE(j->result), L"Der Anbieter antwortet mit HTTP %lu.", code);
            *retry = (code == 429 || code >= 500);
            goto finish;
        }
        wchar_t length[32];
        size = sizeof length;
        BOOL known = WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, NULL, length, &size, NULL);
        uint64_t expected = known ? _wcstoui64(length, NULL, 10) : 0;
        if (head) {
            wchar_t modified[128] = L"", amount[80] = L"Größe unbekannt";
            size = sizeof modified;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_LAST_MODIFIED, NULL, modified, &size, NULL);
            if (known)
                StringCchPrintfW(amount, ARRAYSIZE(amount), L"%.1f MiB", expected / 1048576.);
            StringCchPrintfW(j->result, ARRAYSIZE(j->result), L"%ls%ls%ls", amount,
                             modified[0] ? L" · " : L"", modified);
            ok = TRUE;
            goto finish;
        }
        uint64_t reserve = j->mode == FILE_JOB ? 256ULL << 20 : 0;
        if (expected > limit || (j->mode == XYZ_JOB && expected > LIMIT_XYZ - j->received)) {
            error(j, L"Größenlimit für den Download überschritten.");
            goto finish;
        }
        if (!disk_space(temp, expected + reserve)) {
            error(j, L"Nicht genügend freier Speicherplatz.");
            goto finish;
        }
        file = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
        if (file == INVALID_HANDLE_VALUE) {
            win_error(j, L"Temporäre Datei konnte nicht geöffnet werden");
            goto finish;
        }
        unsigned char buffer[65536];
        uint64_t got = 0, checked = 0;
        while (!cancelled(j)) {
            DWORD n = 0, written = 0;
            if (!WinHttpReadData(req, buffer, sizeof buffer, &n)) {
                *retry = TRUE;
                win_error(j, L"Download unterbrochen");
                break;
            }
            if (!n) {
                ok = (!known || got == expected) && got != 0;
                if (!ok) {
                    error(j, L"Unvollständiger Download.");
                    *retry = TRUE;
                }
                break;
            }
            if (got + n > limit || (j->mode == XYZ_JOB && j->received + n > LIMIT_XYZ)) {
                error(j, L"Downloadlimit überschritten.");
                break;
            }
            if (reserve && got - checked >= 8 * 1024 * 1024) {
                if (!disk_space(temp, reserve + n)) {
                    error(j, L"Freier Speicherplatz reicht nicht aus.");
                    break;
                }
                checked = got;
            }
            if (!WriteFile(file, buffer, n, &written, NULL) || written != n) {
                win_error(j, L"Datei konnte nicht geschrieben werden");
                break;
            }
            got += n;
            j->received += n;
            progress(j, got, expected);
        }
        if (ok && !FlushFileBuffers(file))
            ok = win_error(j, L"Datei konnte nicht gespeichert werden");
    finish:
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
        if (req)
            WinHttpCloseHandle(req);
        if (conn)
            WinHttpCloseHandle(conn);
        if (!again)
            return ok;
    }
    return error(j, L"Download abgebrochen oder zu viele Weiterleitungen.");
}
static BOOL transfer(Job *j, const wchar_t *url, const wchar_t *target, BOOL tile) {
    wchar_t dir[MAP_PATH], temp[MAP_PATH];
    StringCchCopyW(dir, MAP_PATH, target);
    if (!PathRemoveFileSpecW(dir) || !GetTempFileNameW(dir, L"gv", 0, temp))
        return win_error(j, L"Temporäre Datei konnte nicht erstellt werden");
    BOOL ok = FALSE;
    for (int i = 0; i < 3 && !cancelled(j); i++) {
        BOOL retry;
        ok = download(j, url, temp, tile ? LIMIT_TILE : LIMIT_FILE, FALSE, &retry);
        if (ok || !retry)
            break;
        for (int pause = 0; pause < 5 * (i + 1) && !cancelled(j); pause++)
            Sleep(100);
    }
    if (ok) {
        if (tile) {
            if (!map_valid_tile(temp))
                ok = error(j, L"Die Antwort ist kein gültiges Kartenbild mit 256 × 256 Pixeln.");
        } else {
            MapVector *v = map_vector_open(temp);
            if (!v)
                ok = error(j, L"Ungültige oder unvollständige Mapsforge-Datei.");
            map_vector_close(v);
        }
    }
    if (ok && !cancelled(j)) {
        if (!MoveFileExW(temp, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            ok = win_error(j, L"Zieldatei konnte nicht ersetzt werden");
    } else
        ok = FALSE;
    DeleteFileW(temp);
    return ok;
}
typedef struct {
    int first, last, top, bottom, count;
} Range;
static Range tile_range(Job *j, int z) {
    int n = 1 << z;
    double lat_delta = j->radius / 111.195,
           lon_delta = fmin(180., lat_delta / fmax(.000001, cos(j->lat * MAP_PI / 180.)));
    double left = j->lon - lon_delta, right = j->lon + lon_delta;
    Range r = {.first = (int)floor(map_x(left) * n),
               .last = (int)floor(map_x(right) * n),
               .top = (int)fmax(0., fmin(n - 1., floor(map_y(j->lat + lat_delta) * n))),
               .bottom = (int)fmax(0., fmin(n - 1., floor(map_y(j->lat - lat_delta) * n)))};
    if (r.last - r.first >= n)
        r.last = r.first + n - 1;
    r.count = (r.last - r.first + 1) * (r.bottom - r.top + 1);
    return r;
}
static BOOL url_tile(Job *j, wchar_t *out, int z, int x, int y) {
    size_t n = 0;
    for (const wchar_t *p = j->url; *p;) {
        wchar_t num[32];
        const wchar_t *word = NULL;
        if (!wcsncmp(p, L"{apiKey}", 8)) {
            p += 8;
            word = j->key;
        } else if (!wcsncmp(p, L"{z}", 3) || !wcsncmp(p, L"{x}", 3) || !wcsncmp(p, L"{y}", 3)) {
            StringCchPrintfW(num, 32, L"%d", p[1] == L'z' ? z : p[1] == L'x' ? x : y);
            word = num;
            p += 3;
        }
        if (word) {
            size_t k = wcslen(word);
            if (n + k >= URL_CAP)
                return FALSE;
            memcpy(out + n, word, k * sizeof *out);
            n += k;
        } else {
            if (n + 1 >= URL_CAP)
                return FALSE;
            out[n++] = *p++;
        }
    }
    out[n] = 0;
    return TRUE;
}
static BOOL write_metadata(Job *j, const wchar_t *name, const wchar_t *value) {
    wchar_t target[MAP_PATH], tmp[MAP_PATH];
    if (FAILED(StringCchPrintfW(target, MAP_PATH, L"%ls\\%ls", j->target, name)) ||
        !GetTempFileNameW(j->target, L"gv", 0, tmp))
        return win_error(j, L"Karteninformationen konnten nicht angelegt werden");
    HANDLE file = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    char bytes[2048];
    int n = WideCharToMultiByte(CP_UTF8, 0, value, -1, bytes, sizeof bytes, NULL, NULL);
    DWORD written = 0;
    BOOL ok = file != INVALID_HANDLE_VALUE && n > 0 && WriteFile(file, bytes, (DWORD)n - 1, &written, NULL) &&
              written == (DWORD)n - 1 && FlushFileBuffers(file);
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    if (ok)
        ok = MoveFileExW(tmp, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    DeleteFileW(tmp);
    if (!ok)
        error(j, L"Karteninformationen konnten nicht gespeichert werden.");
    return ok;
}
static BOOL read_metadata(const wchar_t *dir, const wchar_t *name, wchar_t *out, int cap);
static BOOL verify_provider(Job *j) {
    /* Compare a fingerprint so a provider URL containing a private key is never stored on disk. */
    BYTE digest[32];
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    NTSTATUS rc = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (rc >= 0)
        rc = BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0);
    if (rc >= 0)
        rc = BCryptHashData(hash, (BYTE *)j->url, (ULONG)((wcslen(j->url) + 1) * sizeof(wchar_t)), 0);
    if (rc >= 0)
        rc = BCryptHashData(hash, (BYTE *)j->credit, (ULONG)(wcslen(j->credit) * sizeof(wchar_t)), 0);
    if (rc >= 0)
        rc = BCryptFinishHash(hash, digest, sizeof digest, 0);
    if (hash)
        BCryptDestroyHash(hash);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    if (rc < 0)
        return error(j, L"Anbieterkennung konnte nicht erstellt werden.");
    wchar_t expected[65], existing[128];
    for (size_t i = 0; i < sizeof digest; i++)
        StringCchPrintfW(expected + i * 2, 3, L"%02x", digest[i]);
    if (read_metadata(j->target, L"geovoice-source.txt", existing, ARRAYSIZE(existing))) {
        if (wcscmp(expected, existing))
            return error(j, L"Dieser Ordner gehört zu einer anderen Anbieter-URL oder Quellenangabe. Einen "
                            L"neuen, leeren Ordner wählen.");
        return TRUE;
    }
    wchar_t glob[MAP_PATH];
    if (FAILED(StringCchPrintfW(glob, MAP_PATH, L"%ls\\*", j->target)))
        return error(j, L"Ordnerpfad zu lang.");
    WIN32_FIND_DATAW entry;
    HANDLE scan = FindFirstFileW(glob, &entry);
    if (scan == INVALID_HANDLE_VALUE)
        return win_error(j, L"Zielordner ist nicht lesbar");
    BOOL empty = TRUE;
    do {
        if (wcscmp(entry.cFileName, L".") && wcscmp(entry.cFileName, L"..") &&
            _wcsicmp(entry.cFileName, L".geovoice-download.lock")) {
            empty = FALSE;
            break;
        }
    } while (FindNextFileW(scan, &entry));
    FindClose(scan);
    if (!empty)
        return error(j, L"Für einen neuen Download einen leeren Ordner wählen. Bestehende Studio-Downloads "
                        L"mit gleichem Anbieter können fortgesetzt werden.");
    return write_metadata(j, L"geovoice-source.txt", expected);
}
static BOOL directory_exists(const wchar_t *path) {
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}
static BOOL make_dir(const wchar_t *path) { return CreateDirectoryW(path, NULL) || directory_exists(path); }
static BOOL raster_tiles(Job *j) {
    int failures = 0;
    for (int z = j->zmin; z <= j->zmax && !cancelled(j); z++) {
        Range r = tile_range(j, z);
        int n = 1 << z;
        wchar_t level[MAP_PATH];
        if (FAILED(StringCchPrintfW(level, MAP_PATH, L"%ls\\%d", j->target, z)) || !make_dir(level))
            return error(j, L"Zoomordner konnte nicht angelegt werden.");
        for (int col = r.first; col <= r.last && !cancelled(j); col++) {
            int x = (col % n + n) % n;
            wchar_t column[MAP_PATH];
            if (FAILED(StringCchPrintfW(column, MAP_PATH, L"%ls\\%d", level, x)) || !make_dir(column))
                return error(j, L"Kachelordner konnte nicht angelegt werden.");
            for (int y = r.top; y <= r.bottom && !cancelled(j); y++) {
                wchar_t path[MAP_PATH], url[URL_CAP];
                if (FAILED(StringCchPrintfW(path, MAP_PATH, L"%ls\\%d.png", column, y)) ||
                    !url_tile(j, url, z, x, y))
                    return error(j, L"Dateipfad oder URL ist zu lang.");
                if (!map_valid_tile(path)) {
                    if (!transfer(j, url, path, TRUE)) {
                        if (cancelled(j))
                            return FALSE;
                        /* Preserve valid previous tiles and report the specific cause. */
                        failures++;
                        if (failures >= 3 || j->received >= LIMIT_XYZ)
                            goto failed;
                    } else
                        j->loaded++;
                }
                j->done++;
                progress(j, 0, 0);
            }
        }
    }
    if (cancelled(j))
        return FALSE;
    if (failures)
        goto failed;
    wchar_t center[82];
    StringCchPrintfW(center, ARRAYSIZE(center), L"%.8f,%.8f", j->lat, j->lon);
    if (!write_metadata(j, L"center.txt", center))
        return FALSE;
    StringCchPrintfW(j->result, ARRAYSIZE(j->result),
                     L"%d Kacheln verfügbar · %d neu geladen. Der Ordner ist für den Player bereit.",
                     j->done, j->loaded);
    return TRUE;
failed:
    {
        wchar_t why[512];
        StringCchCopyW(why, ARRAYSIZE(why), j->result);
        StringCchPrintfW(j->result, ARRAYSIZE(j->result),
                         L"Unvollständig. %d Kacheln neu, %d fehlgeschlagen. %.340ls", j->loaded, failures,
                         why);
        return FALSE;
    }
}
static BOOL raster(Job *j) {
    wchar_t lockpath[MAP_PATH];
    if (FAILED(StringCchPrintfW(lockpath, MAP_PATH, L"%ls\\.geovoice-download.lock", j->target)))
        return error(j, L"Zielpfad ist zu lang.");
    HANDLE guard = CreateFileW(lockpath, GENERIC_READ | GENERIC_WRITE | DELETE, 0, NULL, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (guard == INVALID_HANDLE_VALUE)
        return win_error(j, L"Zielordner ist belegt oder schreibgeschützt");
    BOOL ok = verify_provider(j) && write_metadata(j, L"attribution.txt", j->credit) && raster_tiles(j);
    CloseHandle(guard);
    return ok;
}
static DWORD WINAPI run_job(void *arg) {
    Job *j = arg;
    j->session =
        WinHttpOpen(L"GeoVoice Studio/1.0 (offline maps)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (j->session) {
        WinHttpSetTimeouts(j->session, 5000, 10000, 10000, 15000);
        if (j->mode == INFO_JOB) {
            BOOL retry;
            j->success = download(j, j->url, NULL, 0, TRUE, &retry);
        } else if (j->mode == FILE_JOB) {
            j->success = transfer(j, j->url, j->target, FALSE);
            if (j->success)
                StringCchCopyW(j->result, ARRAYSIZE(j->result), L"Kartenpaket geprüft und gespeichert.");
        } else
            j->success = raster(j);
        WinHttpCloseHandle(j->session);
        j->session = NULL;
    } else
        win_error(j, L"HTTP-Verbindung konnte nicht initialisiert werden");
    if (cancelled(j)) {
        j->success = FALSE;
        StringCchCopyW(j->result, ARRAYSIZE(j->result),
                       L"Abgebrochen. Bereits vollständig gespeicherte Kacheln sind weiter verwendbar.");
    }
    return 0;
}
static void notice(HWND d, const wchar_t *s) { SetDlgItemTextW(d, M_STATUS, s); }
static BOOL choose_folder(HWND d, wchar_t *path) {
    BROWSEINFOW bi = {.hwndOwner = d,
                      .lpszTitle = L"XYZ-Kartenordner auswählen",
                      .ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE};
    PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi);
    if (!id)
        return FALSE;
    BOOL ok = SHGetPathFromIDListW(id, path);
    CoTaskMemFree(id);
    return ok;
}
static BOOL choose_file(HWND d, wchar_t *path, BOOL save) {
    OPENFILENAMEW ofn = {.lStructSize = sizeof ofn,
                         .hwndOwner = d,
                         .lpstrFilter = L"Mapsforge-Karten (*.map)\0*.map\0\0",
                         .lpstrFile = path,
                         .nMaxFile = MAP_PATH,
                         .lpstrDefExt = L"map",
                         .Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST |
                                  (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST)};
    return save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
}
static BOOL numeric_name(const wchar_t *s, int max, int *out) {
    if (!*s)
        return FALSE;
    unsigned n = 0;
    while (*s) {
        if (*s < L'0' || *s > L'9')
            return FALSE;
        n = 10 * n + *s++ - L'0';
        if (n > (unsigned)max)
            return FALSE;
    }
    *out = (int)n;
    return TRUE;
}
/* Find a real tile to place newly opened XYZ folders at their coverage. */
static BOOL locate_tile(const wchar_t *dir, int depth, int z, int x, Job *j, int *budget) {
    wchar_t glob[MAP_PATH];
    if (FAILED(StringCchPrintfW(glob, MAP_PATH, L"%ls\\*", dir)))
        return FALSE;
    WIN32_FIND_DATAW item;
    HANDLE iter = FindFirstFileW(glob, &item);
    if (iter == INVALID_HANDLE_VALUE)
        return FALSE;
    BOOL found = FALSE;
    do {
        if (--*budget <= 0)
            break;
        if (item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;
        BOOL is_dir = (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (is_dir != (depth < 2))
            continue;
        wchar_t path[MAP_PATH];
        if (FAILED(StringCchPrintfW(path, MAP_PATH, L"%ls\\%ls", dir, item.cFileName)))
            continue;
        int val;
        if (depth == 2) {
            wchar_t *dot = wcsrchr(item.cFileName, L'.');
            if (!dot || (_wcsicmp(dot, L".png") && _wcsicmp(dot, L".jpg") && _wcsicmp(dot, L".jpeg")))
                continue;
            *dot = 0;
            if (!numeric_name(item.cFileName, (1 << z) - 1, &val) || !map_valid_tile(path))
                continue;
            j->zmin = z;
            j->lat = map_lat((val + .5) / (1 << z));
            j->lon = map_lon((x + .5) / (1 << z));
            found = TRUE;
        } else if (numeric_name(item.cFileName, depth ? (1 << z) - 1 : 20, &val))
            found = locate_tile(path, depth + 1, depth ? z : val, depth ? val : 0, j, budget);
    } while (!found && FindNextFileW(iter, &item));
    FindClose(iter);
    return found;
}
static BOOL read_metadata(const wchar_t *dir, const wchar_t *name, wchar_t *out, int cap) {
    wchar_t path[MAP_PATH];
    if (FAILED(StringCchPrintfW(path, MAP_PATH, L"%ls\\%ls", dir, name)))
        return FALSE;
    HANDLE f =
        CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return FALSE;
    char bytes[2048];
    DWORD n = 0;
    BOOL ok = ReadFile(f, bytes, sizeof bytes - 1, &n, NULL);
    CloseHandle(f);
    if (!ok || !n || n == sizeof bytes - 1)
        return FALSE;
    bytes[n] = 0;
    const char *s = bytes;
    if (n >= 3 && !memcmp(s, "\xef\xbb\xbf", 3))
        s += 3;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, cap) > 1;
}
static BOOL coordinate(HWND d, int id, double limit, double *out) {
    wchar_t buf[64];
    GetDlgItemTextW(d, id, buf, ARRAYSIZE(buf));
    for (wchar_t *p = buf; *p; p++)
        if (*p == L',')
            *p = L'.';
    wchar_t *end = NULL;
    double v = wcstod(buf, &end);
    if (end == buf || *end || !isfinite(v) || fabs(v) > limit)
        return FALSE;
    *out = v;
    return TRUE;
}
static BOOL encode_key(Job *j, const wchar_t *name) {
    wchar_t value[512];
    DWORD n = GetEnvironmentVariableW(name, value, ARRAYSIZE(value));
    if (!n || n >= ARRAYSIZE(value))
        return FALSE;
    char bytes[1536];
    int count = WideCharToMultiByte(CP_UTF8, 0, value, n, bytes, sizeof bytes, NULL, NULL);
    size_t used = 0;
    static const wchar_t digits[] = L"0123456789ABCDEF";
    for (int i = 0; i < count; i++) {
        unsigned c = (unsigned char)bytes[i];
        if (used + 3 >= ARRAYSIZE(j->key))
            return FALSE;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            strchr("-._~", (int)c))
            j->key[used++] = (wchar_t)c;
        else {
            j->key[used++] = L'%';
            j->key[used++] = digits[c >> 4];
            j->key[used++] = digits[c & 15];
        }
    }
    j->key[used] = 0;
    SecureZeroMemory(value, sizeof value);
    SecureZeroMemory(bytes, sizeof bytes);
    return TRUE;
}
static BOOL raster_spec(Job *j, BOOL estimate) {
    HWND d = j->dialog;
    if (!coordinate(d, M_LAT, 90, &j->lat) || !coordinate(d, M_LON, 180, &j->lon)) {
        notice(d, L"Gültige Breite (−90 bis 90) und Länge (−180 bis 180) eingeben.");
        return FALSE;
    }
    j->radius = (int)GetDlgItemInt(d, M_RADIUS, NULL, FALSE);
    j->zmin = (int)GetDlgItemInt(d, M_ZMIN, NULL, FALSE);
    j->zmax = (int)GetDlgItemInt(d, M_ZMAX, NULL, FALSE);
    if (j->radius < 1 || j->radius > 5 || j->zmin < 12 || j->zmax > 16 || j->zmin > j->zmax) {
        notice(d, L"Radius 1–5 km und aufsteigende Zoomstufen von 12 bis 16 eingeben.");
        return FALSE;
    }
    j->total = 0;
    for (int z = j->zmin; z <= j->zmax; z++)
        j->total += tile_range(j, z).count;
    if (j->total > 50000) {
        notice(d, L"Mehr als 50.000 Kacheln. Radius oder Zoom verkleinern.");
        return FALSE;
    }
    if (estimate) {
        wchar_t text[200];
        StringCchPrintfW(text, 200, L"%d Kacheln im gewählten Ausschnitt. Limit: 50.000 Kacheln / 2 GiB.",
                         j->total);
        notice(d, text);
        return FALSE;
    }
    GetDlgItemTextW(d, M_TEMPLATE, j->url, URL_CAP);
    GetDlgItemTextW(d, M_CREDIT, j->credit, ARRAYSIZE(j->credit));
    if (!wcsstr(j->url, L"{z}") || !wcsstr(j->url, L"{x}") || !wcsstr(j->url, L"{y}")) {
        notice(d, L"Die XYZ-URL muss {z}, {x} und {y} enthalten.");
        return FALSE;
    }
    if (!j->credit[0] || !IsDlgButtonChecked(d, M_PERMISSION)) {
        notice(d, L"Quellenangabe eintragen und Offline-Erlaubnis bestätigen.");
        return FALSE;
    }
    if (wcsstr(j->url, L"{apiKey}")) {
        wchar_t env[128];
        GetDlgItemTextW(d, M_ENV, env, ARRAYSIZE(env));
        if (!encode_key(j, env)) {
            notice(d, L"Die Umgebungsvariable für den API-Key fehlt oder ihr Inhalt ist zu lang.");
            return FALSE;
        }
        if (_wcsnicmp(j->url, L"https://", 8)) {
            notice(d, L"Für die Übertragung eines API-Keys ist HTTPS erforderlich.");
            return FALSE;
        }
    }
    URL_COMPONENTSW components;
    wchar_t url[URL_CAP], host[256], resource[URL_CAP];
    if (!url_tile(j, url, j->zmin, 0, 0) || !split_url(url, &components, host, resource)) {
        notice(d, L"Ungültige oder für Offline-Downloads gesperrte Anbieter-URL.");
        return FALSE;
    }
    return choose_folder(d, j->target);
}
static void controls(HWND d, BOOL idle) {
    for (int id = M_OPEN; id <= M_RASTER; id++)
        EnableWindow(GetDlgItem(d, id), idle);
    EnableWindow(GetDlgItem(d, M_ESTIMATE), idle);
    EnableWindow(GetDlgItem(d, M_ABORT), !idle);
}
static void begin(Job *j, Mode mode) {
    HWND d = j->dialog;
    j->mode = mode;
    j->key[0] = 0;
    j->target[0] = 0;
    j->result[0] = 0;
    if (mode == XYZ_JOB) {
        if (!raster_spec(j, FALSE))
            return;
    } else {
        GetDlgItemTextW(d, M_URL, j->url, URL_CAP);
        if (!j->url[0]) {
            notice(d, L"Paket-URL eingeben.");
            return;
        }
        if (mode == FILE_JOB) {
            int ix = (int)SendDlgItemMessageW(d, M_COUNTRY, CB_GETCURSEL, 0, 0);
            if (ix < 0 || ix >= (int)ARRAYSIZE(slugs))
                ix = 0;
            StringCchPrintfW(j->target, MAP_PATH, L"%ls.map", slugs[ix]);
            if (!choose_file(d, j->target, TRUE))
                return;
        }
    }
    j->received = 0;
    j->done = 0;
    j->loaded = 0;
    j->last_update = 0;
    j->success = FALSE;
    InterlockedExchange(&j->cancel, 0);
    report(j, L"Verbindung wird hergestellt …");
    controls(d, FALSE);
    notice(d, L"Verbindung wird hergestellt …");
    j->thread = CreateThread(NULL, 0, run_job, j, 0, NULL);
    if (!j->thread) {
        controls(d, TRUE);
        notice(d, L"Download konnte nicht gestartet werden.");
        return;
    }
    SetTimer(d, 1, 150, NULL);
}
static void finish_job(Job *j) {
    HWND d = j->dialog;
    KillTimer(d, 1);
    CloseHandle(j->thread);
    j->thread = NULL;
    SecureZeroMemory(j->key, sizeof j->key);
    controls(d, TRUE);
    if (j->success && j->mode != INFO_JOB) {
        BOOL activated = j->mode == FILE_JOB
                             ? map_use_file(j->canvas, j->target)
                             : map_use_directory(j->canvas, j->target, j->credit, j->lat, j->lon, j->zmax);
        if (activated)
            SetDlgItemTextW(d, M_ACTIVE, j->target);
        else
            StringCchCopyW(j->result, ARRAYSIZE(j->result),
                           L"Datei gespeichert, die Karte konnte aber nicht geöffnet werden.");
    }
    notice(d, j->result);
    if (j->closing)
        EndDialog(d, 0);
}
static INT_PTR CALLBACK dialog_proc(HWND d, UINT msg, WPARAM wp, LPARAM lp) {
    Job *j = (Job *)GetWindowLongPtrW(d, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG: {
        j = (Job *)lp;
        SetWindowLongPtrW(d, DWLP_USER, (LONG_PTR)j);
        j->dialog = d;
        wchar_t path[MAP_PATH], value[64];
        map_current(j->canvas, path, MAP_PATH, &j->lat, &j->lon);
        SetDlgItemTextW(d, M_ACTIVE, path[0] ? path : L"Keine Karte ausgewählt");
        for (size_t i = 0; i < ARRAYSIZE(countries); i++)
            SendDlgItemMessageW(d, M_COUNTRY, CB_ADDSTRING, 0, (LPARAM)countries[i]);
        SendDlgItemMessageW(d, M_COUNTRY, CB_SETCURSEL, 0, 0);
        SetDlgItemTextW(d, M_URL, MIRROR L"austria.map");
        StringCchPrintfW(value, 64, L"%.6f", j->lat);
        SetDlgItemTextW(d, M_LAT, value);
        StringCchPrintfW(value, 64, L"%.6f", j->lon);
        SetDlgItemTextW(d, M_LON, value);
        SetDlgItemInt(d, M_RADIUS, 2, FALSE);
        SetDlgItemInt(d, M_ZMIN, 12, FALSE);
        SetDlgItemInt(d, M_ZMAX, 16, FALSE);
        SetDlgItemTextW(d, M_CREDIT, MAP_CREDIT);
        controls(d, TRUE);
        return TRUE;
    }
    case WM_TIMER:
        if (j->thread) {
            if (WaitForSingleObject(j->thread, 0) == WAIT_OBJECT_0)
                finish_job(j);
            else {
                EnterCriticalSection(&j->lock);
                notice(d, cancelled(j) ? L"Abbruch läuft …" : j->status);
                LeaveCriticalSection(&j->lock);
            }
        }
        return TRUE;
    case WM_CLOSE:
        SendMessageW(d, WM_COMMAND, IDCANCEL, 0);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDCANCEL:
            if (j->thread) {
                j->closing = TRUE;
                InterlockedExchange(&j->cancel, 1);
                notice(d, L"Abbruch läuft …");
            } else
                EndDialog(d, 0);
            return TRUE;
        case M_ABORT:
            InterlockedExchange(&j->cancel, 1);
            return TRUE;
        }
        if (j->thread)
            return TRUE;
        switch (LOWORD(wp)) {
        case M_COUNTRY:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int i = (int)SendDlgItemMessageW(d, M_COUNTRY, CB_GETCURSEL, 0, 0);
                if (i >= 0 && i < (int)ARRAYSIZE(slugs)) {
                    wchar_t url[256];
                    StringCchPrintfW(url, 256, MIRROR L"%ls.map", slugs[i]);
                    SetDlgItemTextW(d, M_URL, url);
                }
            }
            return TRUE;
        case M_OPEN: {
            wchar_t path[MAP_PATH] = L"";
            if (choose_file(d, path, FALSE)) {
                if (map_use_file(j->canvas, path)) {
                    SetDlgItemTextW(d, M_ACTIVE, path);
                    notice(d, L"Mapsforge-Karte geöffnet.");
                } else
                    notice(d, L"Ungültige oder unvollständige Mapsforge-Karte (Format 3–5, bis 8 GiB).");
            }
            return TRUE;
        }
        case M_FOLDER: {
            wchar_t path[MAP_PATH] = L"";
            if (!choose_folder(d, path))
                return TRUE;
            int budget = 5000;
            if (!locate_tile(path, 0, 0, 0, j, &budget)) {
                notice(d, L"Keine lesbare XYZ-Kachel gefunden. Den Ordner oberhalb der Zoomstufen wählen "
                          L"(z/x/y.png).");
                return TRUE;
            }
            wchar_t center[128], tail;
            double lat, lon;
            if (read_metadata(path, L"center.txt", center, ARRAYSIZE(center)) &&
                swscanf(center, L"%lf,%lf %lc", &lat, &lon, &tail) == 2 && isfinite(lat) && isfinite(lon) &&
                fabs(lat) <= 90 && fabs(lon) <= 180) {
                j->lat = lat;
                j->lon = lon;
            }
            if (!read_metadata(path, L"attribution.txt", j->credit, ARRAYSIZE(j->credit)))
                GetDlgItemTextW(d, M_CREDIT, j->credit, ARRAYSIZE(j->credit));
            if (map_use_directory(j->canvas, path, j->credit, j->lat, j->lon, j->zmin)) {
                SetDlgItemTextW(d, M_ACTIVE, path);
                SetDlgItemTextW(d, M_CREDIT, j->credit);
                notice(d, L"XYZ-Karte geöffnet.");
            }
            return TRUE;
        }
        case M_ESTIMATE:
            raster_spec(j, TRUE);
            return TRUE;
        case M_INFO:
            begin(j, INFO_JOB);
            return TRUE;
        case M_DOWNLOAD:
            begin(j, FILE_JOB);
            return TRUE;
        case M_RASTER:
            begin(j, XYZ_JOB);
            return TRUE;
        }
        break;
    }
    return FALSE;
}
void map_dialog(HWND canvas) {
    Job j = {.canvas = canvas};
    InitializeCriticalSection(&j.lock);
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_MAP), GetAncestor(canvas, GA_ROOT),
                    dialog_proc, (LPARAM)&j);
    if (SUCCEEDED(com))
        CoUninitialize();
    DeleteCriticalSection(&j.lock);
}
