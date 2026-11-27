#include "bootloader.h"

#include "device.h"
#include "log.h"
#include "model.h"
#include "parttable.h"
#include "storage.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SIGNATURE "ota:fastboot"
#define PART_TABLE_OFF PT_OFF
#define PART_TABLE_LEN PT_LEN
// The expected partition table, from partitions/<model>.txt (pack_recovery.sh)
#define TABLE_FILE "partition-table.bin"
#define TMP_IMAGE "/tmp/zees-uboot.bin"
#define TMP_ORIG "/tmp/zees-uboot-orig.bin"

// The boot ROM's header at the start of every SPL, stock and Zee's alike.
static const uint8_t SPL_MAGIC[8] = {0x06, 0x05, 0x04, 0x03, 0x02, 0x55, 0xaa, 0x55};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bl_status_t status;
static int zees_cached = -1;

// The partition as it was before this process first wrote to it. A retry
// after a write that went wrong must not take the broken flash for the
// original.
static uint8_t *first_orig;
static long long first_size;

void bootloader_get(bl_status_t *out) {
	pthread_mutex_lock(&lock);
	*out = status;
	pthread_mutex_unlock(&lock);
}

static void phase(const char *p) {
	pthread_mutex_lock(&lock);
	snprintf(status.phase, sizeof(status.phase), "%s", p);
	pthread_mutex_unlock(&lock);
	LOG("bootloader: %s", p);
}

static void finish(bl_state_t st, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void finish(bl_state_t st, const char *fmt, ...) {
	va_list ap;
	pthread_mutex_lock(&lock);
	va_start(ap, fmt);
	vsnprintf(status.message, sizeof(status.message), fmt, ap);
	va_end(ap);
	status.state = st;
	pthread_mutex_unlock(&lock);
	LOG("bootloader: %s: %s", st == BL_DONE ? "done" : "FAILED", status.message);
}

static long long part_size(void) {
	char dev[32];
	int n = mtd_find("uboot", dev, sizeof(dev));
	return n >= 0 ? mtd_part_size(n) : -1;
}

static uint8_t *read_part(long long *size_out) {
	long long size = part_size();
	if (size <= 0 || size > 4 * 1024 * 1024) {
		return NULL;
	}
	uint8_t *buf = malloc((size_t)size);
	if (buf && uboot_part_read(buf, (size_t)size) != 0) {
		free(buf);
		buf = NULL;
	}
	*size_out = size;
	return buf;
}

static bool contains(const uint8_t *buf, size_t len, const char *s) {
	size_t sl = strlen(s);
	for (size_t i = 0; i + sl <= len; i++) {
		if (buf[i] == (uint8_t)s[0] && memcmp(buf + i, s, sl) == 0) {
			return true;
		}
	}
	return false;
}

bool bootloader_is_zees(void) {
	if (zees_cached < 0) {
		long long size;
		uint8_t *buf = read_part(&size);
		zees_cached = buf && contains(buf, (size_t)size, SIGNATURE);
		LOG("bootloader: %s", !buf ? "cannot read the uboot partition" : zees_cached ? "Zee's (fastboot)" : "stock");
		free(buf);
	}
	return zees_cached == 1;
}

static void image_path(char *out, size_t size, const char *suffix) {
	snprintf(out, size, "%s/%s%s", bootloader_dir(), g_model.uboot_image, suffix);
}

static void table_path(char *out, size_t size, const char *suffix) {
	snprintf(out, size, "%s/" TABLE_FILE "%s", bootloader_dir(), suffix);
}

bool bootloader_can_install(void) {
	if (!g_model.uboot_installable) {
		return false;
	}
	char p[512], t[512];
	image_path(p, sizeof(p), "");
	table_path(t, sizeof(t), "");
	return access(p, R_OK) == 0 && access(t, R_OK) == 0;
}

static uint8_t *load_file(const char *path, size_t *len) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = n > 0 ? malloc((size_t)n) : NULL;
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	*len = (size_t)n;
	return buf;
}

static int save_file(const char *path, const uint8_t *buf, size_t len) {
	FILE *f = fopen(path, "wb");
	if (!f) {
		return -1;
	}
	int rc = fwrite(buf, 1, len, f) == len ? 0 : -1;
	if (fclose(f) != 0) {
		rc = -1;
	}
	return rc;
}

static int md5_of(const char *path, char out[33]) {
	char q[600], cmd[700];
	snprintf(cmd, sizeof(cmd), "md5sum %s", shq(path, q, sizeof(q)));
	FILE *p = popen(cmd, "r");
	if (!p) {
		return -1;
	}
	char line[128] = "";
	char *r = fgets(line, sizeof(line), p);
	int rc = pclose(p);
	return (r && rc == 0 && sscanf(line, "%32s", out) == 1) ? 0 : -1;
}

// The file at path, if its md5 is the one recorded in md5_path.
static uint8_t *load_checked(const char *path, const char *md5_path, size_t *len) {
	char want[40] = "", have[33];
	FILE *mf = fopen(md5_path, "r");
	if (mf) {
		if (fscanf(mf, "%32s", want) != 1) {
			want[0] = '\0';
		}
		fclose(mf);
	}
	uint8_t *buf = load_file(path, len);
	if (!buf || md5_of(path, have) != 0 || strlen(want) != 32 || strcmp(have, want) != 0) {
		free(buf);
		return NULL;
	}
	return buf;
}

// Erase, write `file` (the whole partition is erased; nandwrite pads the
// last page) and compare the first len bytes read back with `want`.
static int write_and_verify(const char *file, const uint8_t *want, size_t len, long long psize) {
	if (uboot_part_write(file) != 0) {
		LOG("bootloader: the write itself failed");
	}
	uint8_t *back = malloc((size_t)psize);
	if (!back) {
		return -1;
	}
	int rc = (uboot_part_read(back, (size_t)psize) == 0 && memcmp(back, want, len) == 0) ? 0 : -1;
	free(back);
	return rc;
}

// A copy of the partition as it was, on the card when there is one.
static void backup_to_card(const uint8_t *orig, size_t len) {
	char card[256];
	if (storage_mount(2, card, sizeof(card)) != 0) {
		LOG("bootloader: no card, no backup copy");
		return;
	}
	// A backup, so the card is writable for it (storage.h), and read-only again
	// right after.
	if (storage_set_writable(true) != 0) {
		LOG("bootloader: the card cannot be made writable, no backup copy");
		storage_unmount();
		return;
	}
	char path[512];
	for (int i = 1; i < 100; i++) {
		snprintf(path, sizeof(path), "%s/uboot-backup-%d.bin", card, i);
		if (access(path, F_OK) != 0) {
			if (save_file(path, orig, len) == 0) {
				LOG("bootloader: old partition saved as %s", path);
				phase("Old bootloader saved on the card");
			}
			break;
		}
	}
	storage_set_writable(false);
	storage_unmount();
}

static void *worker(void *arg) {
	(void)arg;
	uint8_t *img = NULL, *orig = NULL, *want_table = NULL;
	size_t img_len = 0, table_len = 0;
	long long psize = 0;
	char path[512], md5_path[520];
	uint8_t table[PART_TABLE_LEN];
	char why[160];
	bool table_fixed = false;

	phase("Checking the bundled image");
	image_path(path, sizeof(path), "");
	image_path(md5_path, sizeof(md5_path), ".md5");
	img = load_checked(path, md5_path, &img_len);
	if (!img) {
		finish(BL_FAILED, "The bundled bootloader image is missing or corrupt. Nothing was changed.");
		goto out;
	}
	table_path(path, sizeof(path), "");
	table_path(md5_path, sizeof(md5_path), ".md5");
	want_table = load_checked(path, md5_path, &table_len);
	if (!want_table || table_len != PART_TABLE_LEN) {
		finish(BL_FAILED, "The bundled partition table is missing or corrupt. Nothing was changed.");
		goto out;
	}
	if (img_len < PART_TABLE_OFF + PART_TABLE_LEN || memcmp(img, SPL_MAGIC, sizeof(SPL_MAGIC)) != 0) {
		finish(BL_FAILED, "The bundled image does not start with an SPL. Nothing was changed.");
		goto out;
	}
	for (int i = 0; i < PART_TABLE_LEN; i++) {
		if (img[PART_TABLE_OFF + i] != 0xff) {
			finish(BL_FAILED, "The bundled image carries a partition table of its own. Nothing was changed.");
			goto out;
		}
	}

	phase("Reading the current bootloader");
	if (first_orig) {
		psize = first_size;
		orig = malloc((size_t)psize);
		if (orig) {
			memcpy(orig, first_orig, (size_t)psize);
		}
		LOG("bootloader: using the partition as read before the first write");
	} else {
		orig = read_part(&psize);
	}
	if (!orig) {
		finish(BL_FAILED, "The bootloader partition cannot be read. Nothing was changed.");
		goto out;
	}
	if ((long long)img_len > psize) {
		finish(BL_FAILED, "The image (%zu bytes) does not fit the partition (%lld). Nothing was changed.", img_len,
			   psize);
		goto out;
	}
	if (memcmp(orig + PART_TABLE_OFF, "nand", 4) != 0) {
		finish(BL_FAILED, "The bootloader partition has no partition table at 0x5800. Nothing was changed.");
		goto out;
	}

	// The player's table, as partitions/<model>.txt has it (parttable.h); the
	// duplicated "rootfs" of the R1's firmware 2.0 is renamed "rootfs2".
	phase("Checking the partition table");
	switch (pt_check(orig + PART_TABLE_OFF, want_table, table, why, sizeof(why))) {
	case PT_SAME:
		LOG("bootloader: partition table %s", why);
		break;
	case PT_FIXED:
		LOG("bootloader: partition table: %s", why);
		table_fixed = true;
		break;
	case PT_MISMATCH:
		finish(BL_FAILED, "The player's partition table is not the expected one: %s. Nothing was changed.", why);
		goto out;
	}
	int bad = uboot_part_bad_blocks();
	if (bad != 0) {
		finish(BL_FAILED, bad > 0 ? "The bootloader partition has a bad block. Nothing was changed."
								  : "The bootloader partition's blocks cannot be checked. Nothing was changed.");
		goto out;
	}

	if (!first_orig && (first_orig = malloc((size_t)psize))) {
		memcpy(first_orig, orig, (size_t)psize);
		first_size = psize;
	}

	// The player's own partition table into the new image (fixed, if it was).
	memcpy(img + PART_TABLE_OFF, table, PART_TABLE_LEN);
	if (save_file(TMP_IMAGE, img, img_len) != 0 || save_file(TMP_ORIG, orig, (size_t)psize) != 0) {
		finish(BL_FAILED, "Cannot stage the image in /tmp. Nothing was changed.");
		goto out;
	}
	backup_to_card(orig, (size_t)psize);

	phase("Writing the bootloader");
	for (int attempt = 1; attempt <= 2; attempt++) {
		if (write_and_verify(TMP_IMAGE, img, img_len, psize) == 0) {
			zees_cached = -1;
			free(first_orig);
			first_orig = NULL;
			finish(BL_DONE, "Zee's bootloader is installed%s. Fastboot: hold Play/Pause at power-on, or use "
							"\"Reboot to fastboot\".",
				   table_fixed ? ", and the partition table's second \"rootfs\" is now \"rootfs2\"" : "");
			goto out;
		}
		LOG("bootloader: read back differs (attempt %d)", attempt);
		phase("Read back differs, writing again");
	}

	phase("Putting the old bootloader back");
	for (int attempt = 1; attempt <= 2; attempt++) {
		if (write_and_verify(TMP_ORIG, orig, (size_t)psize, psize) == 0) {
			zees_cached = -1;
			free(first_orig);
			first_orig = NULL;
			finish(BL_FAILED, "The new bootloader could not be written correctly; the old one was put back and "
							  "checked.");
			goto out;
		}
	}
	finish(BL_FAILED_BAD, "The bootloader partition does not read back correctly. Do not turn the player off; "
						  "try again, and keep it on the charger.");

out:
	unlink(TMP_IMAGE);
	unlink(TMP_ORIG);
	free(img);
	free(orig);
	free(want_table);
	return NULL;
}

int bootloader_install_start(void) {
	pthread_mutex_lock(&lock);
	if (status.state == BL_RUNNING) {
		pthread_mutex_unlock(&lock);
		return -1;
	}
	memset(&status, 0, sizeof(status));
	status.state = BL_RUNNING;
	pthread_mutex_unlock(&lock);
	pthread_t t;
	if (pthread_create(&t, NULL, worker, NULL) != 0) {
		finish(BL_FAILED, "Cannot start the install thread.");
		return -1;
	}
	pthread_detach(t);
	return 0;
}
