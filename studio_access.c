#include "studio_ui.h"
#include <bcrypt.h>
#include <objbase.h>
#include <wchar.h>
#include <wctype.h>

typedef struct {
    UCHAR salt[16], hash[32];
} Credential;
typedef struct {
    DWORD version;
    Credential admin, route;
} Credentials;
static BOOL unlocked;
enum { ACCESS_V1 = 1, ACCESS_V2 = 2 };

static BOOL derive(const wchar_t *input, const UCHAR *salt, DWORD version, UCHAR hash[32]) {
    BCRYPT_ALG_HANDLE algorithm = NULL;
    NTSTATUS status =
        BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (status >= 0) {
        status = BCryptDeriveKeyPBKDF2(algorithm, (PUCHAR)input, (ULONG)(wcslen(input) * sizeof(wchar_t)),
                                       (PUCHAR)salt, 16, version == ACCESS_V1 ? 210000 : 600000, hash, 32, 0);
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    return status >= 0;
}

static int read_keys(HWND owner, Credentials *keys) {
    wchar_t path[GV_AUDIO_PATH];
    swprintf(path, ARRAYSIZE(path), L"%ls\\access.dat", studio_library.dir);
    HANDLE handle =
        CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND)
        return 0;
    DWORD length = 0;
    BOOL valid = FALSE;
    if (handle != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION info;
        valid = GetFileType(handle) == FILE_TYPE_DISK && GetFileInformationByHandle(handle, &info) &&
                !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
                info.nFileSizeHigh == 0 && info.nFileSizeLow == sizeof(*keys) &&
                ReadFile(handle, keys, sizeof(*keys), &length, NULL) && length == sizeof(*keys) &&
                (keys->version == ACCESS_V1 || keys->version == ACCESS_V2);
        CloseHandle(handle);
    }
    if (!valid)
        studio_error(owner, L"Die Zugangsdaten des Studios konnten nicht gelesen werden. Bitte die "
                            L"Studio-Bibliothek prüfen.");
    return valid ? 1 : -1;
}

static BOOL save_keys(const Credentials *keys) {
    wchar_t path[GV_AUDIO_PATH], temp[GV_AUDIO_PATH];
    swprintf(path, ARRAYSIZE(path), L"%ls\\access.dat", studio_library.dir);
    GUID id;
    wchar_t unique[40];
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(&id, unique, ARRAYSIZE(unique)))
        return FALSE;
    swprintf(temp, ARRAYSIZE(temp), L"%ls\\access-%ls.tmp", studio_library.dir, unique);
    HANDLE handle = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return FALSE;
    DWORD count = 0;
    BOOL saved = WriteFile(handle, keys, sizeof(*keys), &count, NULL) && count == sizeof(*keys) &&
                 FlushFileBuffers(handle);
    CloseHandle(handle);
    if (saved)
        saved = MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!saved)
        DeleteFileW(temp);
    return saved;
}

typedef struct {
    const wchar_t *label;
    const Credential *credential;
    DWORD version;
} Verify;

static INT_PTR CALLBACK verify_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    Verify *form = (Verify *)GetWindowLongPtrW(wnd, DWLP_USER);
    if (msg == WM_INITDIALOG) {
        form = (Verify *)lp;
        SetWindowLongPtrW(wnd, DWLP_USER, lp);
        SetDlgItemTextW(wnd, ID_PROMPT, form->label);
        SendDlgItemMessageW(wnd, ID_VALUE, EM_SETLIMITTEXT, 127, 0);
        return TRUE;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
        wchar_t value[128];
        UCHAR hash[32];
        GetDlgItemTextW(wnd, ID_VALUE, value, ARRAYSIZE(value));
        BOOL valid = derive(value, form->credential->salt, form->version, hash);
        SecureZeroMemory(value, sizeof(value));
        unsigned mismatch = 0;
        if (valid)
            for (size_t i = 0; i < sizeof(hash); i++)
                mismatch |= hash[i] ^ form->credential->hash[i];
        SecureZeroMemory(hash, sizeof(hash));
        if (!valid || mismatch) {
            studio_error(wnd, L"Die Eingabe konnte nicht bestätigt werden.");
            SetDlgItemTextW(wnd, ID_VALUE, L"");
            SetFocus(GetDlgItem(wnd, ID_VALUE));
        } else
            EndDialog(wnd, TRUE);
        return TRUE;
    }
    if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL)) {
        EndDialog(wnd, FALSE);
        return TRUE;
    }
    return FALSE;
}

static BOOL verify(HWND owner, const Credential *credential, DWORD version, const wchar_t *label) {
    Verify form = {label, credential, version};
    return DialogBoxParamW(studio_instance, MAKEINTRESOURCEW(ID_UNLOCK_DLG), owner, verify_proc,
                           (LPARAM)&form) == TRUE;
}

static BOOL create_key(HWND wnd, int field, Credential *key) {
    wchar_t value[128];
    GetDlgItemTextW(wnd, field, value, ARRAYSIZE(value));
    BOOL has_text = FALSE;
    for (const wchar_t *p = value; *p; p++)
        if (!iswspace(*p))
            has_text = TRUE;
    BOOL created =
        has_text &&
        BCryptGenRandom(NULL, key->salt, sizeof(key->salt), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0 &&
        derive(value, key->salt, ACCESS_V2, key->hash);
    SecureZeroMemory(value, sizeof(value));
    return created;
}

static INT_PTR CALLBACK setup_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    if (msg == WM_INITDIALOG) {
        SendDlgItemMessageW(wnd, ID_ADMIN, EM_SETLIMITTEXT, 127, 0);
        SendDlgItemMessageW(wnd, ID_CODE, EM_SETLIMITTEXT, 127, 0);
        return TRUE;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
        Credentials keys = {.version = ACCESS_V2};
        if (!create_key(wnd, ID_ADMIN, &keys.admin) || !create_key(wnd, ID_CODE, &keys.route))
            studio_error(wnd, L"Bitte ein Admin-Passwort und einen Route-Code eingeben.");
        else if (!save_keys(&keys))
            studio_error(wnd, L"Die Zugangsdaten konnten nicht gespeichert werden.");
        else {
            unlocked = TRUE;
            EndDialog(wnd, TRUE);
        }
        SecureZeroMemory(&keys, sizeof(keys));
        return TRUE;
    }
    if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL)) {
        EndDialog(wnd, FALSE);
        return TRUE;
    }
    return FALSE;
}

void studio_security(HWND owner) {
    Credentials keys;
    int result = read_keys(owner, &keys);
    if (result == 0 || (result == 1 && verify(owner, &keys.admin, keys.version, L"Bisheriges Admin-Passwort")))
        DialogBoxParamW(studio_instance, MAKEINTRESOURCEW(ID_SECURITY_DLG), owner, setup_proc, 0);
    SecureZeroMemory(&keys, sizeof(keys));
}

BOOL studio_unlock(HWND owner) {
    if (unlocked)
        return TRUE;
    Credentials keys;
    int result = read_keys(owner, &keys);
    if (result == 0)
        DialogBoxParamW(studio_instance, MAKEINTRESOURCEW(ID_SECURITY_DLG), owner, setup_proc, 0);
    else if (result == 1)
        unlocked = verify(owner, &keys.route, keys.version, L"Route-Code für diese Sitzung");
    SecureZeroMemory(&keys, sizeof(keys));
    return unlocked;
}

void studio_lock(void) { unlocked = FALSE; }
