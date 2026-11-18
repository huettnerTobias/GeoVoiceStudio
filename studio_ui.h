#ifndef GEOVOICE_STUDIO_UI_H
#define GEOVOICE_STUDIO_UI_H
#define ID_FILE_IMPORT 101
#define ID_FILE_EXPORT 102
#define ID_FILE_LEGACY 103
#define ID_ROUTE_NEW 104
#define ID_ROUTE_RENAME 105
#define ID_ROUTE_DELETE 106
#define ID_LIBRARY 107
#define ID_MAPS 108
#define ID_ROUTE 109
#define ID_LIST 110
#define ID_POINT_NEW 111
#define ID_POINT_EDIT 112
#define ID_POINT_DELETE 113
#define ID_UP 114
#define ID_DOWN 115
#define ID_PLAY 116
#define ID_STOP 117
#define ID_NEXT 118
#define ID_FIT 119
#define ID_PLUS 120
#define ID_MINUS 121
#define ID_LINE 122
#define ID_NAMES 123
#define ID_EXIT 124
#define ID_SECURITY 125
#define ID_LOCK 126
#define ID_ALL_EXPORT 127
#define ID_ABOUT 128
#define ID_AERIAL_SETTINGS 129
#define ID_AERIAL_SHOW 130
#define ID_AERIAL_FOLDER 131
#define ID_NAME_DLG 201
#define ID_POINT_DLG 202
#define ID_AUDIO_DLG 203
#define ID_SECURITY_DLG 204
#define ID_UNLOCK_DLG 205
#define ID_FILE_JOB_DLG 206
#define ID_VALUE 300
#define ID_LAT 301
#define ID_LON 302
#define ID_AUDIO 303
#define ID_ADD_AUDIO 304
#define ID_AUDIO_RENAME 305
#define ID_AUDIO_DELETE 306
#define ID_PROMPT 307
#define ID_INFO 308
#define ID_ADMIN 309
#define ID_CODE 310
#define ID_FILE_MENU 401
#define ID_KEYS 402

#ifndef RC_INVOKED
#include <windows.h>
#include "route_io.h"

extern HINSTANCE studio_instance;
extern GvLibrary studio_library;
void studio_error(HWND owner, const wchar_t *message);
BOOL studio_run_file_job(HWND owner, const wchar_t *message, void (*run)(void *), void *arg);
BOOL studio_name(HWND owner, const wchar_t *title, const wchar_t *label, wchar_t *name, size_t capacity);
BOOL studio_point(HWND owner, GvPoint *draft, BOOL is_new);
void studio_audio(HWND owner);
void studio_play(HWND owner, const wchar_t *name);
void studio_stop(void);
BOOL studio_unlock(HWND owner);
void studio_lock(void);
void studio_security(HWND owner);
BOOL studio_package_path(HWND owner, wchar_t *path, DWORD size, BOOL save);
BOOL studio_folder(HWND owner, wchar_t path[MAX_PATH]);
#endif
#endif
