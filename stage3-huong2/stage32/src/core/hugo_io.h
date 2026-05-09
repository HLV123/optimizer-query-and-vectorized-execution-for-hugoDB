/* hugo_io.h — File I/O abstraction layer
 *
 * Linux:   pread/pwrite/fsync
 * Windows: ReadFile/WriteFile + OVERLAPPED, FlushFileBuffers
 *
 * Cùng API, hai impl khác nhau (hugo_io_posix.c / hugo_io_win.c).
 */
#ifndef HUGO_IO_H
#define HUGO_IO_H

#include <stdint.h>
#include <stddef.h>

typedef struct HugoFile HugoFile;

#define HUGO_OPEN_RDONLY  0x01
#define HUGO_OPEN_RDWR    0x02
#define HUGO_OPEN_CREATE  0x04

#define HUGO_OK            0
#define HUGO_ERR_OPEN     -1
#define HUGO_ERR_READ     -2
#define HUGO_ERR_WRITE    -3
#define HUGO_ERR_SYNC     -4
#define HUGO_ERR_SHORT    -5  /* short read/write — không đọc/ghi đủ bytes */

HugoFile* hugo_open(const char *path, int flags);
int       hugo_read (HugoFile *f, void *buf, size_t size, uint64_t offset);
int       hugo_write(HugoFile *f, const void *buf, size_t size, uint64_t offset);
int       hugo_sync (HugoFile *f);
int64_t   hugo_size (HugoFile *f);
void      hugo_close(HugoFile *f);

#endif
