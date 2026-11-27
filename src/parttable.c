#include "parttable.h"

#include <stdio.h>
#include <string.h>

#define PT_HDR 8
#define PT_ENTRY 48
#define PT_NAME 32
#define PT_MAX ((PT_LEN - PT_HDR) / PT_ENTRY)

// The name the duplicate is corrected to, and the one it duplicates.
#define DUP_NAME "rootfs"
#define FIX_NAME "rootfs2"

typedef struct {
	char name[PT_NAME + 1];
	uint32_t size, offset, mask, manager;
} entry_t;

static uint32_t le32(const uint8_t *p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// The number of entries, or -1 when t is not a table.
static int count_of(const uint8_t *t) {
	if (memcmp(t, "nand", 4) != 0) {
		return -1;
	}
	uint32_t n = le32(t + 4);
	return n >= 1 && n <= PT_MAX ? (int)n : -1;
}

static void entry_at(const uint8_t *t, int i, entry_t *e) {
	const uint8_t *p = t + PT_HDR + i * PT_ENTRY;
	memcpy(e->name, p, PT_NAME);
	e->name[PT_NAME] = '\0';
	e->size = le32(p + PT_NAME);
	e->offset = le32(p + PT_NAME + 4);
	e->mask = le32(p + PT_NAME + 8);
	e->manager = le32(p + PT_NAME + 12);
}

pt_result_t pt_check(const uint8_t *have, const uint8_t *want, uint8_t *out, char *why, size_t why_len) {
	int n = count_of(have), wn = count_of(want);

	memcpy(out, have, PT_LEN);
	if (wn < 0) {
		snprintf(why, why_len, "the bundled table is not valid");
		return PT_MISMATCH;
	}
	if (n < 0) {
		snprintf(why, why_len, "no valid table on the player");
		return PT_MISMATCH;
	}
	if (n != wn) {
		snprintf(why, why_len, "%d partitions, %d expected", n, wn);
		return PT_MISMATCH;
	}

	int fixed = -1;
	for (int i = 0; i < n; i++) {
		entry_t h, w;
		entry_at(have, i, &h);
		entry_at(want, i, &w);
		if (h.offset != w.offset || h.size != w.size) {
			snprintf(why, why_len, "partition %d (%s) is 0x%x+0x%x, 0x%x+0x%x expected", i, h.name, h.offset, h.size,
					 w.offset, w.size);
			return PT_MISMATCH;
		}
		if (h.mask != w.mask || h.manager != w.manager) {
			snprintf(why, why_len, "partition %d (%s) has other flags", i, h.name);
			return PT_MISMATCH;
		}
		if (strcmp(h.name, w.name) == 0) {
			continue;
		}
		// The one accepted difference: a second "rootfs" where "rootfs2" is
		// expected (the R1's firmware 2.0 table).
		int dup = 0;
		for (int j = 0; j < i; j++) {
			entry_t e;
			entry_at(have, j, &e);
			dup |= strcmp(e.name, DUP_NAME) == 0;
		}
		if (dup && strcmp(h.name, DUP_NAME) == 0 && strcmp(w.name, FIX_NAME) == 0) {
			fixed = i;
			continue;
		}
		snprintf(why, why_len, "partition %d is \"%s\", \"%s\" expected", i, h.name, w.name);
		return PT_MISMATCH;
	}

	if (fixed < 0) {
		snprintf(why, why_len, "as expected");
		return PT_SAME;
	}
	uint8_t *name = out + PT_HDR + fixed * PT_ENTRY;
	memset(name, 0, PT_NAME);
	memcpy(name, FIX_NAME, sizeof(FIX_NAME) - 1);
	snprintf(why, why_len, "partition %d renamed from \"" DUP_NAME "\" to \"" FIX_NAME "\"", fixed);
	return PT_FIXED;
}
