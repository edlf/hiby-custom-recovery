#include "squashfs.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// LZO1X decompression (the format squashfs' "lzo" compressor writes), with
// every read and write checked against the buffers. 0 on success, *out_len
// set to what was produced.

static int lzo1x_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t *out_len) {
	const uint8_t *ip = in, *const ip_end = in + in_len;
	uint8_t *op = out, *const op_end = out + *out_len;
	const uint8_t *m_pos;
	size_t t, next, state = 0;

#define NEED_IP(n) \
	if ((size_t)(ip_end - ip) < (size_t)(n)) \
	return -1
#define NEED_OP(n) \
	if ((size_t)(op_end - op) < (size_t)(n)) \
	return -1
#define TEST_LB(m) \
	if ((m) < out || (m) >= op) \
	return -1

	NEED_IP(3);
	if (*ip > 17) {
		t = *ip++ - 17;
		if (t < 4) {
			next = t;
			goto match_next;
		}
		goto copy_literal_run;
	}
	for (;;) {
		NEED_IP(1);
		t = *ip++;
		if (t < 16) {
			if (state == 0) { // a literal run
				if (t == 0) {
					NEED_IP(1);
					while (*ip == 0) {
						t += 255;
						ip++;
						NEED_IP(1);
					}
					t += 15 + *ip++;
				}
				t += 3;
			copy_literal_run:
				NEED_OP(t);
				NEED_IP(t + 3);
				memcpy(op, ip, t);
				op += t;
				ip += t;
				state = 4;
				continue;
			} else if (state != 4) { // a 2-byte match, near
				NEED_IP(1);
				next = t & 3;
				m_pos = op - 1 - (t >> 2) - ((size_t)*ip++ << 2);
				TEST_LB(m_pos);
				NEED_OP(2);
				op[0] = m_pos[0];
				op[1] = m_pos[1];
				op += 2;
				goto match_next;
			} else { // a 3-byte match, far
				NEED_IP(1);
				next = t & 3;
				m_pos = op - (1 + 0x0800) - (t >> 2) - ((size_t)*ip++ << 2);
				t = 3;
			}
		} else if (t >= 64) { // M2
			NEED_IP(1);
			next = t & 3;
			m_pos = op - 1 - ((t >> 2) & 7) - ((size_t)*ip++ << 3);
			t = (t >> 5) - 1 + (3 - 1);
		} else if (t >= 32) { // M3
			t = (t & 31) + (3 - 1);
			if (t == 2) {
				NEED_IP(1);
				while (*ip == 0) {
					t += 255;
					ip++;
					NEED_IP(1);
				}
				t += 31 + *ip++;
			}
			NEED_IP(2);
			next = (size_t)ip[0] | (size_t)ip[1] << 8;
			ip += 2;
			m_pos = op - 1 - (next >> 2);
			next &= 3;
		} else { // M4, or the end
			m_pos = op - ((size_t)(t & 8) << 11);
			t = (t & 7) + (3 - 1);
			if (t == 2) {
				NEED_IP(1);
				while (*ip == 0) {
					t += 255;
					ip++;
					NEED_IP(1);
				}
				t += 7 + *ip++;
			}
			NEED_IP(2);
			next = (size_t)ip[0] | (size_t)ip[1] << 8;
			ip += 2;
			m_pos -= next >> 2;
			next &= 3;
			if (m_pos == op) {
				*out_len = (size_t)(op - out);
				return t == 3 ? 0 : -1;
			}
			m_pos -= 0x4000;
		}
		TEST_LB(m_pos);
		NEED_OP(t);
		{
			uint8_t *oe = op + t;
			op[0] = m_pos[0];
			op[1] = m_pos[1];
			op += 2;
			m_pos += 2;
			do {
				*op++ = *m_pos++;
			} while (op < oe);
		}
	match_next:
		state = next;
		t = next;
		NEED_IP(t + 3);
		NEED_OP(t);
		while (t > 0) {
			*op++ = *ip++;
			t--;
		}
	}
#undef NEED_IP
#undef NEED_OP
#undef TEST_LB
}

// ---------------------------------------------------------------------------
// The image

#define SQ_MAGIC 0x73717368u
#define SQ_COMP_LZO 3
#define META_SIZE 8192
#define MAX_BLOCK (1u << 20)

typedef struct {
	squashfs_read_fn read;
	void *ctx;
	uint32_t block_size;
	uint64_t bytes_used, inode_table, dir_table, frag_table;
	uint64_t root;
	uint16_t comp;
} sq_t;

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

// A data or fragment block: `size` as squashfs stores it (bit 24 set: stored
// as is). Into out (block_size bytes); its length, or -1.
static long read_block(sq_t *s, uint64_t at, uint32_t size, uint8_t *out) {
	bool raw = size & (1u << 24);
	size &= ~(1u << 24);
	if (size == 0 || size > s->block_size || at + size > s->bytes_used) {
		return -1;
	}
	uint8_t *in = raw ? out : malloc(size);
	if (!in || s->read(s->ctx, at, in, size) != 0) {
		if (!raw) {
			free(in);
		}
		return -1;
	}
	if (raw) {
		return (long)size;
	}
	size_t n = s->block_size;
	int rc = lzo1x_decompress(in, size, out, &n);
	free(in);
	return rc == 0 ? (long)n : -1;
}

// A metadata block at `at`: into out (8 KiB); its length, or -1. *next is
// where the block after it starts.
static long read_meta(sq_t *s, uint64_t at, uint8_t *out, uint64_t *next) {
	uint8_t h[2];
	if (at + 2 > s->bytes_used || s->read(s->ctx, at, h, 2) != 0) {
		return -1;
	}
	uint16_t hdr = le16(h);
	uint32_t size = hdr & 0x7fff;
	bool raw = hdr & 0x8000;
	if (size == 0 || size > META_SIZE || at + 2 + size > s->bytes_used) {
		return -1;
	}
	*next = at + 2 + size;
	if (raw) {
		return s->read(s->ctx, at + 2, out, size) == 0 ? (long)size : -1;
	}
	uint8_t in[META_SIZE];
	if (s->read(s->ctx, at + 2, in, size) != 0) {
		return -1;
	}
	size_t n = META_SIZE;
	return lzo1x_decompress(in, size, out, &n) == 0 ? (long)n : -1;
}

// Reading through a metadata table: `len` bytes from (block at, offset).
typedef struct {
	sq_t *s;
	uint64_t at, next; // this block, the one after
	uint8_t buf[META_SIZE];
	long have;
	size_t pos;
} meta_t;

static int meta_open(meta_t *m, sq_t *s, uint64_t at, size_t offset) {
	m->s = s;
	m->at = at;
	m->have = read_meta(s, at, m->buf, &m->next);
	m->pos = offset;
	return (m->have < 0 || offset > (size_t)m->have) ? -1 : 0;
}

static int meta_read(meta_t *m, void *dst, size_t len) {
	uint8_t *d = dst;
	while (len > 0) {
		if (m->pos == (size_t)m->have) {
			m->at = m->next;
			m->have = read_meta(m->s, m->at, m->buf, &m->next);
			m->pos = 0;
			if (m->have <= 0) {
				return -1;
			}
		}
		size_t k = (size_t)m->have - m->pos < len ? (size_t)m->have - m->pos : len;
		memcpy(d, m->buf + m->pos, k);
		m->pos += k;
		d += k;
		len -= k;
	}
	return 0;
}

// An inode, by reference (metadata block << 16 | offset).
typedef struct {
	uint16_t type;
	// directories
	uint32_t dir_block, dir_size;
	uint16_t dir_offset;
	// files
	uint64_t start, size;
	uint32_t fragment, frag_offset, first_block; // first_block: the first block size, if any
} inode_t;

static int read_inode(sq_t *s, uint64_t ref, inode_t *ino) {
	meta_t m;
	if (meta_open(&m, s, s->inode_table + (ref >> 16), ref & 0xffff) != 0) {
		return -1;
	}
	uint8_t h[16], b[40];
	if (meta_read(&m, h, 16) != 0) {
		return -1;
	}
	memset(ino, 0, sizeof(*ino));
	ino->type = le16(h);
	switch (ino->type) {
	case 1: // basic directory
		if (meta_read(&m, b, 16) != 0) {
			return -1;
		}
		ino->dir_block = le32(b);
		ino->dir_size = le16(b + 8);
		ino->dir_offset = le16(b + 10);
		return 0;
	case 8: // extended directory
		if (meta_read(&m, b, 24) != 0) {
			return -1;
		}
		ino->dir_size = le32(b + 4);
		ino->dir_block = le32(b + 8);
		ino->dir_offset = le16(b + 18);
		return 0;
	case 2: // basic file
		if (meta_read(&m, b, 16) != 0) {
			return -1;
		}
		ino->start = le32(b);
		ino->fragment = le32(b + 4);
		ino->frag_offset = le32(b + 8);
		ino->size = le32(b + 12);
		break;
	case 9: // extended file
		if (meta_read(&m, b, 40) != 0) {
			return -1;
		}
		ino->start = le64(b);
		ino->size = le64(b + 8);
		ino->fragment = le32(b + 28);
		ino->frag_offset = le32(b + 32);
		break;
	default:
		return -1; // not a file or a directory (a link, a device...)
	}
	// the block sizes follow; only the first one is needed
	bool has_frag = ino->fragment != 0xffffffffu;
	uint64_t blocks = has_frag ? ino->size / s->block_size : (ino->size + s->block_size - 1) / s->block_size;
	if (blocks > 0) {
		uint8_t bs[4];
		if (meta_read(&m, bs, 4) != 0) {
			return -1;
		}
		ino->first_block = le32(bs);
	}
	return 0;
}

// `name` (len bytes) in directory `dir`: its inode reference, or -1.
static int64_t dir_lookup(sq_t *s, const inode_t *dir, const char *name, size_t len) {
	if (dir->type != 1 && dir->type != 8) {
		return -1;
	}
	if (dir->dir_size <= 3) {
		return -1; // empty
	}
	meta_t m;
	if (meta_open(&m, s, s->dir_table + dir->dir_block, dir->dir_offset) != 0) {
		return -1;
	}
	uint64_t left = dir->dir_size - 3; // the size counts . and .. as 3 bytes
	while (left >= 12) {
		uint8_t h[12];
		if (meta_read(&m, h, 12) != 0) {
			return -1;
		}
		left -= 12;
		uint32_t count = le32(h) + 1, start = le32(h + 4);
		if (count > 256) {
			return -1;
		}
		for (uint32_t i = 0; i < count; i++) {
			uint8_t e[8];
			char ename[257];
			if (left < 8 || meta_read(&m, e, 8) != 0) {
				return -1;
			}
			size_t nlen = (size_t)le16(e + 6) + 1;
			if (left < 8 + nlen || meta_read(&m, ename, nlen) != 0) {
				return -1;
			}
			left -= 8 + nlen;
			if (nlen == len && memcmp(ename, name, len) == 0) {
				return (int64_t)((uint64_t)start << 16 | le16(e));
			}
		}
	}
	return -1;
}

int squashfs_read_file(squashfs_read_fn read, void *ctx, const char *path, char *out, size_t out_size) {
	sq_t s = {.read = read, .ctx = ctx};
	uint8_t sb[96];
	if (out_size == 0 || read(ctx, 0, sb, sizeof(sb)) != 0 || le32(sb) != SQ_MAGIC || le16(sb + 28) != 4) {
		return -1;
	}
	s.block_size = le32(sb + 12);
	s.comp = le16(sb + 20);
	s.root = le64(sb + 32);
	s.bytes_used = le64(sb + 40);
	s.inode_table = le64(sb + 64);
	s.dir_table = le64(sb + 72);
	s.frag_table = le64(sb + 80);
	if (s.comp != SQ_COMP_LZO || s.block_size == 0 || s.block_size > MAX_BLOCK) {
		return -1;
	}

	inode_t ino;
	if (read_inode(&s, s.root, &ino) != 0) {
		return -1;
	}
	for (const char *p = path; *p;) {
		const char *slash = strchr(p, '/');
		size_t len = slash ? (size_t)(slash - p) : strlen(p);
		int64_t ref = len ? dir_lookup(&s, &ino, p, len) : -1;
		if (ref < 0 || read_inode(&s, (uint64_t)ref, &ino) != 0) {
			return -1;
		}
		p += len + (slash ? 1 : 0);
	}
	if (ino.type != 2 && ino.type != 9) {
		return -1;
	}

	uint8_t *blk = malloc(s.block_size);
	if (!blk) {
		return -1;
	}
	long have, from = 0;
	if (ino.size < s.block_size && ino.fragment != 0xffffffffu) {
		// the whole file in a fragment: its entry in the fragment table
		uint8_t ptr[8], ent[16];
		uint64_t idx = ino.fragment;
		meta_t m;
		if (read(ctx, s.frag_table + 8 * (idx / 512), ptr, 8) != 0 ||
			meta_open(&m, &s, le64(ptr), (size_t)(idx % 512) * 16) != 0 || meta_read(&m, ent, 16) != 0) {
			free(blk);
			return -1;
		}
		have = read_block(&s, le64(ent), le32(ent + 8), blk);
		from = ino.frag_offset;
	} else {
		have = ino.first_block ? read_block(&s, ino.start, ino.first_block, blk) : -1;
	}
	int n = -1;
	if (have >= 0 && from <= have) {
		uint64_t len = ino.size < (uint64_t)(have - from) ? ino.size : (uint64_t)(have - from);
		if (len > out_size - 1) {
			len = out_size - 1;
		}
		memcpy(out, blk + from, (size_t)len);
		out[len] = '\0';
		n = (int)len;
	}
	free(blk);
	return n;
}
