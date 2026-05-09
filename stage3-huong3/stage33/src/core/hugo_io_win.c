/* hugo_io_win.c — Windows impl (ReadFile/WriteFile + OVERLAPPED)
 *
 * Build trên Windows với MinGW:
 *   gcc -c hugo_io_win.c
 * Trên Linux: KHÔNG compile file này, dùng hugo_io_posix.c.
 */
#ifdef _WIN32

#include "hugo_io.h"
#include <windows.h>
#include <stdlib.h>

struct HugoFile {
    HANDLE h;
};

HugoFile* hugo_open(const char *path, int flags) {
    DWORD access = GENERIC_READ;
    DWORD creation = OPEN_EXISTING;

    if (flags & HUGO_OPEN_RDWR)   access = GENERIC_READ | GENERIC_WRITE;
    if (flags & HUGO_OPEN_CREATE) {
        access = GENERIC_READ | GENERIC_WRITE;
        creation = OPEN_ALWAYS;
    }

    HANDLE h = CreateFileA(path, access, 0, NULL, creation,
                           FILE_FLAG_RANDOM_ACCESS, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    HugoFile *f = (HugoFile*)malloc(sizeof(HugoFile));
    if (!f) { CloseHandle(h); return NULL; }
    f->h = h;
    return f;
}

int hugo_read(HugoFile *f, void *buf, size_t size, uint64_t offset) {
    if (!f) return HUGO_ERR_READ;
    size_t total = 0;
    uint8_t *p = (uint8_t*)buf;
    while (total < size) {
        OVERLAPPED ov = {0};
        ov.Offset     = (DWORD)((offset + total) & 0xFFFFFFFF);
        ov.OffsetHigh = (DWORD)((offset + total) >> 32);
        DWORD n = 0;
        BOOL ok = ReadFile(f->h, p + total, (DWORD)(size - total), &n, &ov);
        if (!ok && GetLastError() != ERROR_HANDLE_EOF) return HUGO_ERR_READ;
        if (n == 0) return HUGO_ERR_SHORT;
        total += n;
    }
    return HUGO_OK;
}

int hugo_write(HugoFile *f, const void *buf, size_t size, uint64_t offset) {
    if (!f) return HUGO_ERR_WRITE;
    size_t total = 0;
    const uint8_t *p = (const uint8_t*)buf;
    while (total < size) {
        OVERLAPPED ov = {0};
        ov.Offset     = (DWORD)((offset + total) & 0xFFFFFFFF);
        ov.OffsetHigh = (DWORD)((offset + total) >> 32);
        DWORD n = 0;
        BOOL ok = WriteFile(f->h, p + total, (DWORD)(size - total), &n, &ov);
        if (!ok) return HUGO_ERR_WRITE;
        if (n == 0) return HUGO_ERR_SHORT;
        total += n;
    }
    return HUGO_OK;
}

int hugo_sync(HugoFile *f) {
    if (!f) return HUGO_ERR_SYNC;
    if (!FlushFileBuffers(f->h)) return HUGO_ERR_SYNC;
    return HUGO_OK;
}

int64_t hugo_size(HugoFile *f) {
    if (!f) return -1;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f->h, &sz)) return -1;
    return (int64_t)sz.QuadPart;
}

void hugo_close(HugoFile *f) {
    if (!f) return;
    CloseHandle(f->h);
    free(f);
}

#endif /* _WIN32 */
