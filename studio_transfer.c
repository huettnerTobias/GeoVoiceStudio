#include "studio_ui.h"
#include <wchar.h>

typedef struct {
    int action, index, result;
    GvImportPlan *plan;
    wchar_t path[2048], error[1024];
} Transfer;

void studio_transfer_result(BOOL imported, const wchar_t *text);

static void transfer_worker(void *arg) {
    Transfer *job = arg;
    switch (job->action) {
    case ID_FILE_EXPORT:
        job->result = gv_export(&studio_library, (size_t)job->index, job->path);
        break;
    case ID_ALL_EXPORT:
        job->result = gv_export_all(&studio_library, job->path);
        break;
    default:
        job->result = job->plan ? 1 : gv_prepare_import(&studio_library, job->path, &job->plan);
        if (job->result == 1) {
            job->result = gv_finish_import(job->plan, 1);
            job->plan = NULL;
        }
    }
    if (job->result != 1) {
        wcsncpy(job->error, gv_error(), ARRAYSIZE(job->error) - 1);
        job->error[ARRAYSIZE(job->error) - 1] = 0;
    }
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
        const wchar_t *prompt = importing ? L"Routen und Audiodateien werden geprüft und importiert …"
                                          : L"Das Routenpaket wird geschrieben …";
        if (!studio_run_file_job(owner, prompt, transfer_worker, &job)) {
            gv_finish_import(job.plan, 0);
            return;
        }
        if (job.result != -1)
            break;
        if (MessageBoxW(owner, job.error, L"Import bestätigen",
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
            gv_finish_import(job.plan, 0);
            return;
        }
    }
    if (job.result != 1) {
        gv_finish_import(job.plan, 0);
        studio_error(owner, job.error);
        return;
    }
    wchar_t done[2220];
    swprintf(done, ARRAYSIZE(done),
             importing ? L"Importiert aus %ls" : L"Routenpaket gespeichert unter %ls", job.path);
    studio_transfer_result(importing, done);
}
