/* hugo_io_posix.c — POSIX impl (Linux/macOS)
 * Trên Windows, dùng hugo_io_win.c thay thế.
 */
#include "hugo_io.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <errno.h>

struct HugoFile {
    int fd;
};

HugoFile* hugo_open(const char *path, int flags) {
    int posix_flags = 0;
    if ((flags & HUGO_OPEN_RDWR) || (flags & HUGO_OPEN_CREATE)) {
        posix_flags = O_RDWR;
    } else {
        posix_flags = O_RDONLY;
    }
    if (flags & HUGO_OPEN_CREATE) posix_flags |= O_CREAT;

    int fd = open(path, posix_flags, 0644);
    if (fd < 0) return NULL;

    HugoFile *f = (HugoFile*)malloc(sizeof(HugoFile));
    if (!f) { close(fd); return NULL; }
    f->fd = fd;
    return f;
}

int hugo_read(HugoFile *f, void *buf, size_t size, uint64_t offset) {
    if (!f) return HUGO_ERR_READ;
    size_t total = 0;
    uint8_t *p = (uint8_t*)buf;
    while (total < size) {
        ssize_t n = pread(f->fd, p + total, size - total, offset + total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return HUGO_ERR_READ;
        }
        if (n == 0) return HUGO_ERR_SHORT;  /* EOF before full size */
        total += (size_t)n;
    }
    return HUGO_OK;
}

int hugo_write(HugoFile *f, const void *buf, size_t size, uint64_t offset) {
    if (!f) return HUGO_ERR_WRITE;
    size_t total = 0;
    const uint8_t *p = (const uint8_t*)buf;
    while (total < size) {
        ssize_t n = pwrite(f->fd, p + total, size - total, offset + total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return HUGO_ERR_WRITE;
        }
        if (n == 0) return HUGO_ERR_SHORT;
        total += (size_t)n;
    }
    return HUGO_OK;
}

int hugo_sync(HugoFile *f) {
    if (!f) return HUGO_ERR_SYNC;
    if (fsync(f->fd) != 0) return HUGO_ERR_SYNC;
    return HUGO_OK;
}

int64_t hugo_size(HugoFile *f) {
    if (!f) return -1;
    struct stat st;
    if (fstat(f->fd, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

void hugo_close(HugoFile *f) {
    if (!f) return;
    close(f->fd);
    free(f);
}
