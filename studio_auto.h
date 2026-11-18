#ifndef GEOVOICE_STUDIO_AUTO_H
#define GEOVOICE_STUDIO_AUTO_H
#include <windows.h>
#include "route_io.h"

#define WM_STUDIO_AUTO (WM_APP + 517)
typedef struct StudioAuto StudioAuto;
typedef enum { AUTO_WAITING, AUTO_LOADING, AUTO_READY, AUTO_FAILED } StudioAutoState;
typedef struct {
    StudioAutoState state;
    ULONGLONG revision;
    wchar_t directory[2048], message[512];
    int zoom;
    double latitude, longitude;
} StudioAutoUpdate;

/* All UI calls are nonblocking except destroy, which belongs after the message loop. */
StudioAuto *studio_auto_create(HWND owner, const wchar_t *library_dir);
void studio_auto_enable(StudioAuto *worker, BOOL enabled);
BOOL studio_auto_submit(StudioAuto *worker, const GvRoute *route);
void studio_auto_forget(StudioAuto *worker, const wchar_t *route_id);
BOOL studio_auto_status(StudioAuto *worker, const wchar_t *route_id, StudioAutoUpdate *out);
/* Computes the path without creating a directory or contacting the provider. */
BOOL studio_auto_directory(const StudioAuto *worker, const wchar_t *route_id, wchar_t out[2048]);
void studio_auto_stop(StudioAuto *worker);
void studio_auto_destroy(StudioAuto *worker);
#endif
