#include "studio_ui.h"
#include <commdlg.h>
#include <commctrl.h>
#include <process.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <mmsystem.h>
#include <math.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>

static const wchar_t audio_filter[] = L"Ansagen (*.wav, *.mp3)\0*.wav;*.mp3\0";
static const wchar_t package_filter[] = L"GeoVoice-Routenpakete (*.geovoice)\0*.geovoice\0";

void studio_error(HWND owner, const wchar_t *message) {
    MessageBoxW(owner, message, L"GeoVoice Studio", MB_ICONERROR | MB_OK);
}

typedef struct {
    const wchar_t *message;
    void (*run)(void *);
    void *arg;
    HANDLE thread;
} FileJob;

static unsigned __stdcall run_job(void *arg) {
    FileJob *job = arg;
    job->run(job->arg);
    return 0;
}

static INT_PTR CALLBACK file_job_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    FileJob *job = (FileJob *)GetWindowLongPtrW(wnd, DWLP_USER);
    if (msg == WM_INITDIALOG) {
        job = (FileJob *)lp;
        SetWindowLongPtrW(wnd, DWLP_USER, lp);
        SetDlgItemTextW(wnd, ID_PROMPT, job->message);
        SendDlgItemMessageW(wnd, ID_INFO, PBM_SETMARQUEE, TRUE, 30);
        if (SetTimer(wnd, 1, 50, NULL))
            job->thread = (HANDLE)_beginthreadex(NULL, 0, run_job, job, 0, NULL);
        if (!job->thread)
            EndDialog(wnd, FALSE);
        return TRUE;
    }
    if (msg == WM_TIMER && wp == 1 && job && job->thread &&
        WaitForSingleObject(job->thread, 0) == WAIT_OBJECT_0) {
        KillTimer(wnd, 1);
        EndDialog(wnd, TRUE);
        return TRUE;
    }
    return msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL);
}

BOOL studio_run_file_job(HWND owner, const wchar_t *message, void (*run)(void *), void *arg) {
    FileJob job = {.message = message, .run = run, .arg = arg};
    DialogBoxParamW(studio_instance, MAKEINTRESOURCEW(ID_FILE_JOB_DLG), owner, file_job_proc, (LPARAM)&job);
    if (!job.thread) {
        studio_error(owner, L"Der Dateivorgang konnte nicht gestartet werden.");
        return FALSE;
    }
    // Also join if the owning dialog was destroyed while Windows was shutting down.
    WaitForSingleObject(job.thread, INFINITE);
    CloseHandle(job.thread);
    return TRUE;
}

static void trim(wchar_t *s) {
    wchar_t *start = s;
    while (iswspace(*start))
        start++;
    size_t len = wcslen(start);
    while (len && iswspace(start[len - 1]))
        len--;
    wmemmove(s, start, len);
    s[len] = 0;
}

BOOL studio_package_path(HWND owner, wchar_t *path, DWORD size, BOOL save) {
    OPENFILENAMEW picker = {.lStructSize = sizeof(picker),
                            .hwndOwner = owner,
                            .lpstrFile = path,
                            .nMaxFile = size,
                            .lpstrFilter = package_filter,
                            .lpstrDefExt = L"geovoice",
                            .lpstrTitle = save ? L"Routenpaket exportieren" : L"Routenpaket importieren",
                            .Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                                     (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST)};
    return save ? GetSaveFileNameW(&picker) : GetOpenFileNameW(&picker);
}

BOOL studio_folder(HWND owner, wchar_t path[MAX_PATH]) {
    BROWSEINFOW info = {.hwndOwner = owner,
                        .lpszTitle = L"GeoVoice-Bibliothek mit routes.json und Audio auswählen",
                        .ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE};
    PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&info);
    if (!id)
        return FALSE;
    BOOL found = SHGetPathFromIDListW(id, path);
    CoTaskMemFree(id);
    return found;
}

typedef struct {
    const wchar_t *title, *label;
    wchar_t *text;
    size_t capacity;
} NameDialog;

static INT_PTR CALLBACK name_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    NameDialog *form = (NameDialog *)GetWindowLongPtrW(wnd, DWLP_USER);
    if (msg == WM_INITDIALOG) {
        form = (NameDialog *)lp;
        SetWindowLongPtrW(wnd, DWLP_USER, lp);
        SetWindowTextW(wnd, form->title);
        SetDlgItemTextW(wnd, ID_PROMPT, form->label);
        SendDlgItemMessageW(wnd, ID_VALUE, EM_SETLIMITTEXT, form->capacity - 1, 0);
        SetDlgItemTextW(wnd, ID_VALUE, form->text);
        return TRUE;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
        wchar_t entered[1024];
        GetDlgItemTextW(wnd, ID_VALUE, entered, ARRAYSIZE(entered));
        trim(entered);
        if (!entered[0]) {
            studio_error(wnd, L"Bitte einen Namen eingeben.");
            return TRUE;
        }
        wcscpy(form->text, entered);
        EndDialog(wnd, TRUE);
        return TRUE;
    }
    if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL)) {
        EndDialog(wnd, FALSE);
        return TRUE;
    }
    return FALSE;
}

BOOL studio_name(HWND owner, const wchar_t *title, const wchar_t *label, wchar_t *name, size_t capacity) {
    NameDialog form = {title, label, name, capacity};
    return DialogBoxParamW(studio_instance, MAKEINTRESOURCEW(ID_NAME_DLG), owner, name_proc,
                           (LPARAM)&form) == TRUE;
}

void studio_stop(void) { mciSendStringW(L"close gvpreview", NULL, 0, NULL); }

void studio_play(HWND owner, const wchar_t *name) {
    wchar_t path[GV_AUDIO_PATH], command[GV_AUDIO_PATH + 80], error[256];
    studio_stop();
    if (!name[0]) {
        MessageBoxW(owner, L"Diesem Punkt ist noch keine Ansage zugeordnet.", L"Ansage anhören",
                    MB_ICONINFORMATION);
        return;
    }
    if (!gv_audio_path(&studio_library, name, path)) {
        studio_error(owner, gv_error());
        return;
    }
    swprintf(command, ARRAYSIZE(command), L"open \"%ls\" type %ls alias gvpreview", path,
             !_wcsicmp(PathFindExtensionW(name), L".mp3") ? L"mpegvideo" : L"waveaudio");
    MCIERROR result = mciSendStringW(command, NULL, 0, NULL);
    if (!result)
        result = mciSendStringW(L"play gvpreview", NULL, 0, NULL);
    if (result) {
        if (!mciGetErrorStringW(result, error, ARRAYSIZE(error)))
            wcscpy(error, L"Die Ansage konnte nicht abgespielt werden.");
        studio_error(owner, error);
        studio_stop();
    }
}

static void fill_audio(HWND list, BOOL combo, const wchar_t *selected) {
    UINT add = combo ? CB_ADDSTRING : LB_ADDSTRING;
    SendMessageW(list, combo ? CB_RESETCONTENT : LB_RESETCONTENT, 0, 0);
    if (combo)
        SendMessageW(list, add, 0, (LPARAM)L"(Keine Ansage)");
    wchar_t pattern[GV_AUDIO_PATH];
    swprintf(pattern, ARRAYSIZE(pattern), L"%ls\\audio\\*", studio_library.dir);
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const wchar_t *ext = PathFindExtensionW(data.cFileName);
            if (!(data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
                wcslen(data.cFileName) < 256 && (!_wcsicmp(ext, L".wav") || !_wcsicmp(ext, L".mp3")))
                SendMessageW(list, add, 0, (LPARAM)data.cFileName);
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    LRESULT row = -1;
    if (selected && *selected)
        row = SendMessageW(list, combo ? CB_FINDSTRINGEXACT : LB_FINDSTRINGEXACT, -1, (LPARAM)selected);
    // Keep an unresolved assignment visible while an older route is being repaired.
    if (combo && selected && *selected && row < 0)
        row = SendMessageW(list, add, 0, (LPARAM)selected);
    SendMessageW(list, combo ? CB_SETCURSEL : LB_SETCURSEL, row < 0 ? 0 : (WPARAM)row, 0);
}

static BOOL audio_selection(HWND list, BOOL combo, wchar_t name[256]) {
    name[0] = 0;
    LRESULT index = SendMessageW(list, combo ? CB_GETCURSEL : LB_GETCURSEL, 0, 0);
    if (index < 0 || (combo && index == 0))
        return FALSE;
    LRESULT length = SendMessageW(list, combo ? CB_GETLBTEXTLEN : LB_GETTEXTLEN, index, 0);
    if (length < 1 || length >= 256)
        return FALSE;
    SendMessageW(list, combo ? CB_GETLBTEXT : LB_GETTEXT, index, (LPARAM)name);
    return TRUE;
}

typedef struct {
    const wchar_t *files;
    wchar_t last[256], error[1024];
    int imported;
} AudioImport;

static void copy_audio(void *arg) {
    AudioImport *job = arg;
    const wchar_t *file = job->files + wcslen(job->files) + 1;
    BOOL multiple = *file != 0;
    do {
        wchar_t full[GV_AUDIO_PATH];
        const wchar_t *source = job->files;
        if (multiple) {
            if (wcslen(job->files) + wcslen(file) + 2 > ARRAYSIZE(full)) {
                wcscpy(job->error, L"Der Dateipfad ist zu lang.");
                break;
            }
            swprintf(full, ARRAYSIZE(full), L"%ls\\%ls", job->files, file);
            source = full;
        }
        wchar_t name[256];
        if (!gv_import_audio(&studio_library, source, name)) {
            lstrcpynW(job->error, gv_error(), ARRAYSIZE(job->error));
            break;
        }
        wcscpy(job->last, name);
        job->imported++;
        if (multiple) file += wcslen(file) + 1;
    } while (multiple && *file);
}

static BOOL import_audio(HWND owner, BOOL multi, wchar_t last[256]) {
    wchar_t *files = calloc(32769, sizeof(*files));
    if (!files) {
        studio_error(owner, L"Nicht genügend Arbeitsspeicher.");
        return FALSE;
    }
    OPENFILENAMEW picker = {.lStructSize = sizeof(picker),
                            .hwndOwner = owner,
                            .lpstrFile = files,
                            .nMaxFile = 32768,
                            .lpstrFilter = audio_filter,
                            .lpstrTitle = L"Ansagen in die Audiobibliothek importieren",
                            .Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST |
                                     OFN_NOCHANGEDIR | (multi ? OFN_ALLOWMULTISELECT : 0)};
    AudioImport job = {.files = files};
    if (GetOpenFileNameW(&picker)) {
        if (studio_run_file_job(owner, L"Ansagen werden in die Bibliothek kopiert und geprüft …", copy_audio, &job)) {
            if (*job.error) studio_error(owner, job.error);
            if (job.imported) wcscpy(last, job.last);
        }
    } else if (CommDlgExtendedError())
        studio_error(owner, L"Die Dateiauswahl ist zu groß oder konnte nicht geöffnet werden.");
    free(files);
    return job.imported > 0;
}

typedef struct {
    GvPoint *point;
    BOOL is_new;
} PointDialog;

static BOOL coordinate(HWND wnd, int id, double limit, double *value) {
    wchar_t text[64], *end;
    GetDlgItemTextW(wnd, id, text, ARRAYSIZE(text));
    trim(text);
    wchar_t original[64];
    swprintf(original, ARRAYSIZE(original), L"%.15g", *value);
    if (!wcscmp(original, text))
        return TRUE;
    for (wchar_t *p = text; *p; p++)
        if (*p == L',')
            *p = L'.';
    *value = wcstod(text, &end);
    if (end != text && !*end && isfinite(*value) && fabs(*value) <= limit)
        return TRUE;
    studio_error(wnd, id == ID_LAT ? L"Bitte einen Breitengrad zwischen −90 und 90 eingeben."
                                   : L"Bitte einen Längengrad zwischen −180 und 180 eingeben.");
    SetFocus(GetDlgItem(wnd, id));
    return FALSE;
}

static INT_PTR CALLBACK point_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    PointDialog *form = (PointDialog *)GetWindowLongPtrW(wnd, DWLP_USER);
    HWND combo = GetDlgItem(wnd, ID_AUDIO);
    if (msg == WM_INITDIALOG) {
        form = (PointDialog *)lp;
        SetWindowLongPtrW(wnd, DWLP_USER, lp);
        SetWindowTextW(wnd, form->is_new ? L"Routenpunkt hinzufügen" : L"Routenpunkt bearbeiten");
        SendDlgItemMessageW(wnd, ID_VALUE, EM_SETLIMITTEXT, ARRAYSIZE(form->point->name) - 1, 0);
        SendDlgItemMessageW(wnd, ID_LAT, EM_SETLIMITTEXT, 63, 0);
        SendDlgItemMessageW(wnd, ID_LON, EM_SETLIMITTEXT, 63, 0);
        SetDlgItemTextW(wnd, ID_VALUE, form->point->name);
        wchar_t text[64];
        swprintf(text, ARRAYSIZE(text), L"%.15g", form->point->lat);
        SetDlgItemTextW(wnd, ID_LAT, text);
        swprintf(text, ARRAYSIZE(text), L"%.15g", form->point->lon);
        SetDlgItemTextW(wnd, ID_LON, text);
        fill_audio(combo, TRUE, form->point->audio);
        return TRUE;
    }
    if (msg == WM_COMMAND) {
        wchar_t name[256];
        switch (LOWORD(wp)) {
        case ID_ADD_AUDIO:
            if (import_audio(wnd, FALSE, name))
                fill_audio(combo, TRUE, name);
            return TRUE;
        case ID_PLAY:
            audio_selection(combo, TRUE, name);
            studio_play(wnd, name);
            return TRUE;
        case IDOK: {
            GvPoint updated = *form->point;
            GetDlgItemTextW(wnd, ID_VALUE, updated.name, ARRAYSIZE(updated.name));
            trim(updated.name);
            if (!updated.name[0]) {
                studio_error(wnd, L"Bitte einen Punktnamen eingeben.");
                return TRUE;
            }
            // Keep the full stored precision on edits that only change a name or recording.
            if (!coordinate(wnd, ID_LAT, 90, &updated.lat) || !coordinate(wnd, ID_LON, 180, &updated.lon))
                return TRUE;
            audio_selection(combo, TRUE, updated.audio);
            *form->point = updated;
            studio_stop();
            EndDialog(wnd, TRUE);
            return TRUE;
        }
        }
    }
    if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL)) {
        studio_stop();
        EndDialog(wnd, FALSE);
        return TRUE;
    }
    return FALSE;
}

BOOL studio_point(HWND owner, GvPoint *draft, BOOL is_new) {
    PointDialog form = {draft, is_new};
    return DialogBoxParamW(studio_instance, MAKEINTRESOURCEW(ID_POINT_DLG), owner, point_proc,
                           (LPARAM)&form) == TRUE;
}

static INT_PTR CALLBACK audio_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    HWND list = GetDlgItem(wnd, ID_AUDIO);
    wchar_t name[256] = L"";
    if (msg == WM_INITDIALOG) {
        fill_audio(list, FALSE, NULL);
        return TRUE;
    }
    if (msg == WM_COMMAND) {
        switch (LOWORD(wp)) {
        case ID_ADD_AUDIO:
            if (studio_unlock(wnd) && import_audio(wnd, TRUE, name))
                fill_audio(list, FALSE, name);
            return TRUE;
        case ID_AUDIO:
            if (HIWORD(wp) != LBN_DBLCLK)
                break;
            // Double-click previews the selected file.
            /* fall through */
        case ID_PLAY:
            if (audio_selection(list, FALSE, name))
                studio_play(wnd, name);
            return TRUE;
        case ID_STOP:
            studio_stop();
            return TRUE;
        case ID_AUDIO_RENAME: {
            if (!audio_selection(list, FALSE, name) || !studio_unlock(wnd))
                return TRUE;
            wchar_t edited[256];
            wcscpy(edited, name);
            if (studio_name(wnd, L"Ansage umbenennen", L"Dateiname mit Endung", edited,
                            ARRAYSIZE(edited))) {
                studio_stop();
                if (gv_rename_audio(&studio_library, name, edited))
                    fill_audio(list, FALSE, edited);
                else
                    studio_error(wnd, gv_error());
            }
            return TRUE;
        }
        case ID_AUDIO_DELETE:
            if (audio_selection(list, FALSE, name) && studio_unlock(wnd)) {
                wchar_t message[360];
                swprintf(message, ARRAYSIZE(message), L"„%ls“ aus der Audiobibliothek löschen?", name);
                if (MessageBoxW(wnd, message, L"Ansage löschen",
                                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                    studio_stop();
                    if (gv_delete_audio(&studio_library, name))
                        fill_audio(list, FALSE, NULL);
                    else
                        studio_error(wnd, gv_error());
                }
            }
            return TRUE;
        }
    }
    if (msg == WM_CLOSE || (msg == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL))) {
        studio_stop();
        EndDialog(wnd, TRUE);
        return TRUE;
    }
    return FALSE;
}

void studio_audio(HWND owner) {
    DialogBoxParamW(studio_instance, MAKEINTRESOURCEW(ID_AUDIO_DLG), owner, audio_proc, 0);
}
