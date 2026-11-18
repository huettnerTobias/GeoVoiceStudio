#include "studio_aerial.h"
#include "studio_ui.h"
#include "map_orthofoto.h"
#include <commctrl.h>
#include <shellapi.h>
#include <strsafe.h>

enum Preference { UNDECIDED, MANUAL, AUTOMATIC };
static const wchar_t settings_key[] = L"Software\\GeoVoice\\Studio";
static const wchar_t setting[] = L"AutoOrthofotoV1";
static StudioAuto *worker;
static HWND owner_window, map_window;
static void (*show_status)(const wchar_t *);
static enum Preference preference;
static wchar_t visible_route[256];
static ULONGLONG displayed_revision;

static enum Preference read_preference(void) {
    DWORD value, bytes = sizeof value;
    if (RegGetValueW(HKEY_CURRENT_USER, settings_key, setting, RRF_RT_REG_DWORD, NULL, &value, &bytes)
        != ERROR_SUCCESS)
        return UNDECIDED;
    return value == 1 ? AUTOMATIC : value == 0 ? MANUAL : UNDECIDED;
}

static HRESULT CALLBACK open_terms(HWND dialog, UINT message, WPARAM wp, LPARAM lp, LONG_PTR data) {
    (void)wp; (void)data;
    if (message == TDN_HYPERLINK_CLICKED) {
        const wchar_t *url = (const wchar_t *)lp;
        if ((!wcscmp(url, ORTHOFOTO_PROVIDER) || !wcscmp(url, ORTHOFOTO_LICENCE)) &&
            (INT_PTR)ShellExecuteW(dialog, L"open", url, NULL, NULL, SW_SHOWNORMAL) <= 32)
            MessageBoxW(dialog, url, L"Link im Browser öffnen", MB_OK | MB_ICONINFORMATION);
    }
    return S_OK;
}

static enum Preference ask_preference(void) {
    const TASKDIALOG_BUTTON buttons[] = {{IDYES, L"Automatisch laden"}, {IDNO, L"Manuell laden"}};
    const wchar_t *body = L"Studio lädt die Luftbilder entlang der ausgewählten Route im Hintergrund. "
                          L"Der Kartendienst erhält die angefragten Kartenausschnitte und deine IP-Adresse. "
                          L"Die Bilder stehen danach offline zur Verfügung. Deine Auswahl gilt für weitere "
                          L"Routen und nach dem Neustart.\n\n"
                          L"Bei der Weitergabe gehören die Quellenangabe, der Lizenzlink und der Hinweis auf "
                          L"den Routenausschnitt dazu. Studio speichert sie im Luftbildordner.";
    TASKDIALOGCONFIG dialog = {
        .cbSize = sizeof dialog, .hwndParent = owner_window,
        .dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW,
        .pszWindowTitle = L"Automatische Luftbilder",
        .pszMainInstruction = L"Luftbilder für Routenpunkte in Österreich automatisch laden?",
        .pszContent = body, .pButtons = buttons, .cButtons = ARRAYSIZE(buttons),
        .nDefaultButton = preference == AUTOMATIC ? IDYES : IDNO,
        .pszFooter = L"Datenquelle: <a href=\"" ORTHOFOTO_PROVIDER L"\">basemap.at</a>  ·  "
                     L"<a href=\"" ORTHOFOTO_LICENCE L"\">CC BY 4.0</a>\n"
                     L"Änderbar unter Karte → Automatische Luftbilder.",
        .pfCallback = open_terms, .cxWidth = 310
    };
    int choice = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&dialog, &choice, NULL, NULL))) {
        wchar_t text[1024];
        StringCchPrintfW(text, ARRAYSIZE(text), L"%ls\n\n%ls\n\nDatenquelle: basemap.at\n%ls\nCC BY 4.0\n%ls",
                         dialog.pszMainInstruction, body, ORTHOFOTO_PROVIDER, ORTHOFOTO_LICENCE);
        choice = MessageBoxW(owner_window, text, dialog.pszWindowTitle,
                             MB_YESNOCANCEL | MB_DEFBUTTON2 | MB_ICONQUESTION);
    }
    return choice == IDYES ? AUTOMATIC : choice == IDNO ? MANUAL : UNDECIDED;
}

static BOOL cached_directory(const GvRoute *route, wchar_t directory[MAP_PATH]) {
    wchar_t marker[MAP_PATH];
    if (!worker || !route || !studio_auto_directory(worker, route->id, directory) ||
        FAILED(StringCchPrintfW(marker, MAP_PATH, L"%ls\\zoom.txt", directory)))
        return FALSE;
    DWORD attr = GetFileAttributesW(marker);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

void aerial_init(HWND owner, HWND canvas, const wchar_t *library, void (*status)(const wchar_t *)) {
    owner_window = owner;
    map_window = canvas;
    show_status = status;
    visible_route[0] = 0;
    displayed_revision = 0;
    worker = studio_auto_create(owner, library);
    preference = read_preference();
    if (worker)
        studio_auto_enable(worker, preference == AUTOMATIC);
}

void aerial_notification(const GvRoute *route) {
    if (!show_status)
        return;
    wchar_t directory[MAP_PATH];
    BOOL cached = cached_directory(route, directory);
    HMENU menu = GetMenu(owner_window);
    EnableMenuItem(menu, ID_AERIAL_SHOW, MF_BYCOMMAND | (cached && route->count ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, ID_AERIAL_FOLDER, MF_BYCOMMAND | (cached ? MF_ENABLED : MF_GRAYED));
    if (preference != AUTOMATIC) {
        show_status(preference == MANUAL ? L"Luftbilder werden manuell geladen." : L"");
        return;
    }
    if (!worker) {
        show_status(L"Automatischer Luftbild-Download konnte nicht gestartet werden.");
        return;
    }
    StudioAutoUpdate update;
    if (route && route->count && studio_auto_status(worker, route->id, &update)) {
        show_status(update.message);
        if (update.state == AUTO_READY && cached && update.revision != displayed_revision) {
            displayed_revision = update.revision;
            map_show_auto_directory(map_window, directory, FALSE, update.latitude, update.longitude);
        }
    } else
        show_status(L"Automatische Luftbilder aktiviert.");
}

void aerial_route(const GvRoute *route) {
    if (!show_status)
        return;
    const wchar_t *id = route ? route->id : L"";
    if (wcscmp(visible_route, id)) {
        map_leave_auto_directory(map_window);
        StringCchCopyW(visible_route, ARRAYSIZE(visible_route), id);
        displayed_revision = 0;
        wchar_t directory[MAP_PATH];
        if (preference == AUTOMATIC && route && route->count && cached_directory(route, directory))
            map_show_auto_directory(map_window, directory, FALSE, 0, 0);
    }
    if (worker && preference == AUTOMATIC && route && !studio_auto_submit(worker, route)) {
        show_status(L"Die Luftbilder konnten nicht zum Download vorgemerkt werden.");
        return;
    }
    aerial_notification(route);
}

static void configure(const GvRoute *route, BOOL first_point) {
    enum Preference chosen = ask_preference();
    if (chosen == UNDECIDED) {
        if (!first_point) return;
        chosen = MANUAL;
    }
    preference = chosen;
    DWORD value = chosen == AUTOMATIC;
    LONG saved = RegSetKeyValueW(HKEY_CURRENT_USER, settings_key, setting, REG_DWORD, &value, sizeof value);
    if (worker)
        studio_auto_enable(worker, chosen == AUTOMATIC);
    displayed_revision = 0;
    aerial_route(route);
    if (saved != ERROR_SUCCESS)
        studio_error(owner_window, L"Die Auswahl gilt für diese Sitzung. Windows konnte die Einstellung "
                                   L"für den nächsten Start nicht speichern.");
}

void aerial_point_added(const GvRoute *route) {
    if (preference == UNDECIDED)
        configure(route, TRUE);
}

void aerial_command(int command, const GvRoute *route) {
    if (command == ID_AERIAL_SETTINGS) {
        configure(route, FALSE);
        return;
    }
    wchar_t directory[MAP_PATH];
    if (!cached_directory(route, directory)) {
        show_status(L"Für diese Route ist noch kein Luftbildordner verfügbar.");
        return;
    }
    if (command == ID_AERIAL_SHOW && route->count)
        map_show_auto_directory(map_window, directory, TRUE, route->points[0].lat, route->points[0].lon);
    else if (command == ID_AERIAL_FOLDER &&
             (INT_PTR)ShellExecuteW(owner_window, L"open", directory, NULL, NULL, SW_SHOWNORMAL) <= 32)
        MessageBoxW(owner_window, directory, L"Luftbildordner im Explorer öffnen", MB_OK | MB_ICONINFORMATION);
}

void aerial_forget(const wchar_t *route_id) {
    if (worker) studio_auto_forget(worker, route_id);
}
void aerial_stop(void) {
    if (worker) studio_auto_stop(worker);
}
void aerial_destroy(void) {
    if (worker) studio_auto_destroy(worker);
    worker = NULL;
    show_status = NULL;
}
