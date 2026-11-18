#include "studio_ui.h"
#include <commctrl.h>
#include <process.h>
#include <wchar.h>

#define ID_TRANSFER_DLG 206
#define WM_TRANSFER_DONE (WM_APP + 41)
typedef struct {
    int action, index, result;
    BOOL replace;
    wchar_t path[2048], error[1024];
    HWND dialog;
    HANDLE worker;
} Transfer;

void studio_transfer_result(BOOL imported, const wchar_t *text);

static unsigned __stdcall transfer_worker(void *arg) {
    Transfer *job = arg;
    switch (job->action) {
    case ID_FILE_EXPORT:
        job->result = gv_export(&studio_library, (size_t)job->index, job->path);
        break;
    case ID_ALL_EXPORT:
        job->result = gv_export_all(&studio_library, job->path);
        break;
    default:
        job->result = gv_import(&studio_library, job->path, job->replace);
    }
    if (job->result != 1) {
        wcsncpy(job->error, gv_error(), ARRAYSIZE(job->error) - 1);
        job->error[ARRAYSIZE(job->error) - 1] = 0;
    }
    PostMessageW(job->dialog, WM_TRANSFER_DONE, 0, 0);
    return 0;
}

static INT_PTR CALLBACK progress(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    Transfer *job = (Transfer *)GetWindowLongPtrW(wnd, DWLP_USER);
    if (msg == WM_INITDIALOG) {
        job = (Transfer *)lp;
        job->dialog = wnd;
        SetWindowLongPtrW(wnd, DWLP_USER, lp);
        BOOL importing = job->action == ID_FILE_IMPORT || job->action == ID_FILE_LEGACY;
        SetDlgItemTextW(wnd, ID_PROMPT,
                        importing ? L"Routen und Audiodateien werden geprüft und importiert …"
                                  : L"Das Routenpaket wird geschrieben …");
        SendDlgItemMessageW(wnd, ID_INFO, PBM_SETMARQUEE, TRUE, 30);
        job->worker = (HANDLE)_beginthreadex(NULL, 0, transfer_worker, job, 0, NULL);
        if (!job->worker) {
            wcscpy(job->error, L"Der Dateivorgang konnte nicht gestartet werden.");
            EndDialog(wnd, 0);
        }
        return TRUE;
    }
    if (msg == WM_TRANSFER_DONE) {
        WaitForSingleObject(job->worker, INFINITE);
        CloseHandle(job->worker);
        job->worker = NULL;
        EndDialog(wnd, 1);
        return TRUE;
    }
    if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL))
        return TRUE;
    return FALSE;
}

void studio_transfer(HWND owner, int action, int index) {
    BOOL importing = action == ID_FILE_IMPORT || action == ID_FILE_LEGACY;
    if (importing && !studio_unlock(owner))
        return;
    Transfer job = {.action = action, .index = index};
    if (action == ID_FILE_LEGACY) {
        if (!studio_folder(owner, job.path))
            return;
    } else {
        if (!importing) {
            if (action == ID_FILE_EXPORT && (index < 0 || (size_t)index >= studio_library.count))
                return;
            const wchar_t *name =
                action == ID_ALL_EXPORT ? L"GeoVoice-Routen" : studio_library.routes[index].name;
            swprintf(job.path, ARRAYSIZE(job.path), L"%.100ls.geovoice", name);
            for (wchar_t *p = job.path; *p; p++)
                if (*p < 32 || wcschr(L"<>:\"/\\|?*", *p))
                    *p = L'_';
        }
        if (!studio_package_path(owner, job.path, ARRAYSIZE(job.path), !importing))
            return;
    }
    studio_stop();
    for (;;) {
        if (DialogBoxParamW(studio_instance, MAKEINTRESOURCEW(ID_TRANSFER_DLG), owner, progress,
                            (LPARAM)&job) == -1) {
            studio_error(owner, L"Der Dateiübertragungsdialog konnte nicht geöffnet werden.");
            return;
        }
        if (job.result != -1 || !importing || job.replace)
            break;
        if (MessageBoxW(owner, job.error, L"Import bestätigen",
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
            return;
        job.replace = TRUE;
    }
    if (job.result != 1) {
        studio_error(owner, job.error);
        return;
    }
    wchar_t done[2220];
    swprintf(done, ARRAYSIZE(done),
             importing ? L"Importiert aus %ls" : L"Routenpaket gespeichert unter %ls", job.path);
    studio_transfer_result(importing, done);
}
