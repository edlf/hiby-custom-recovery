#ifndef MD5_STREAM_H
#define MD5_STREAM_H

#include <stddef.h>
#include <stdint.h>

// MD5 (RFC 1321), fed in pieces: a partition is far bigger than the RAM. Only
// to check that a copy matches what was read, not against an attacker.

typedef struct {
	uint32_t s[4];
	uint64_t len;
	uint8_t buf[64];
} md5_ctx;

void md5_init(md5_ctx *c);
void md5_update(md5_ctx *c, const void *data, size_t len);
// Lower-case hex, 32 characters and the terminator.
void md5_final_hex(md5_ctx *c, char out[33]);

#endif /* MD5_STREAM_H */
