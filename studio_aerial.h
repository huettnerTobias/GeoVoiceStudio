#ifndef GEOVOICE_STUDIO_AERIAL_H
#define GEOVOICE_STUDIO_AERIAL_H
#include "studio_auto.h"

void aerial_init(HWND owner, HWND canvas, const wchar_t *library, void (*status)(const wchar_t *));
void aerial_route(const GvRoute *route);
void aerial_point_added(const GvRoute *route);
void aerial_command(int command, const GvRoute *route);
void aerial_notification(const GvRoute *route);
void aerial_forget(const wchar_t *route_id);
void aerial_stop(void);
void aerial_destroy(void);
#endif
