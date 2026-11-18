#include "map_image.h"
#include <string.h>

GpImage *map_tile_image(const wchar_t *path) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return NULL;
    BY_HANDLE_FILE_INFORMATION info;
    unsigned char header[8];
    DWORD n;
    BOOL accepted = GetFileType(file) == FILE_TYPE_DISK && GetFileInformationByHandle(file, &info) &&
                    !info.nFileSizeHigh && info.nFileSizeLow <= 2 * 1024 * 1024 &&
                    !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
                    ReadFile(file, header, sizeof header, &n, NULL) && n == sizeof header &&
                    (!memcmp(header, "\x89PNG\r\n\x1a\n", 8) || !memcmp(header, "\xff\xd8\xff", 3));
    GpImage *im = NULL;
    UINT w = 0, h = 0;
    GpStatus loaded = accepted ? GdipLoadImageFromFile(path, &im) : UnknownImageFormat;
    CloseHandle(file);
    if (loaded != Ok || !im)
        return NULL;
    if (GdipGetImageWidth(im, &w) != Ok || GdipGetImageHeight(im, &h) != Ok || w != 256 || h != 256) {
        GdipDisposeImage(im);
        return NULL;
    }
    return im;
}
BOOL map_valid_tile(const wchar_t *path) {
    GpImage *im = map_tile_image(path);
    if (!im)
        return FALSE;
    HBITMAP decoded = NULL;
    GpStatus result = GdipCreateHBITMAPFromBitmap((GpBitmap *)im, &decoded, 0);
    if (decoded)
        DeleteObject(decoded);
    GdipDisposeImage(im);
    return result == Ok;
}
