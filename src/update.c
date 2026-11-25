#include "update.h"

#include "device.h"
#include "log.h"
#include "md5.h"
#include "model.h"
#include "squashfs.h"
#include "storage.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// On the PC (make host, see sim_device.c) the package is unpacked with 7z
// instead of loop-mounted, and flash_erase/nandwrite are stood in for by
// sim_device.c's mtd_image_*; everything else is the real code.
#ifdef HOST_BUILD
#define UPT_MOUNT "/tmp/sonix-sim-upt"
#else
#define UPT_MOUNT "/tmp/upt"
#endif
#define MAX_IMAGES 4

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static update_status_t status;

void update_get(update_status_t *out) {
	pthread_mutex_lock(&lock);
	*out = status;
	pthread_mutex_unlock(&lock);
}

static void set_phase(const char *phase, int percent) {
	pthread_mutex_lock(&lock);
	snprintf(status.phase, sizeof(status.phase), "%s", phase);
	status.percent = percent;
	pthread_mutex_unlock(&lock);
	LOG("update: %s", phase);
}

static void set_percent(int percent) {
	pthread_mutex_lock(&lock);
	status.percent = percent;
	pthread_mutex_unlock(&lock);
}

static void finish(update_state_t state, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void finish(update_state_t state, const char *fmt, ...) {
	va_list ap;
	pthread_mutex_lock(&lock);
	va_start(ap, fmt);
	vsnprintf(status.message, sizeof(status.message), fmt, ap);
	va_end(ap);
	status.state = state;
	pthread_mutex_unlock(&lock);
	LOG("update: %s: %s", state == UPDATE_DONE ? "done" : "FAILED", status.message);
}

// ---------------------------------------------------------------------------
// Package parsing

// "key=value" / "key = value" -> value, trimmed. The .in files have CRLF.
static int key_value(const char *line, const char *key, char *out, size_t size) {
	size_t kl = strlen(key);
	while (*line == ' ' || *line == '\t') {
		line++;
	}
	if (strncmp(line, key, kl) != 0) {
		return -1;
	}
	line += kl;
	while (*line == ' ' || *line == '\t') {
		line++;
	}
	if (*line != '=') {
		return -1;
	}
	line++;
	while (*line == ' ' || *line == '\t') {
		line++;
	}
	snprintf(out, size, "%s", line);
	out[strcspn(out, "\r\n")] = '\0';
	size_t n = strlen(out);
	while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t')) {
		out[--n] = '\0';
	}
	return 0;
}

static int file_key(const char *path, const char *key, char *out, size_t size) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return -1;
	}
	char line[256];
	int rc = -1;
	while (fgets(line, sizeof(line), f)) {
		if (key_value(line, key, out, size) == 0) {
			rc = 0; // the last one wins, as in ota_utils.sh
		}
	}
	fclose(f);
	return rc;
}

typedef struct {
	char type[16];
	char name[64];
	long long size;
	char md5[40];
} image_t;

static int parse_images(const char *path, image_t *img, int max) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return -1;
	}
	char line[256], v[128];
	int n = -1;
	while (fgets(line, sizeof(line), f)) {
		if (key_value(line, "img_type", v, sizeof(v)) == 0) {
			if (++n >= max) {
				fclose(f);
				return -1;
			}
			memset(&img[n], 0, sizeof(img[n]));
			snprintf(img[n].type, sizeof(img[n].type), "%s", v);
		} else if (n >= 0 && key_value(line, "img_name", v, sizeof(v)) == 0) {
			snprintf(img[n].name, sizeof(img[n].name), "%s", v);
		} else if (n >= 0 && key_value(line, "img_size", v, sizeof(v)) == 0) {
			img[n].size = atoll(v);
		} else if (n >= 0 && key_value(line, "img_md5", v, sizeof(v)) == 0) {
			snprintf(img[n].md5, sizeof(img[n].md5), "%s", v);
		}
	}
	fclose(f);
	return n + 1;
}


// The chunks of an image: <name>.0000.<img_md5>, then each one named after
// the md5 of the one before it (<name>.0001.<md5 of chunk 0>, ...), every one
// 512 KiB but the last. ota_md5_<name>.<img_md5> has their md5s, one per line.
#define MAX_CHUNKS 4096
#define CHUNK_BUF (64 * 1024)

static int read_md5_list(const char *dir, const image_t *im, char (*list)[33], int max) {
	char path[512];
	snprintf(path, sizeof(path), "%s/ota_md5_%s.%s", dir, im->name, im->md5);
	FILE *f = fopen(path, "r");
	if (!f) {
		return -1;
	}
	char line[128];
	int n = 0;
	while (n < max && fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%32s", list[n]) == 1 && strlen(list[n]) == 32) {
			n++;
		}
	}
	fclose(f);
	return n;
}

// CRC-32 (IEEE, as zlib and U-Boot have it), fed in pieces.
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
	static uint32_t table[256];
	if (!table[1]) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++) {
				c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
			}
			table[i] = c;
		}
	}
	crc = ~crc;
	while (n--) {
		crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	}
	return ~crc;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// The kernel has to be a uImage the SPL can boot: it reads the legacy header,
// copies header + data to load - 64 and jumps to the entry point without
// checking anything (a bad one leaves the player hanging at power-on). The
// same rules as fastboot's fb_check_kernel_image() (uboot/): magic, header
// and data CRC, Linux/MIPS/kernel/uncompressed, inside RAM above the SPL
// (0x80008000 up to the 64 MiB the boot gives Linux), entry inside the image.
#define UIMAGE_HDR 64
#define SPL_RAM_START 0x80008000u
#define RAM_END (0x80000000u + (64u << 20))

typedef struct {
	uint8_t hdr[UIMAGE_HDR];
	size_t have;	   // header bytes seen
	long long data;	   // data bytes seen
	uint32_t dcrc;	   // CRC of the data so far
} uimage_check_t;

static void uimage_feed(uimage_check_t *u, const uint8_t *p, size_t n) {
	if (u->have < UIMAGE_HDR) {
		size_t k = UIMAGE_HDR - u->have < n ? UIMAGE_HDR - u->have : n;
		memcpy(u->hdr + u->have, p, k);
		u->have += k;
		p += k;
		n -= k;
	}
	if (u->have == UIMAGE_HDR && n > 0) {
		long long want = (long long)be32(u->hdr + 12) - u->data; // ih_size
		size_t k = want > 0 ? (want < (long long)n ? (size_t)want : n) : 0;
		u->dcrc = crc32_update(u->dcrc, p, k);
		u->data += (long long)k;
	}
}

static int uimage_ok(const uimage_check_t *u, char *why, size_t why_size) {
	const uint8_t *h = u->hdr;
	if (u->have < UIMAGE_HDR || be32(h) != 0x27051956u) {
		snprintf(why, why_size, "The kernel is not a uImage (bad magic).");
		return -1;
	}
	uint8_t copy[UIMAGE_HDR];
	memcpy(copy, h, sizeof(copy));
	memset(copy + 4, 0, 4); // ih_hcrc counts as 0
	if (crc32_update(0, copy, sizeof(copy)) != be32(h + 4)) {
		snprintf(why, why_size, "The kernel's uImage header is corrupt (checksum).");
		return -1;
	}
	uint32_t size = be32(h + 12), load = be32(h + 16), ep = be32(h + 20);
	if (u->data != (long long)size) {
		snprintf(why, why_size, "The kernel is truncated (%lld of %u data bytes).", u->data, size);
		return -1;
	}
	if (u->dcrc != be32(h + 24)) {
		snprintf(why, why_size, "The kernel's data is corrupt (uImage checksum).");
		return -1;
	}
	if (h[28] != 5 || h[29] != 5 || h[30] != 2 || h[31] != 0) { // os, arch, type, comp
		snprintf(why, why_size, "The kernel is not one the bootloader can start (os/arch/type/comp %d/%d/%d/%d, "
								"needs 5/5/2/0).",
				 h[28], h[29], h[30], h[31]);
		return -1;
	}
	uint64_t start = (uint64_t)load - UIMAGE_HDR, end = (uint64_t)load + size;
	if (load < UIMAGE_HDR || start < SPL_RAM_START || end > RAM_END) {
		snprintf(why, why_size, "The kernel loads at 0x%08x, outside the RAM the bootloader can use.", load);
		return -1;
	}
	if (ep < load || (uint64_t)ep >= end) {
		snprintf(why, why_size, "The kernel's entry point 0x%08x is outside it.", ep);
		return -1;
	}
	return 0;
}

// The chunk file names, in order: <name>.0000.<img_md5>, then each named
// after the md5 the list gives the chunk before it. `n` from read_md5_list().
static void chunk_path(const char *dir, const image_t *im, char (*list)[33], int i, char *out, size_t size) {
	snprintf(out, size, "%s/%s.%04d.%s", dir, im->name, i, i == 0 ? im->md5 : list[i - 1]);
}

// One pass over an image's chunks, in order: each chunk's md5 against the
// list, the size, and the md5 of them all against img_md5. With `w`, the data
// also goes to the writer as it is read, and progress is shown out of
// `total` bytes, `base` of them already done. 0 when all is right; otherwise
// the reason in `why`. A chunk that fails its md5 is never written: the
// writer only ever gets chunks that passed.
static int chunk_pass(const char *dir, const image_t *im, mtd_image_writer_t *w, long long base, long long total,
					  char *why, size_t why_size) {
	static char list[MAX_CHUNKS][33];
	int n = read_md5_list(dir, im, list, MAX_CHUNKS);
	if (n <= 0) {
		snprintf(why, why_size, "%s: no chunk md5 list (ota_md5_%s.%s).", im->name, im->name, im->md5);
		return -1;
	}
	uint8_t *buf = malloc(CHUNK_BUF);
	uint8_t *chunk = malloc(1 << 20); // one chunk, checked before any of it is written
	if (!buf || !chunk) {
		free(buf);
		free(chunk);
		snprintf(why, why_size, "Out of memory.");
		return -1;
	}
	md5_ctx all;
	md5_init(&all);
	bool is_kernel = strcmp(im->type, "kernel") == 0;
	uimage_check_t uck;
	memset(&uck, 0, sizeof(uck));
	char prev[33];
	snprintf(prev, sizeof(prev), "%s", im->md5);
	long long got = 0;
	int rc = 0;
	for (int i = 0; got < im->size; i++) {
		if (i >= n) {
			snprintf(why, why_size, "%s: chunk %d is not in the md5 list.", im->name, i);
			rc = -1;
			break;
		}
		char path[600];
		snprintf(path, sizeof(path), "%s/%s.%04d.%s", dir, im->name, i, prev);
		FILE *f = fopen(path, "rb");
		if (!f) {
			snprintf(why, why_size, "%s: chunk %d is missing.", im->name, i);
			rc = -1;
			break;
		}
		size_t len = fread(chunk, 1, 1 << 20, f);
		bool too_big = len == (1 << 20) && fgetc(f) != EOF;
		bool err = ferror(f);
		fclose(f);
		if (err || len == 0 || too_big) {
			snprintf(why, why_size, "%s: chunk %d cannot be read.", im->name, i);
			rc = -1;
			break;
		}
		md5_ctx c;
		md5_init(&c);
		md5_update(&c, chunk, len);
		md5_final_hex(&c, prev);
		if (strcmp(prev, list[i]) != 0) {
			snprintf(why, why_size, "%s is corrupt (chunk %d md5 mismatch). Copy the file to the card again.",
					 im->name, i);
			rc = -1;
			break;
		}
		if (got + (long long)len > im->size) {
			snprintf(why, why_size, "%s: the chunks are bigger than img_size (%lld bytes).", im->name, im->size);
			rc = -1;
			break;
		}
		md5_update(&all, chunk, len);
		if (is_kernel) {
			uimage_feed(&uck, chunk, len);
			if (w && got == 0 && (uck.have < UIMAGE_HDR || be32(uck.hdr) != 0x27051956u)) {
				// checked in full before anything was written; a card that now
				// reads otherwise stops here, before the first byte goes in
				snprintf(why, why_size, "The kernel is not a uImage (bad magic).");
				rc = -1;
				break;
			}
		}
		if (w) {
			for (size_t o = 0; o < len && rc == 0; o += CHUNK_BUF) {
				size_t k = len - o < CHUNK_BUF ? len - o : CHUNK_BUF;
				if (mtd_image_write(w, chunk + o, k) != 0) {
					snprintf(why, why_size, "%s: nandwrite stopped taking data.", im->name);
					rc = -1;
				}
			}
			if (rc) {
				break;
			}
		}
		got += (long long)len;
		set_percent((int)((base + got) * 100 / (total > 0 ? total : 1)));
	}
	if (rc == 0) {
		char md5[33];
		md5_final_hex(&all, md5);
		if (strcmp(md5, im->md5) != 0) {
			snprintf(why, why_size, "%s is corrupt (md5 mismatch). Copy the file to the card again.", im->name);
			rc = -1;
		} else if (is_kernel && uimage_ok(&uck, why, why_size) != 0) {
			rc = -1;
		}
	}
	free(buf);
	free(chunk);
	return rc;
}

// The system image as one stream over its chunk files, for squashfs.c: a
// read at an offset of the image goes to the chunk(s) holding it.
typedef struct {
	char (*paths)[600];
	long long *ends; // where each chunk ends in the image
	int n;
} chunk_image_t;

static int chunk_image_read(void *ctx, uint64_t off, void *buf, size_t len) {
	chunk_image_t *ci = ctx;
	uint8_t *d = buf;
	while (len > 0) {
		int i = 0;
		while (i < ci->n && (uint64_t)ci->ends[i] <= off) {
			i++;
		}
		if (i == ci->n) {
			return -1;
		}
		long long begin = i ? ci->ends[i - 1] : 0;
		size_t k = (size_t)((uint64_t)ci->ends[i] - off) < len ? (size_t)((uint64_t)ci->ends[i] - off) : len;
		FILE *f = fopen(ci->paths[i], "rb");
		bool ok = f && fseeko(f, (off_t)(off - (uint64_t)begin), SEEK_SET) == 0 && fread(d, 1, k, f) == k;
		if (f) {
			fclose(f);
		}
		if (!ok) {
			return -1;
		}
		d += k;
		off += k;
		len -= k;
	}
	return 0;
}

// Which player the system image is for: its /usr/resource/hostname, read
// straight from the chunks with squashfs.c (a few metadata blocks and one
// fragment; nothing is put together or copied anywhere). 0 when it names this
// player; otherwise the reason in `why`, and *unknown set when it could not be
// read at all.
static int check_rootfs_model(const char *dir, const image_t *im, bool *unknown, char *why, size_t why_size) {
	static char list[MAX_CHUNKS][33];
	static char paths[MAX_CHUNKS][600];
	static long long ends[MAX_CHUNKS];
	*unknown = true;
	int n = read_md5_list(dir, im, list, MAX_CHUNKS);
	chunk_image_t ci = {paths, ends, 0};
	long long at = 0;
	for (int i = 0; i < n && at < im->size; i++) {
		chunk_path(dir, im, list, i, paths[i], sizeof(paths[i]));
		struct stat st;
		if (stat(paths[i], &st) != 0) {
			break;
		}
		at += st.st_size;
		ends[i] = at;
		ci.n = i + 1;
	}
	char host[128];
	if (squashfs_read_file(chunk_image_read, &ci, "usr/resource/hostname", host, sizeof(host)) <= 0) {
		snprintf(why, why_size, "the system image has no /usr/resource/hostname that can be read");
		return -1;
	}
	*unknown = false;
	host[strcspn(host, "\r\n")] = '\0';
	if (strcmp(host, g_model.rootfs_hostname) != 0) {
		snprintf(why, why_size, "This package is for another player: its system is a '%s', this is a %s.", host,
				 g_model.name);
		return -1;
	}
	LOG("update: the system image is a %s", host);
	return 0;
}

// The card's <stem>.upt, in any case.
static int find_upt(const char *mnt, char *out, size_t size) {
	char want[64];
	snprintf(want, sizeof(want), "%s.upt", g_model.upt_stem);
	DIR *d = opendir(mnt);
	if (!d) {
		return -1;
	}
	struct dirent *e;
	int rc = -1;
	while ((e = readdir(d))) {
		if (strcasecmp(e->d_name, want) == 0) {
			snprintf(out, size, "%s/%s", mnt, e->d_name);
			rc = 0;
			break;
		}
	}
	closedir(d);
	return rc;
}

// ---------------------------------------------------------------------------
// The worker

static bool upt_mounted;
// What check_package() found, for verify_images() after the write.
static image_t checked_img[MAX_IMAGES];
static int n_checked;
static char checked_dir[300];
static void cleanup(void) {
	if (upt_mounted) {
#ifdef HOST_BUILD
		run("rm -rf " UPT_MOUNT);
#else
		if (umount(UPT_MOUNT) != 0) {
			LOG("update: umount %s: %s", UPT_MOUNT, strerror(errno));
		}
#endif
		upt_mounted = false;
	}
	storage_unmount();
}

// Checks the package. 0 when it can be written.
static int check_package(char *dir, size_t dir_size) {
	char v[128];
	if (file_key(UPT_MOUNT "/ota_config.in", "current_version", v, sizeof(v)) != 0 || !v[0]) {
		finish(UPDATE_FAILED, "Not a firmware package: no ota_config.in.");
		return -1;
	}
	int ver = atoi(v);
	snprintf(dir, dir_size, UPT_MOUNT "/ota_v%d", ver);

	char path[512];
	snprintf(path, sizeof(path), "%s/ota_v%d.ok", dir, ver);
	if (access(path, F_OK) != 0) {
		finish(UPDATE_FAILED, "The package is incomplete (no ota_v%d.ok).", ver);
		return -1;
	}

	// The compatibility record is the only thing that says which player the
	// package is for.
	snprintf(path, sizeof(path), "%s/ota_compatibility.in", dir);
	if (file_key(path, "compatibility", v, sizeof(v)) == 0) {
		if (strncmp(v, g_model.compat, strlen(g_model.compat)) != 0) {
			finish(UPDATE_FAILED, "This package is not for the %s: its compatibility record says '%s'.",
				   g_model.name, v);
			return -1;
		}
		LOG("update: compatibility %s", v);
	} else if (g_model.compat_needed) {
		finish(UPDATE_FAILED, "This package has no compatibility record, so nothing says it is for the %s.",
			   g_model.name);
		return -1;
	}

	image_t img[MAX_IMAGES];
	snprintf(path, sizeof(path), "%s/ota_update.in", dir);
	int n = parse_images(path, img, MAX_IMAGES);
	if (n <= 0) {
		finish(UPDATE_FAILED, "The package lists no images (ota_update.in).");
		return -1;
	}
	if (file_key(path, "ota_version", v, sizeof(v)) == 0 && atoi(v) != ver) {
		finish(UPDATE_FAILED, "The package is inconsistent: ota_update.in is for version %s, not %d.", v, ver);
		return -1;
	}
	int seen_kernel = 0, seen_rootfs = 0;

	for (int i = 0; i < n; i++) {
		const char *part;
		if (strcmp(img[i].type, "kernel") == 0) {
			part = "kernel";
			seen_kernel++;
		} else if (strcmp(img[i].type, "rootfs") == 0) {
			part = "rootfs"; // the first one: the normal system's
			seen_rootfs++;
		} else {
			finish(UPDATE_FAILED, "Unsupported image type '%s'.", img[i].type);
			return -1;
		}
		if (seen_kernel > 1 || seen_rootfs > 1) {
			finish(UPDATE_FAILED, "ota_update.in lists the %s twice.", part);
			return -1;
		}
		if (!img[i].name[0] || strchr(img[i].name, '/') || img[i].size <= 0 || strlen(img[i].md5) != 32) {
			finish(UPDATE_FAILED, "Image %d in ota_update.in is incomplete.", i);
			return -1;
		}
		char dev[32];
		int num = mtd_find(part, dev, sizeof(dev));
		long long psize = num >= 0 ? mtd_part_size(num) : -1;
		if (psize <= 0 || img[i].size > psize) {
			finish(UPDATE_FAILED, "%s (%lld bytes) does not fit %s (%lld bytes).", img[i].name, img[i].size,
				   part, psize);
			return -1;
		}

		char phase[64];
		snprintf(phase, sizeof(phase), "Checking %s", img[i].name);
		set_phase(phase, (i * 100) / n);
		char why[200];
		if (chunk_pass(dir, &img[i], NULL, 0, img[i].size, why, sizeof(why)) != 0) {
			finish(UPDATE_FAILED, "%s", why);
			return -1;
		}
		LOG("update: %s: %lld bytes, every chunk's md5 and the whole md5 ok, fits %s (%lld bytes)", img[i].name,
			img[i].size, dev, psize);
		if (strcmp(img[i].type, "rootfs") == 0) {
			// Which player it is for, from the system itself. Another player's
			// system is always refused. When it cannot be read, the R1's
			// compatibility record (checked above, required) still decides;
			// the R3 Pro II's packages have none, so there nothing would.
			set_phase("Checking which player it is for", -1);
			bool unknown;
			if (check_rootfs_model(dir, &img[i], &unknown, why, sizeof(why)) != 0) {
				if (!unknown || !g_model.compat_needed) {
					finish(UPDATE_FAILED, unknown ? "Cannot tell which player this package is for: %s." : "%s", why);
					return -1;
				}
				LOG("update: %s; the compatibility record decides", why);
			}
		}
	}
	memcpy(checked_img, img, sizeof(img[0]) * (size_t)n);
	n_checked = n;
	snprintf(checked_dir, sizeof(checked_dir), "%s", dir);
	set_percent(100);
	return 0;
}

// The partition an image goes to: "kernel", or the first "rootfs".
static const char *image_part(const image_t *im) { return strcmp(im->type, "kernel") == 0 ? "kernel" : "rootfs"; }

static unsigned part_erasesize(int num) {
	mtd_part_t parts[16];
	int n = mtd_list(parts, 16);
	for (int i = 0; i < n; i++) {
		if (parts[i].num == num && parts[i].erasesize) {
			return parts[i].erasesize;
		}
	}
	return 0x20000;
}

// After the writer: each image read back from its partition and its md5
// compared with img_md5. Read as the boot reads it -- the SPL and
// mtdblock_bbt skip bad blocks -- which is also how nandwrite laid it out:
// the first img_size bytes of the good blocks, in order. 0 when all match.
static int verify_images(char *bad, size_t bad_size) {
	int rc = 0;
	bad[0] = '\0';
	for (int i = 0; i < n_checked; i++) {
		const image_t *im = &checked_img[i];
		char dev[32], phase[64];
		int num = mtd_find(image_part(im), dev, sizeof(dev));
		snprintf(phase, sizeof(phase), "Checking the written %s", strcmp(im->type, "kernel") == 0 ? "kernel" : "system");
		set_phase(phase, 0);
		unsigned es = part_erasesize(num);
		long long psize = mtd_part_size(num);
		// The partition through nanddump --bb=padbad (bad blocks given as 0xff
		// in their place); only the good blocks count, in order.
		int fd = num >= 0 ? mtd_open_ro(num) : -1; // which blocks are bad
		mtd_dump_t *d = fd >= 0 ? mtd_dump_begin(num, psize) : NULL;
		uint8_t *buf = malloc(es);
		md5_ctx c;
		md5_init(&c);
		long long got = 0;
		bool read_ok = d && buf;
		for (long long off = 0; read_ok && got < im->size && off < psize; off += es) {
			if (mtd_dump_read(d, buf, es) != 0) {
				read_ok = false;
				break;
			}
			if (mtd_block_bad(fd, off) == 1) {
				continue;
			}
			size_t want = (size_t)(im->size - got < es ? im->size - got : es);
			md5_update(&c, buf, want);
			got += (long long)want;
			set_percent((int)(got * 100 / im->size));
		}
		if (d) {
			mtd_dump_end(d, NULL); // stopped early on purpose: its status does not count
		}
		if (fd >= 0) {
			close(fd);
		}
		free(buf);
		char md5[33] = "";
		if (read_ok && got == im->size) {
			md5_final_hex(&c, md5);
		}
		if (strcmp(md5, im->md5) != 0) {
			LOG("update: %s on %s reads back as %s, the package says %s", im->name, dev, md5[0] ? md5 : "(unreadable)",
				im->md5);
			size_t l = strlen(bad);
			snprintf(bad + l, bad_size - l, "%s%s", l ? " and " : "",
					 strcmp(im->type, "kernel") == 0 ? "the kernel" : "the system");
			rc = -1;
		} else {
			LOG("update: %s on %s reads back with the package's md5", im->name, dev);
		}
	}
	return rc;
}

// Writes the checked images, the kernel first, each into its partition:
// "kernel" and the first "rootfs", the normal system's, whatever the boot
// record says (the worker has set it to the recovery already). Every chunk
// is read and checked again as it goes in (chunk_pass), so a card that reads
// differently now than during the check stops the write at that chunk.
// 0 when every image went in whole; nothing is read back here.
static int write_package(char *why, size_t why_size) {
	long long total = 0, base = 0;
	for (int i = 0; i < n_checked; i++) {
		total += checked_img[i].size;
	}
	for (int pass = 0; pass < 2; pass++) { // the kernel, then the system
		for (int i = 0; i < n_checked; i++) {
			const image_t *im = &checked_img[i];
			bool is_kernel = strcmp(im->type, "kernel") == 0;
			if (is_kernel != (pass == 0)) {
				continue;
			}
			char dev[32];
			int num = mtd_find(image_part(im), dev, sizeof(dev));
			if (num < 0) {
				snprintf(why, why_size, "There is no %s partition.", image_part(im));
				return -1;
			}
			pthread_mutex_lock(&lock);
			status.system_touched = 1; // from the erase on
			pthread_mutex_unlock(&lock);
			set_phase(is_kernel ? "Writing kernel" : "Writing system", (int)(base * 100 / total));
			LOG("update: writing %s (%lld bytes) to %s", im->name, im->size, dev);
			mtd_image_writer_t *w = mtd_image_begin(num, im->size, MTD_IMAGE_MARKBAD);
			if (!w) {
				snprintf(why, why_size, "%s cannot be erased or opened for writing.", dev);
				return -1;
			}
			int rc = chunk_pass(checked_dir, im, w, base, total, why, why_size);
			if (mtd_image_end(w) != 0 && rc == 0) {
				snprintf(why, why_size, "nandwrite could not write %s to %s.", im->name, dev);
				rc = -1;
			}
			if (rc != 0) {
				return -1;
			}
			base += im->size;
			LOG("update: %s written to %s", im->name, dev);
		}
	}
	return 0;
}

// The file picked on the card, empty for <stem>.upt.
static char requested[512];

static void *worker(void *arg) {
	(void)arg;
	char card[256] = "";
	char upt[512];
	char dir[300];
	char q[700];

	set_phase("Looking for the card", -1);
	if (storage_mount(10, card, sizeof(card)) != 0) {
		finish(UPDATE_FAILED, "No microSD card could be read (FAT32 or exFAT).");
		card[0] = '\0';
		goto out;
	}
	if (requested[0]) {
		snprintf(upt, sizeof(upt), "%s", requested);
		if (access(upt, R_OK) != 0) {
			finish(UPDATE_FAILED, "%s is no longer on the card.", upt);
			goto out;
		}
	} else if (find_upt(card, upt, sizeof(upt)) != 0) {
		finish(UPDATE_FAILED, "There is no %s.upt in the top folder of the card.", g_model.upt_stem);
		goto out;
	}
	LOG("update: package %s", upt);

	set_phase("Opening the package", -1);
#ifdef HOST_BUILD
	if (run("rm -rf " UPT_MOUNT " && mkdir -p " UPT_MOUNT " && 7z x -y -o" UPT_MOUNT " %s >/dev/null",
			shq(upt, q, sizeof(q))) != 0) {
#else
	mkdir(UPT_MOUNT, 0755);
	if (run("mount -o loop,ro -t iso9660 %s " UPT_MOUNT, shq(upt, q, sizeof(q))) != 0) {
#endif
		finish(UPDATE_FAILED, "%s is not a firmware package (cannot open it).", strrchr(upt, '/') + 1);
		goto out;
	}
	upt_mounted = true;

	if (check_package(dir, sizeof(dir)) != 0) {
		goto out;
	}

	// The record on the recovery from before the first erase: if the write is
	// cut short, the player starts here again (update.h).
	if (bootflag_read() != BOOT_RECOVERY) {
		set_phase("Setting the boot record", -1);
		if (bootflag_write(BOOT_RECOVERY) != 0) {
			finish(UPDATE_FAILED, "Cannot write the boot record; nothing was changed.");
			goto out;
		}
	}

	set_phase("Writing", -1);
	char why[200] = "";
	if (write_package(why, sizeof(why)) != 0) {
		update_status_t s;
		update_get(&s);
		LOG("update: writing failed: %s", why);
		finish(UPDATE_FAILED, "Writing failed: %s%s", why,
			   s.system_touched ? " The system is incomplete: the player will start in recovery until an "
								  "update succeeds."
								: " Nothing was written.");
		goto out;
	}

	// The writer says it is done; the flash has to say so too. A mismatch puts
	// the record back on the recovery, so the player comes back here instead
	// of starting a broken system.
	char bad[64];
	if (verify_images(bad, sizeof(bad)) != 0) {
		bootflag_write(BOOT_RECOVERY);
		finish(UPDATE_FAILED,
			   "The update was written, but %s does not read back as in the package. The player will start in "
			   "recovery; install it again.",
			   bad);
		goto out;
	}

	if (bootflag_read() != BOOT_NORMAL && bootflag_write(BOOT_NORMAL) != 0) {
		finish(UPDATE_FAILED, "The update was written, but the boot record could not be set back to the system.");
		goto out;
	}
	finish(UPDATE_DONE, "The update is installed and verified.");

out:
	cleanup();
	return NULL;
}

int update_start(const char *path) {
	pthread_mutex_lock(&lock);
	if (status.state == UPDATE_RUNNING) {
		pthread_mutex_unlock(&lock);
		return -1;
	}
	snprintf(requested, sizeof(requested), "%s", path ? path : "");
	memset(&status, 0, sizeof(status));
	status.state = UPDATE_RUNNING;
	status.percent = -1;
	pthread_mutex_unlock(&lock);

	pthread_t t;
	if (pthread_create(&t, NULL, worker, NULL) != 0) {
		finish(UPDATE_FAILED, "Cannot start the update thread.");
		return -1;
	}
	pthread_detach(t);
	return 0;
}

// ---------------------------------------------------------------------------
// The card, one folder at a time, for the file browser

static int entry_cmp(const void *a, const void *b) {
	const upt_entry_t *x = a, *y = b;
	if (x->is_dir != y->is_dir) {
		return x->is_dir ? -1 : 1; // folders first
	}
	return strcasecmp(x->name, y->name);
}

int update_list_dir(const char *rel, upt_entry_t *out, int max) {
	char card[256];
	if (storage_mount(3, card, sizeof(card)) != 0) {
		return -1;
	}
	if (strstr(rel, "..")) {
		return -1;
	}
	char dir[512];
	snprintf(dir, sizeof(dir), "%s%s%s", card, rel[0] ? "/" : "", rel);
	DIR *d = opendir(dir);
	if (!d) {
		return -1;
	}
	int n = 0;
	struct dirent *e;
	while (n < max && (e = readdir(d))) {
		if (e->d_name[0] == '.' || strcasecmp(e->d_name, "System Volume Information") == 0 ||
			strcasecmp(e->d_name, "LOST.DIR") == 0) {
			continue;
		}
		char full[800];
		snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
		struct stat st;
		if (stat(full, &st) != 0) {
			continue;
		}
		size_t l = strlen(e->d_name);
		bool is_dir = S_ISDIR(st.st_mode);
		if (!is_dir && !(S_ISREG(st.st_mode) && l > 4 && strcasecmp(e->d_name + l - 4, ".upt") == 0)) {
			continue; // only folders and .upt files
		}
		snprintf(out[n].path, sizeof(out[n].path), "%s", full);
		snprintf(out[n].name, sizeof(out[n].name), "%s", e->d_name);
		out[n].is_dir = is_dir;
		out[n].size = (long long)st.st_size;
		n++;
	}
	closedir(d);
	qsort(out, (size_t)n, sizeof(out[0]), entry_cmp);
	return n;
}
