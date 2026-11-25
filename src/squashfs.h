#ifndef SQUASHFS_H
#define SQUASHFS_H

#include <stddef.h>
#include <stdint.h>

// Reads one small file out of a squashfs 4.0 image without mounting it:
// superblock, the inode and directory tables, then the file's fragment or
// first data block. LZO compression only (what HiBy's and the Sonix images
// use), or blocks stored uncompressed. Up to one block of the file (128 KiB)
// is read: for /usr/resource/hostname and the like.
//
// The image comes through `read`: `len` bytes at offset `off` into `buf`, 0 on
// success. update.c reads it straight from the package's chunk files, so the
// image is never put together anywhere.
typedef int (*squashfs_read_fn)(void *ctx, uint64_t off, void *buf, size_t len);

// `path` relative to the root ("usr/resource/hostname"). Bytes put in out
// (NUL-terminated, at most out_size - 1), or -1 when the file is not there or
// the image cannot be read.
int squashfs_read_file(squashfs_read_fn read, void *ctx, const char *path, char *out, size_t out_size);

#endif /* SQUASHFS_H */
