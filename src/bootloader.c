#include "bootloader.h"

#include "device.h"
#include "log.h"
#include "model.h"
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
#define PART_TABLE_OFF 0x5800
#define PART_TABLE_LEN 0x400
#define TMP_IMAGE "/tmp/sonix-uboot.bin"
#define TMP_ORIG "/tmp/sonix-uboot-orig.bin"

// The boot ROM's header at the start of every SPL, stock and Sonix alike.
static const uint8_t SPL_MAGIC[8] = {0x06, 0x05, 0x04, 0x03, 0x02, 0x55, 0xaa, 0x55};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bl_status_t status;
static int sonix_cached = -1;

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

bool bootloader_is_sonix(void) {
	if (sonix_cached < 0) {
		long long size;
		uint8_t *buf = read_part(&size);
		sonix_cached = buf && contains(buf, (size_t)size, SIGNATURE);
		LOG("bootloader: %s", !buf ? "cannot read the uboot partition" : sonix_cached ? "Sonix (fastboot)" : "stock");
		free(buf);
	}
	return sonix_cached == 1;
}

static void image_path(char *out, size_t size, const char *suffix) {
	snprintf(out, size, "%s/%s%s", bootloader_dir(), g_model.uboot_image, suffix);
}

bool bootloader_can_install(void) {
	if (!g_model.uboot_installable) {
		return false;
	}
	char p[512];
	image_path(p, sizeof(p), "");
	return access(p, R_OK) == 0;
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
	storage_unmount();
}

static void *worker(void *arg) {
	(void)arg;
	uint8_t *img = NULL, *orig = NULL;
	size_t img_len = 0;
	long long psize = 0;
	char path[512], md5_want[40] = "", md5_have[33];

	phase("Checking the bundled image");
	image_path(path, sizeof(path), "");
	img = load_file(path, &img_len);
	char md5_path[520];
	image_path(md5_path, sizeof(md5_path), ".md5");
	FILE *mf = fopen(md5_path, "r");
	if (mf) {
		if (fscanf(mf, "%32s", md5_want) != 1) {
			md5_want[0] = '\0';
		}
		fclose(mf);
	}
	if (!img || md5_of(path, md5_have) != 0 || strlen(md5_want) != 32 || strcmp(md5_have, md5_want) != 0) {
		finish(BL_FAILED, "The bundled bootloader image is missing or corrupt. Nothing was changed.");
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

	// The player's own partition table into the new image.
	memcpy(img + PART_TABLE_OFF, orig + PART_TABLE_OFF, PART_TABLE_LEN);
	if (save_file(TMP_IMAGE, img, img_len) != 0 || save_file(TMP_ORIG, orig, (size_t)psize) != 0) {
		finish(BL_FAILED, "Cannot stage the image in /tmp. Nothing was changed.");
		goto out;
	}
	backup_to_card(orig, (size_t)psize);

	phase("Writing the bootloader");
	for (int attempt = 1; attempt <= 2; attempt++) {
		if (write_and_verify(TMP_IMAGE, img, img_len, psize) == 0) {
			sonix_cached = -1;
			free(first_orig);
			first_orig = NULL;
			finish(BL_DONE, "The Sonix bootloader is installed. Fastboot: hold Play/Pause at power-on, or use "
							"\"Reboot to fastboot\".");
			goto out;
		}
		LOG("bootloader: read back differs (attempt %d)", attempt);
		phase("Read back differs, writing again");
	}

	phase("Putting the old bootloader back");
	for (int attempt = 1; attempt <= 2; attempt++) {
		if (write_and_verify(TMP_ORIG, orig, (size_t)psize, psize) == 0) {
			sonix_cached = -1;
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
