#include "studio_ui.h"
#include "studio_aerial.h"
#include "studio_style.h"
#include "map.h"
#include <commctrl.h>
#include <shlobj.h>
#include <wchar.h>
#include <stdlib.h>

HINSTANCE studio_instance;
GvLibrary studio_library;

static HWND window, route_box, route_label, table, canvas, title, point_title, map_title, summary, hint, status;
static HWND empty, empty_title, aerial_info, hot_button;
static HFONT normal_font, title_font, section_font, action_font;
static StudioPalette colors;
static RECT points_card, map_card;
static int dpi = 96, route_index = -1, point_index = -1;
static BOOL refreshing, draw_lines = TRUE, draw_names = TRUE;

static int px(int value) { return MulDiv(value, dpi, 96); }
static GvRoute *route(void) {
    return route_index >= 0 && (size_t)route_index < studio_library.count
               ? &studio_library.routes[route_index]
               : NULL;
}
static GvPoint *point(void) {
    GvRoute *r = route();
    return r && point_index >= 0 && (size_t)point_index < r->count ? &r->points[point_index] : NULL;
}

static void message(const wchar_t *text) { SetWindowTextW(status, text); }
static void aerial_status(const wchar_t *text) { SetWindowTextW(aerial_info, text ? text : L""); }
static HFONT font(int size, int weight) {
    return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}
static HWND control(const wchar_t *type, const wchar_t *text, DWORD style, DWORD ex, int id, HFONT f) {
    HWND child = CreateWindowExW(ex, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window,
                                 (HMENU)(INT_PTR)id, studio_instance, NULL);
    SendMessageW(child, WM_SETFONT, (WPARAM)f, FALSE);
    return child;
}
static HWND label(const wchar_t *text, HFONT f) {
    return control(L"STATIC", text, SS_LEFT | SS_NOPREFIX, 0, 0, f);
}
static LRESULT CALLBACK button_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    (void)data;
    /* Owner-drawn buttons otherwise let IsDialogMessage redirect Enter to IDOK. */
    if (msg == WM_GETDLGCODE && wp == VK_RETURN)
        return DefSubclassProc(wnd, msg, wp, lp) | DLGC_WANTMESSAGE;
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        if (!(lp & (1L << 30)))
            SendMessageW(wnd, BM_CLICK, 0, 0);
        return 0;
    }
    if (msg == WM_CHAR && wp == L'\r')
        return 0;
    if (msg == WM_MOUSEMOVE && hot_button != wnd) {
        TRACKMOUSEEVENT track = {.cbSize = sizeof track, .dwFlags = TME_LEAVE, .hwndTrack = wnd};
        if (TrackMouseEvent(&track)) {
            hot_button = wnd;
            InvalidateRect(wnd, NULL, FALSE);
        }
    } else if (msg == WM_MOUSELEAVE || msg == WM_NCDESTROY) {
        if (hot_button == wnd)
            hot_button = NULL;
        if (msg == WM_NCDESTROY)
            RemoveWindowSubclass(wnd, button_proc, id);
        else
            InvalidateRect(wnd, NULL, FALSE);
    }
    return DefSubclassProc(wnd, msg, wp, lp);
}
static void button(int id, const wchar_t *text) {
    HWND button = control(L"BUTTON", text, WS_TABSTOP | BS_OWNERDRAW, 0, id, action_font);
    SetWindowSubclass(button, button_proc, 1, 0);
}
static void place(HWND child, int x, int y, int w, int h) {
    MoveWindow(child, px(x), px(y), px(w), px(h), TRUE);
}
static void place_id(int id, int x, int y, int w, int h) { place(GetDlgItem(window, id), x, y, w, h); }

static void layout(void) {
    RECT r;
    GetClientRect(window, &r);
    int w = MulDiv(r.right, 96, dpi), h = MulDiv(r.bottom, 96, dpi);
    int margin = w >= 1050 ? 20 : 12, gap = 12, pad = w >= 1050 ? 16 : 12;
    int left = min(460, max(315, (w - 2 * margin - gap) * 36 / 100));
    int map_x = margin + left + gap, right = w - margin - map_x, top = 136, bottom = h - 44;
    points_card = (RECT){px(margin), px(top), px(margin + left), px(bottom)};
    map_card = (RECT){px(map_x), px(top), px(w - margin), px(bottom)};
    int lx = margin + pad, lw = left - 2 * pad, mx = map_x + pad, mw = right - 2 * pad;

    place(title, 65, 15, 264, 35);
    place_id(ID_FILE_IMPORT, w - 352, 14, 156, 34);
    place_id(ID_FILE_EXPORT, w - 188, 14, 168, 34);
    place(route_label, 20, 82, 82, 21);
    int choice = min(430, w - 530);
    place(route_box, 105, 77, choice, 310);
    place_id(ID_ROUTE_NEW, 115 + choice, 75, 96, 33);
    place_id(ID_ROUTE_RENAME, 217 + choice, 75, 106, 33);
    place_id(ID_LIBRARY, w - 150, 75, 130, 33);

    place(point_title, lx, top + 14, lw - 145, 29);
    place_id(ID_POINT_NEW, lx + lw - 142, top + 12, 142, 32);
    place(summary, lx, top + 48, lw, 20);
    int table_y = top + 78, table_h = bottom - 91 - table_y;
    place(table, lx, table_y, lw, table_h);
    int empty_y = table_y + max(27, (table_h - 54) / 2);
    place(empty_title, lx + 10, empty_y, lw - 20, 22);
    place(empty, lx + 10, empty_y + 24, lw - 20, 45);
    int move_w = max(50, lw / 5), edit_w = (lw - 2 * move_w - 18) / 2;
    int y = bottom - 80;
    place_id(ID_POINT_EDIT, lx, y, edit_w, 30);
    place_id(ID_POINT_DELETE, lx + edit_w + 6, y, edit_w, 30);
    place_id(ID_UP, lx + lw - 2 * move_w - 6, y, move_w, 30);
    place_id(ID_DOWN, lx + lw - move_w, y, move_w, 30);
    y += 36;
    int listen_w = (lw - 12) * 33 / 100, stop_w = (lw - 12) * 22 / 100;
    place_id(ID_PLAY, lx, y, listen_w, 30);
    place_id(ID_STOP, lx + listen_w + 6, y, stop_w, 30);
    place_id(ID_NEXT, lx + listen_w + stop_w + 12, y, lw - listen_w - stop_w - 12, 30);

    place(map_title, mx, top + 14, mw - 285, 29);
    place_id(ID_MAPS, mx + mw - 282, top + 12, 100, 32);
    place_id(ID_FIT, mx + mw - 176, top + 12, 100, 32);
    place_id(ID_MINUS, mx + mw - 69, top + 12, 32, 32);
    place_id(ID_PLUS, mx + mw - 32, top + 12, 32, 32);
    place(hint, mx, top + 48, mw, 22);
    place(canvas, mx, table_y, mw, bottom - 45 - table_y);
    place(aerial_info, mx, bottom - 38, mw, 34);
    place(status, 20, h - 24, w - 40, 21);
    int client = px(lw) - GetSystemMetrics(SM_CXVSCROLL) - px(2);
    ListView_SetColumnWidth(table, 0, px(38));
    ListView_SetColumnWidth(table, 1, (client - px(38)) * 52 / 100);
    ListView_SetColumnWidth(table, 2, (client - px(38)) * 48 / 100);
    InvalidateRect(window, NULL, FALSE);
}

static void render_points(void) {
    GvRoute *r = route();
    size_t count = r ? r->count : 0;
    MapPoint *markers = count ? calloc(count, sizeof(*markers)) : NULL;
    if (count && !markers) {
        studio_error(window, L"Die Routenpunkte passen nicht in den Arbeitsspeicher.");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        markers[i] = (MapPoint){r->points[i].lat, r->points[i].lon, r->points[i].name};
    }
    map_points(canvas, markers, count, point_index);
    free(markers);
}

static void enable_action(int id, BOOL yes) {
    HWND child = GetDlgItem(window, id);
    if (child)
        EnableWindow(child, yes);
    EnableMenuItem(GetMenu(window), id, MF_BYCOMMAND | (yes ? MF_ENABLED : MF_GRAYED));
}

static void buttons(void) {
    GvRoute *r = route();
    BOOL selected = point() != NULL;
    enable_action(ID_ROUTE_RENAME, r != NULL);
    enable_action(ID_ROUTE_DELETE, r != NULL);
    enable_action(ID_FILE_EXPORT, r && r->count);
    enable_action(ID_ALL_EXPORT, studio_library.count != 0);
    enable_action(ID_POINT_NEW, r != NULL);
    enable_action(ID_POINT_EDIT, selected);
    enable_action(ID_POINT_DELETE, selected);
    enable_action(ID_UP, selected && point_index > 0);
    enable_action(ID_DOWN, selected && (size_t)(point_index + 1) < r->count);
    enable_action(ID_NEXT, r && r->count);
    enable_action(ID_FIT, r && r->count);
    enable_action(ID_PLAY, selected && point()->audio[0]);
    enable_action(ID_STOP, r && r->count);
}

static void refresh(BOOL fit) {
    refreshing = TRUE;
    if (!studio_library.count)
        route_index = -1;
    else if (route_index < 0 || (size_t)route_index >= studio_library.count)
        route_index = (int)studio_library.count - 1;
    SendMessageW(route_box, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < studio_library.count; i++)
        SendMessageW(route_box, CB_ADDSTRING, 0, (LPARAM)studio_library.routes[i].name);
    if (route_index < 0)
        SendMessageW(route_box, CB_ADDSTRING, 0, (LPARAM)L"Noch keine Route");
    SendMessageW(route_box, CB_SETCURSEL, route_index < 0 ? 0 : route_index, 0);
    GvRoute *r = route();
    size_t count = r ? r->count : 0, assigned = 0;
    if (!count)
        studio_stop();
    SetWindowTextW(hint, r ? L"Doppelklick setzt einen Punkt. Mausrad zum Zoomen."
                           : L"Ziehen verschiebt die Karte. Mausrad zum Zoomen.");
    InvalidateRect(GetDlgItem(window, ID_ROUTE_NEW), NULL, FALSE);
    if (!count)
        point_index = -1;
    else if (point_index < 0 || (size_t)point_index >= count)
        point_index = 0;
    SendMessageW(table, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(table);
    for (size_t i = 0; i < count; i++) {
        GvPoint *p = &r->points[i];
        wchar_t number[32];
        swprintf(number, ARRAYSIZE(number), L"%zu", i + 1);
        LVITEMW row = {.mask = LVIF_TEXT, .iItem = (int)i, .pszText = number};
        ListView_InsertItem(table, &row);
        ListView_SetItemText(table, (int)i, 1, p->name);
        if (p->audio[0])
            assigned++;
        ListView_SetItemText(table, (int)i, 2, p->audio[0] ? p->audio : L"Keine Ansage");
    }
    SendMessageW(table, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(table, NULL, TRUE);
    if (point_index >= 0) {
        ListView_SetItemState(table, point_index, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(table, point_index, FALSE);
    }
    SetWindowTextW(empty_title, r ? L"Noch keine Punkte" : L"Noch keine Route");
    SetWindowTextW(empty, r ? L"Setze den ersten Punkt per Doppelklick auf die Karte oder über „Punkt hinzufügen“."
                            : L"Lege eine neue Route an oder importiere ein Routenpaket.");
    ShowWindow(empty_title, count ? SW_HIDE : SW_SHOW);
    ShowWindow(empty, count ? SW_HIDE : SW_SHOW);
    if (!count) {
        SetWindowPos(empty_title, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowPos(empty, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    wchar_t detail[100];
    swprintf(detail, ARRAYSIZE(detail), L"%zu %ls  ·  %zu mit Ansage", count, count == 1 ? L"Punkt" : L"Punkte", assigned);
    SetWindowTextW(summary, detail);
    render_points();
    if (fit && count)
        map_fit(canvas);
    buttons();
    refreshing = FALSE;
    aerial_route(r);
}

static BOOL save(void) {
    if (gv_save(&studio_library)) {
        message(L"Änderung in der Studio-Bibliothek gespeichert.");
        return TRUE;
    }
    studio_error(window, gv_error());
    return FALSE;
}

static void edit_route(BOOL create) {
    GvRoute *r = route();
    if ((!create && !r) || !studio_unlock(window))
        return;
    wchar_t name[1024] = L"";
    if (!create)
        wcscpy(name, r->name);
    if (!studio_name(window, create ? L"Neue Route" : L"Route umbenennen", L"Routenname", name,
                     ARRAYSIZE(name)))
        return;
    if (create) {
        if (!gv_add_route(&studio_library, name)) {
            studio_error(window, gv_error());
            return;
        }
        size_t last = studio_library.count - 1;
        if (!save())
            gv_remove_route(&studio_library, last);
        else {
            route_index = (int)last;
            point_index = -1;
        }
    } else {
        wchar_t previous[1024];
        wcscpy(previous, r->name);
        wcscpy(r->name, name);
        if (!save())
            wcscpy(r->name, previous);
    }
    refresh(FALSE);
}

static void edit_point(BOOL create, double latitude, double longitude) {
    GvRoute *r = route();
    GvPoint *p = point();
    if (!r) {
        message(L"Zuerst eine Route anlegen oder auswählen.");
        return;
    }
    if ((!create && !p) || !studio_unlock(window))
        return;
    GvPoint draft = {0};
    if (create) {
        swprintf(draft.name, ARRAYSIZE(draft.name), L"Punkt %zu", r->count + 1);
        draft.lat = latitude;
        draft.lon = longitude;
    } else
        draft = *p;
    if (!studio_point(window, &draft, create))
        return;
    BOOL point_added = FALSE;
    if (create) {
        if (!gv_add_point(r, draft.name, draft.lat, draft.lon, draft.audio)) {
            studio_error(window, gv_error());
            return;
        }
        if (!save())
            gv_remove_point(r, r->count - 1);
        else {
            point_index = (int)r->count - 1;
            point_added = TRUE;
        }
    } else {
        GvPoint original = *p;
        *p = draft;
        if (!save())
            *p = original;
    }
    refresh(FALSE);
    if (point())
        map_center(canvas, point()->lat, point()->lon);
    if (point_added)
        aerial_point_added(r);
}

static void picked(double lat, double lon, void *context) {
    (void)context;
    edit_point(TRUE, lat, lon);
}

static void move_point(int delta) {
    GvRoute *r = route();
    int next = point_index + delta;
    if (!point() || next < 0 || (size_t)next >= r->count || !studio_unlock(window))
        return;
    GvPoint old = r->points[next];
    r->points[next] = r->points[point_index];
    r->points[point_index] = old;
    if (save())
        point_index = next;
    else {
        r->points[point_index] = r->points[next];
        r->points[next] = old;
    }
    refresh(FALSE);
}

static void remove_selected(BOOL entire) {
    GvRoute *r = route();
    GvPoint *p = point();
    if (!r || (!entire && !p) || !studio_unlock(window))
        return;
    wchar_t confirm[1200];
    swprintf(confirm, ARRAYSIZE(confirm),
             entire ? L"Route „%ls“ mit allen Punkten löschen?" : L"Punkt „%ls“ aus der Route entfernen?",
             entire ? r->name : p->name);
    if (MessageBoxW(window, confirm, entire ? L"Route löschen" : L"Punkt entfernen",
                    MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES)
        return;
    if (entire) {
        GvRoute old = *r;
        size_t tail = studio_library.count - (size_t)route_index - 1;
        memmove(r, r + 1, tail * sizeof(*r));
        studio_library.count--;
        if (save()) {
            aerial_forget(old.id);
            free(old.points);
            point_index = 0;
        } else {
            memmove(r + 1, r, tail * sizeof(*r));
            *r = old;
            studio_library.count++;
        }
    } else {
        GvPoint old = *p;
        size_t tail = r->count - (size_t)point_index - 1;
        memmove(p, p + 1, tail * sizeof(*p));
        r->count--;
        if (!save()) {
            memmove(p + 1, p, tail * sizeof(*p));
            *p = old;
            r->count++;
        } else if (point_index == (int)r->count)
            point_index--;
    }
    refresh(entire);
}

static void choose_point(int index, BOOL center) {
    GvRoute *r = route();
    if (!r || index < 0 || (size_t)index >= r->count)
        return;
    point_index = index;
    refreshing = TRUE;
    ListView_SetItemState(table, index, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
    ListView_EnsureVisible(table, index, FALSE);
    refreshing = FALSE;
    map_select(canvas, index);
    buttons();
    if (center)
        map_center(canvas, r->points[index].lat, r->points[index].lon);
    wchar_t details[160];
    swprintf(details, ARRAYSIZE(details), L"Punkt %d von %zu   ·   %.7f°, %.7f°", index + 1, r->count,
             r->points[index].lat, r->points[index].lon);
    message(details);
}

void studio_transfer(HWND owner, int action, int route_index);

static void command(int id, int event) {
    GvRoute *r = route();
    GvPoint *p = point();
    switch (id) {
    case ID_ROUTE:
        if (event == CBN_SELCHANGE) {
            route_index = (int)SendMessageW(route_box, CB_GETCURSEL, 0, 0);
            point_index = 0;
            studio_stop();
            refresh(TRUE);
            message(L"Route ausgewählt.");
        }
        break;
    case ID_ROUTE_NEW:
    case ID_ROUTE_RENAME:
        edit_route(id == ID_ROUTE_NEW);
        break;
    case ID_ROUTE_DELETE:
    case ID_POINT_DELETE:
        remove_selected(id == ID_ROUTE_DELETE);
        break;
    case ID_POINT_NEW: {
        double lat, lon;
        map_get_center(canvas, &lat, &lon);
        edit_point(TRUE, p ? p->lat : lat, p ? p->lon : lon);
        break;
    }
    case ID_POINT_EDIT:
        edit_point(FALSE, 0, 0);
        break;
    case ID_UP:
    case ID_DOWN:
        move_point(id == ID_UP ? -1 : 1);
        break;
    case ID_NEXT:
        if (r && r->count)
            choose_point((point_index + 1) % (int)r->count, TRUE);
        break;
    case ID_PLAY:
        if (p)
            studio_play(window, p->audio);
        break;
    case ID_STOP:
        studio_stop();
        break;
    case ID_FIT:
        map_fit(canvas);
        break;
    case ID_PLUS:
    case ID_MINUS:
        map_zoom(canvas, id == ID_PLUS ? 1 : -1);
        break;
    case ID_LINE:
    case ID_NAMES:
        if (id == ID_LINE)
            draw_lines = !draw_lines;
        else
            draw_names = !draw_names;
        CheckMenuItem(GetMenu(window), id,
                      MF_BYCOMMAND |
                          ((id == ID_LINE ? draw_lines : draw_names) ? MF_CHECKED : MF_UNCHECKED));
        map_overlay(canvas, draw_lines, draw_names);
        break;
    case ID_LIBRARY:
        studio_audio(window);
        refresh(FALSE);
        break;
    case ID_MAPS:
        map_manage(canvas, route());
        break;
    case ID_AERIAL_SETTINGS:
    case ID_AERIAL_SHOW:
    case ID_AERIAL_FOLDER:
        aerial_command(id, r);
        break;
    case ID_FILE_IMPORT:
    case ID_FILE_LEGACY:
    case ID_FILE_EXPORT:
    case ID_ALL_EXPORT:
        studio_transfer(window, id, route_index);
        break;
    case ID_LOCK:
        studio_lock();
        message(L"Bearbeitung gesperrt.");
        break;
    case ID_SECURITY:
        studio_security(window);
        break;
    case IDOK:
        if (GetFocus() == table && p)
            edit_point(FALSE, 0, 0);
        break;
    case ID_EXIT:
        SendMessageW(window, WM_CLOSE, 0, 0);
        break;
    case ID_ABOUT:
        MessageBoxW(window,
                    L"1. Route anlegen oder eine bestehende Java-Bibliothek über Datei übernehmen.\n\n"
                    L"2. Unter Offlinekarten eine Karte wählen. Punkte per Doppelklick auf die Karte oder "
                    L"über Punkt hinzufügen anlegen. Beim ersten gespeicherten Punkt kannst du automatische "
                    L"Luftbilder aktivieren. Die Einstellung findest du später unter Karte.\n\n"
                    L"3. Jedem Punkt eine WAV- oder MP3-Ansage zuordnen. Die Punktliste bestimmt die "
                    L"Abspielreihenfolge. Mit Anhören und Nächster Punkt testen.\n\n"
                    L"4. Das .geovoice-Paket auf USB exportieren und im Java-Player unter Mehr → Wartung "
                    L"importieren. Die Offlinekarte separat mitnehmen.",
                    L"Mit GeoVoice Studio arbeiten", MB_ICONINFORMATION);
    }
}

void studio_transfer_result(BOOL imported, const wchar_t *text) {
    if (imported) {
        point_index = 0;
        if (studio_library.count)
            route_index = (int)studio_library.count - 1;
        refresh(TRUE);
    }
    message(text);
}

static BOOL build_window(void) {
    normal_font = font(15, FW_NORMAL);
    action_font = font(14, FW_MEDIUM);
    title_font = font(25, FW_SEMIBOLD);
    section_font = font(20, FW_SEMIBOLD);
    title = label(L"GeoVoice Studio", title_font);
    button(ID_FILE_IMPORT, L"Paket importieren");
    button(ID_FILE_EXPORT, L"Route exportieren");
    route_label = label(L"Route", action_font);
    route_box =
        control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 0, ID_ROUTE, normal_font);
    SendMessageW(route_box, CB_SETITEMHEIGHT, (WPARAM)-1, px(28));
    button(ID_ROUTE_NEW, L"Neue Route");
    button(ID_ROUTE_RENAME, L"Umbenennen");
    button(ID_LIBRARY, L"Audiobibliothek");
    point_title = label(L"Routenpunkte", section_font);
    button(ID_POINT_NEW, L"Punkt hinzufügen");
    summary = label(L"", normal_font);
    map_title = label(L"Karte", section_font);
    hint = label(L"Doppelklick setzt einen Punkt. Mausrad zum Zoomen.", normal_font);
    table = control(WC_LISTVIEWW, L"",
                    LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP | LVS_NOSORTHEADER,
                    0, ID_LIST, normal_font);
    ListView_SetExtendedListViewStyle(table, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    HIMAGELIST row_spacing = ImageList_Create(1, px(29), ILC_COLOR32, 0, 1);
    if (row_spacing)
        ListView_SetImageList(table, row_spacing, LVSIL_SMALL);
    const wchar_t *names[] = {L"Nr.", L"Name", L"Ansage"};
    for (int i = 0; i < 3; i++) {
        LVCOLUMNW col = {.mask = LVCF_TEXT | LVCF_WIDTH, .pszText = (LPWSTR)names[i], .cx = 100};
        ListView_InsertColumn(table, i, &col);
    }
    empty_title = control(L"STATIC", L"", SS_CENTER | SS_NOPREFIX, 0, 0, action_font);
    empty = control(L"STATIC", L"", SS_CENTER | SS_NOPREFIX, 0, 0, normal_font);
    button(ID_POINT_EDIT, L"Bearbeiten");
    button(ID_POINT_DELETE, L"Entfernen");
    button(ID_UP, L"Hoch");
    button(ID_DOWN, L"Runter");
    button(ID_PLAY, L"Anhören");
    button(ID_STOP, L"Stopp");
    button(ID_NEXT, L"Nächster Punkt");
    button(ID_MAPS, L"Offlinekarten");
    button(ID_FIT, L"Ganze Route");
    button(ID_MINUS, L"−");
    button(ID_PLUS, L"+");
    canvas = map_create(window, 150, picked, NULL);
    if (!canvas)
        return FALSE;
    aerial_info = label(L"", normal_font);
    aerial_status(L"");
    status = label(L"Bereit", normal_font);
    ListView_SetBkColor(table, colors.panel);
    ListView_SetTextColor(table, colors.text);
    layout();
    aerial_init(window, canvas, studio_library.dir, aerial_status);
    refresh(TRUE);
    return TRUE;
}

static LRESULT CALLBACK window_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_STUDIO_AUTO:
        aerial_notification(route());
        return 0;
    case WM_CREATE:
        window = wnd;
        if (!build_window())
            return -1;
        return 0;
    case WM_SIZE:
        if (canvas && wp != SIZE_MINIMIZED)
            layout();
        return 0;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
        studio_palette(&colors);
        if (table) {
            ListView_SetBkColor(table, colors.panel);
            ListView_SetTextColor(table, colors.text);
        }
        RedrawWindow(wnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT rect;
        HDC dc = BeginPaint(wnd, &ps);
        GetClientRect(wnd, &rect);
        studio_workspace_paint(dc, rect, points_card, map_card, dpi, &colors);
        EndPaint(wnd, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {
        RECT rect;
        GetClientRect(wnd, &rect);
        studio_workspace_paint((HDC)wp, rect, points_card, map_card, dpi, &colors);
        return 0;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *item = (const DRAWITEMSTRUCT *)lp;
        if (item->CtlType == ODT_BUTTON) {
            studio_button_paint(item, dpi, item->hwndItem == hot_button, &colors);
            return TRUE;
        }
        break;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *info = (MINMAXINFO *)lp;
        info->ptMinTrackSize.x = px(860);
        info->ptMinTrackSize.y = px(555);
        return 0;
    }
    case WM_COMMAND:
        command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_NOTIFY: {
        NMHDR *hdr = (NMHDR *)lp;
        if (hdr->idFrom == ID_LIST && hdr->code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW *paint = (NMLVCUSTOMDRAW *)lp;
            if (paint->nmcd.dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
            if (paint->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                if (!(paint->nmcd.uItemState & CDIS_SELECTED))
                    paint->clrTextBk = (paint->nmcd.dwItemSpec & 1) ? colors.stripe : colors.panel;
                return CDRF_DODEFAULT;
            }
        }
        if (hdr->idFrom != ID_LIST || refreshing)
            break;
        if (hdr->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *row = (NMLISTVIEW *)lp;
            if ((row->uChanged & LVIF_STATE) && (row->uNewState & LVIS_SELECTED) &&
                !(row->uOldState & LVIS_SELECTED))
                choose_point(row->iItem, TRUE);
            else if ((row->uChanged & LVIF_STATE) && (row->uOldState & LVIS_SELECTED) &&
                     !(row->uNewState & LVIS_SELECTED) && row->iItem == point_index &&
                     ListView_GetNextItem(table, -1, LVNI_SELECTED) < 0) {
                point_index = -1;
                map_select(canvas, -1);
                buttons();
            }
        } else if (hdr->code == NM_DBLCLK && ((NMITEMACTIVATE *)lp)->iItem >= 0)
            command(ID_POINT_EDIT, 0);
        else if (hdr->code == LVN_KEYDOWN) {
            WORD key = ((NMLVKEYDOWN *)lp)->wVKey;
            if (key == VK_DELETE)
                command(ID_POINT_DELETE, 0);
            if (key == VK_RETURN)
                command(ID_POINT_EDIT, 0);
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND child = (HWND)lp;
        COLORREF bg = child == title ? colors.header : colors.panel;
        BOOL secondary = child == hint || child == summary || child == status || child == empty;
        SetBkColor(dc, bg);
        SetTextColor(dc, child == title ? colors.header_text : secondary ? colors.muted : colors.text);
        SetDCBrushColor(dc, bg);
        return (LRESULT)GetStockObject(DC_BRUSH);
    }
    case WM_DESTROY:
        aerial_stop();
        studio_stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR args, int show) {
    (void)previous;
    (void)args;
    studio_instance = instance;
    if (FAILED(OleInitialize(NULL)))
        return 1;
    INITCOMMONCONTROLSEX controls = {sizeof(controls),
                                     ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_LINK_CLASS};
    InitCommonControlsEx(&controls);
    wchar_t data[1024];
    // This override also permits isolated acceptance runs without touching the user's library.
    DWORD override = GetEnvironmentVariableW(L"GEOVOICE_STUDIO_DIR", data, ARRAYSIZE(data));
    if (override >= ARRAYSIZE(data)) {
        studio_error(NULL, L"Der Bibliothekspfad ist zu lang.");
        return 1;
    }
    if (!override) {
        if (FAILED(
                SHGetFolderPathW(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, data)))
            return 1;
        wcscat(data, L"\\GeoVoiceStudioC");
    }
    if (SHCreateDirectoryExW(NULL, data, NULL) != ERROR_SUCCESS &&
        GetFileAttributesW(data) == INVALID_FILE_ATTRIBUTES) {
        studio_error(NULL, L"Das Datenverzeichnis konnte nicht angelegt werden.");
        return 1;
    }
    wchar_t lock_path[1200];
    swprintf(lock_path, ARRAYSIZE(lock_path), L"%ls\\studio.lock", data);
    HANDLE lock = CreateFileW(lock_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (lock == INVALID_HANDLE_VALUE) {
        studio_error(NULL, GetLastError() == ERROR_SHARING_VIOLATION
                               ? L"Diese Studio-Bibliothek ist bereits geöffnet."
                               : L"Kein Schreibzugriff auf die Studio-Bibliothek.");
        return 1;
    }
    if (!gv_open(&studio_library, data)) {
        studio_error(NULL, gv_error());
        CloseHandle(lock);
        return 1;
    }
    HDC screen = GetDC(NULL);
    dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    studio_palette(&colors);
    WNDCLASSW cls = {.lpfnWndProc = window_proc,
                     .hInstance = instance,
                     .hCursor = LoadCursorW(NULL, IDC_ARROW),
                     .hIcon = LoadIconW(NULL, IDI_APPLICATION),
                     .lpszMenuName = MAKEINTRESOURCEW(ID_FILE_MENU),
                     .lpszClassName = L"GeoVoiceStudioMain"};
    RegisterClassW(&cls);
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int width = min(px(1220), work.right - work.left), height = min(px(780), work.bottom - work.top);
    HWND wnd = CreateWindowExW(
        WS_EX_CONTROLPARENT, cls.lpszClassName, L"GeoVoice Studio", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2,
        width, height, NULL, NULL, instance, NULL);
    int result = 1;
    if (wnd) {
        ShowWindow(wnd, show);
        UpdateWindow(wnd);
        HACCEL accel = LoadAcceleratorsW(instance, MAKEINTRESOURCEW(ID_KEYS));
        MSG msg;
        int got;
        while ((got = GetMessageW(&msg, NULL, 0, 0)) > 0) {
            if (!TranslateAcceleratorW(wnd, accel, &msg) && !IsDialogMessageW(wnd, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        result = got < 0 ? 1 : (int)msg.wParam;
    }
    aerial_destroy();
    gv_free(&studio_library);
    CloseHandle(lock);
    DeleteObject(normal_font);
    DeleteObject(title_font);
    DeleteObject(section_font);
    DeleteObject(action_font);
    OleUninitialize();
    return result;
}
