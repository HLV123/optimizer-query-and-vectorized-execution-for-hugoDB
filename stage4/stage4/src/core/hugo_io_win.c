#ifdef _WIN32
#include "hugo_io.h"
#include <windows.h>
#include <stdlib.h>
struct HugoFile { HANDLE h; };
HugoFile* hugo_open(const char *path, int flags) {
    DWORD access = GENERIC_READ, creation = OPEN_EXISTING;
    if (flags & HUGO_OPEN_RDWR)   access = GENERIC_READ | GENERIC_WRITE;
    if (flags & HUGO_OPEN_CREATE) { access = GENERIC_READ | GENERIC_WRITE; creation = OPEN_ALWAYS; }
    HANDLE h = CreateFileA(path, access, FILE_SHARE_READ, NULL, creation, FILE_FLAG_RANDOM_ACCESS, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    HugoFile *f = (HugoFile*)malloc(sizeof(HugoFile));
    if (!f) { CloseHandle(h); return NULL; }
    f->h = h; return f;
}
static int seek_to(HugoFile *f, uint64_t offset) {
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)offset;
    return SetFilePointerEx(f->h, li, NULL, FILE_BEGIN) ? HUGO_OK : HUGO_ERR_READ;
}
int hugo_read(HugoFile *f, void *buf, size_t size, uint64_t offset) {
    if (!f) return HUGO_ERR_READ;
    if (seek_to(f, offset) != HUGO_OK) return HUGO_ERR_READ;
    size_t total = 0; uint8_t *p = (uint8_t*)buf;
    while (total < size) {
        DWORD n = 0;
        if (!ReadFile(f->h, p + total, (DWORD)(size - total), &n, NULL)) return HUGO_ERR_READ;
        if (n == 0) return HUGO_ERR_SHORT;
        total += n;
    }
    return HUGO_OK;
}
int hugo_write(HugoFile *f, const void *buf, size_t size, uint64_t offset) {
    if (!f) return HUGO_ERR_WRITE;
    LARGE_INTEGER cur; cur.QuadPart = 0;
    GetFileSizeEx(f->h, &cur);
    if ((uint64_t)cur.QuadPart < offset + size) {
        LARGE_INTEGER ns; ns.QuadPart = (LONGLONG)(offset + size);
        SetFilePointerEx(f->h, ns, NULL, FILE_BEGIN); SetEndOfFile(f->h);
    }
    if (seek_to(f, offset) != HUGO_OK) return HUGO_ERR_WRITE;
    size_t total = 0; const uint8_t *p = (const uint8_t*)buf;
    while (total < size) {
        DWORD n = 0;
        if (!WriteFile(f->h, p + total, (DWORD)(size - total), &n, NULL)) return HUGO_ERR_WRITE;
        if (n == 0) return HUGO_ERR_SHORT;
        total += n;
    }
    return HUGO_OK;
}
int hugo_sync(HugoFile *f) { if (!f) return HUGO_ERR_SYNC; if (!FlushFileBuffers(f->h)) return HUGO_ERR_SYNC; return HUGO_OK; }
int64_t hugo_size(HugoFile *f) { if (!f) return -1; LARGE_INTEGER sz; if (!GetFileSizeEx(f->h, &sz)) return -1; return (int64_t)sz.QuadPart; }
void hugo_close(HugoFile *f) { if (!f) return; CloseHandle(f->h); free(f); }
#endif
