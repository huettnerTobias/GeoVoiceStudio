#include "studio_ui.h"
#include "map.h"
#include <commctrl.h>
#include <shlobj.h>
#include <wchar.h>
#include <stdlib.h>

HINSTANCE studio_instance;
GvLibrary studio_library;

static HWND window, route_box, table, canvas, title, point_title, map_title, summary, hint, status, empty;
static HFONT normal_font, title_font, section_font;
static HBRUSH paper;
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
static void button(int id, const wchar_t *text) {
    control(L"BUTTON", text, WS_TABSTOP | BS_PUSHBUTTON, 0, id, normal_font);
}
static void place(HWND child, int x, int y, int w, int h) {
    MoveWindow(child, px(x), px(y), px(w), px(h), TRUE);
}
static void place_id(int id, int x, int y, int w, int h) { place(GetDlgItem(window, id), x, y, w, h); }

static void layout(void) {
    RECT r;
    GetClientRect(window, &r);
    int w = MulDiv(r.right, 96, dpi), h = MulDiv(r.bottom, 96, dpi);
    int left = (w - 54) * 43 / 100, right_x = left + 36, right = w - right_x - 18;
    int btn_w = (left - 12) / 3;
    place(title, 18, 10, 277, 37);
    place_id(ID_LIBRARY, w - 284, 13, 126, 29);
    place_id(ID_MAPS, w - 151, 13, 133, 29);
    place(route_box, 18, 63, w - 470, 300);
    place_id(ID_ROUTE_NEW, w - 442, 61, 118, 30);
    place_id(ID_FILE_IMPORT, w - 317, 61, 141, 30);
    place_id(ID_FILE_EXPORT, w - 169, 61, 151, 30);
    place(point_title, 18, 113, left, 30);
    place(summary, 18, 145, left, 28);
    place(map_title, right_x, 113, right - 178, 30);
    place_id(ID_FIT, w - 192, 110, 100, 28);
    place_id(ID_MINUS, w - 86, 110, 31, 28);
    place_id(ID_PLUS, w - 49, 110, 31, 28);
    place(hint, right_x, 145, right, 26);
    place(table, 18, 176, left, h - 314);
    place(empty, 32, 219, left - 30, 75);
    place(canvas, right_x, 176, right, h - 226);
    const int buttons[9] = {ID_POINT_NEW, ID_POINT_EDIT, ID_POINT_DELETE, ID_UP,          ID_DOWN,
                            ID_NEXT,      ID_PLAY,       ID_STOP,         ID_ROUTE_RENAME};
    for (int i = 0; i < 9; i++)
        place_id(buttons[i], 18 + (i % 3) * (btn_w + 6), h - 130 + (i / 3) * 29, btn_w, 25);
    place(status, 18, h - 32, w - 36, 24);
    int client = px(left) - GetSystemMetrics(SM_CXVSCROLL) - px(6);
    ListView_SetColumnWidth(table, 0, px(36));
    ListView_SetColumnWidth(table, 1, (client - px(36)) * 53 / 100);
    ListView_SetColumnWidth(table, 2, (client - px(36)) * 47 / 100);
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
        ListView_SetItemText(table, (int)i, 2, p->audio[0] ? p->audio : L"—");
    }
    SendMessageW(table, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(table, NULL, TRUE);
    if (point_index >= 0) {
        ListView_SetItemState(table, point_index, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(table, point_index, FALSE);
    }
    SetWindowTextW(empty, r ? L"Ersten Punkt über „Hinzufügen“ oder per Doppelklick auf die Karte setzen."
                            : L"Zum Start eine neue Route anlegen oder ein Routenpaket importieren.");
    ShowWindow(empty, count ? SW_HIDE : SW_SHOW);
    if (!count)
        SetWindowPos(empty, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    wchar_t detail[100];
    swprintf(detail, ARRAYSIZE(detail), L"%zu Punkte  ·  %zu mit Ansage", count, assigned);
    SetWindowTextW(summary, detail);
    render_points();
    if (fit && count)
        map_fit(canvas);
    buttons();
    refreshing = FALSE;
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
    if (create) {
        if (!gv_add_point(r, draft.name, draft.lat, draft.lon, draft.audio)) {
            studio_error(window, gv_error());
            return;
        }
        if (!save())
            gv_remove_point(r, r->count - 1);
        else
            point_index = (int)r->count - 1;
    } else {
        GvPoint original = *p;
        *p = draft;
        if (!save())
            *p = original;
    }
    refresh(FALSE);
    if (point())
        map_center(canvas, point()->lat, point()->lon);
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
    case ID_POINT_NEW:
        edit_point(TRUE, p ? p->lat : 0, p ? p->lon : 0);
        break;
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
        map_manage(canvas);
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
                    L"über Hinzufügen anlegen. Die Punktliste bestimmt die Abspielreihenfolge.\n\n"
                    L"3. Jedem Punkt eine WAV- oder MP3-Ansage zuordnen. Mit Anhören und Weiter testen.\n\n"
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
    title_font = font(27, FW_SEMIBOLD);
    section_font = font(20, FW_SEMIBOLD);
    title = label(L"GeoVoice Studio", title_font);
    route_box =
        control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 0, ID_ROUTE, normal_font);
    button(ID_ROUTE_NEW, L"Neue Route…");
    button(ID_FILE_IMPORT, L"Paket importieren…");
    button(ID_FILE_EXPORT, L"Route exportieren…");
    button(ID_LIBRARY, L"Audiobibliothek…");
    button(ID_MAPS, L"Offlinekarten…");
    point_title = label(L"Routenpunkte", section_font);
    summary = label(L"", normal_font);
    map_title = label(L"Karte", section_font);
    hint = label(L"Doppelklick setzt einen Punkt. Mit dem Mausrad zoomen.", normal_font);
    table = control(WC_LISTVIEWW, L"",
                    LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP | LVS_NOSORTHEADER,
                    WS_EX_CLIENTEDGE, ID_LIST, normal_font);
    ListView_SetExtendedListViewStyle(table, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    const wchar_t *names[] = {L"Nr.", L"Punkt", L"Ansage"};
    for (int i = 0; i < 3; i++) {
        LVCOLUMNW col = {.mask = LVCF_TEXT | LVCF_WIDTH, .pszText = (LPWSTR)names[i], .cx = 100};
        ListView_InsertColumn(table, i, &col);
    }
    empty = label(L"", normal_font);
    button(ID_POINT_NEW, L"Hinzufügen…");
    button(ID_POINT_EDIT, L"Bearbeiten…");
    button(ID_POINT_DELETE, L"Entfernen…");
    button(ID_UP, L"Nach oben");
    button(ID_DOWN, L"Nach unten");
    button(ID_NEXT, L"Weiter");
    button(ID_PLAY, L"Anhören");
    button(ID_STOP, L"Stopp");
    button(ID_ROUTE_RENAME, L"Routenname…");
    button(ID_FIT, L"Ganze Route");
    button(ID_MINUS, L"−");
    button(ID_PLUS, L"+");
    canvas = map_create(window, 150, picked, NULL);
    if (!canvas)
        return FALSE;
    status = label(L"Bereit", normal_font);
    layout();
    refresh(TRUE);
    return TRUE;
}

static LRESULT CALLBACK window_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        window = wnd;
        if (!build_window())
            return -1;
        return 0;
    case WM_SIZE:
        if (canvas)
            layout();
        return 0;
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
        SetBkColor(dc, RGB(250, 251, 252));
        SetTextColor(dc, (HWND)lp == title ? RGB(19, 87, 82) : RGB(37, 53, 61));
        return (LRESULT)paper;
    }
    case WM_DESTROY:
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
                                     ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
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
    paper = CreateSolidBrush(RGB(250, 251, 252));
    WNDCLASSW cls = {.lpfnWndProc = window_proc,
                     .hInstance = instance,
                     .hCursor = LoadCursorW(NULL, IDC_ARROW),
                     .hIcon = LoadIconW(NULL, IDI_APPLICATION),
                     .hbrBackground = paper,
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
    gv_free(&studio_library);
    CloseHandle(lock);
    DeleteObject(normal_font);
    DeleteObject(title_font);
    DeleteObject(section_font);
    DeleteObject(paper);
    OleUninitialize();
    return result;
}
