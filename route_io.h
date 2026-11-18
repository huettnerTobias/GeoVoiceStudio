#ifndef GEOVOICE_ROUTE_IO_H
#define GEOVOICE_ROUTE_IO_H

#include <stddef.h>
#include <wchar.h>

#define GV_AUDIO_PATH 1536
typedef struct {
    wchar_t id[256], name[1024], audio[256];
    double lat, lon;
} GvPoint;
typedef struct {
    wchar_t id[256], name[1024];
    GvPoint *points;
    size_t count;
} GvRoute;
typedef struct {
    GvRoute *routes;
    size_t count;
    wchar_t dir[1024];
} GvLibrary;

/* Initialize a zeroed library. Edits to the arrays become durable with gv_save(). */
int gv_open(GvLibrary *lib, const wchar_t *dir);
int gv_save(const GvLibrary *lib);
void gv_free(GvLibrary *lib);
const wchar_t *gv_error(void);
int gv_add_route(GvLibrary *lib, const wchar_t *name);
int gv_add_point(GvRoute *route, const wchar_t *name, double lat, double lon, const wchar_t *audio);
void gv_remove_route(GvLibrary *lib, size_t index);
void gv_remove_point(GvRoute *route, size_t index);
int gv_export(const GvLibrary *lib, size_t index, const wchar_t *path);
int gv_export_all(const GvLibrary *lib, const wchar_t *path);
/* Package file or Java data directory. -1 requests replacement confirmation. */
int gv_import(GvLibrary *lib, const wchar_t *path, int replace);
/* Keep the target library exclusively owned until finish. A prepared plan owns
   its staged files. Finish exactly once, including after a declined replacement. */
typedef struct GvImportPlan GvImportPlan;
int gv_prepare_import(GvLibrary *lib, const wchar_t *path, GvImportPlan **out);
int gv_finish_import(GvImportPlan *plan, int install);
int gv_import_audio(const GvLibrary *lib, const wchar_t *path, wchar_t outName[256]);
int gv_audio_path(const GvLibrary *lib, const wchar_t *name, wchar_t out[GV_AUDIO_PATH]);
int gv_rename_audio(GvLibrary *lib, const wchar_t *oldName, const wchar_t *newName);
int gv_delete_audio(GvLibrary *lib, const wchar_t *name);

#endif
