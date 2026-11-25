#include "update.h"

#include "device.h"
#include "log.h"
#include "md5.h"
#include "model.h"
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
// instead of loop-mounted, and the writing is simulated; the checks are the
// real ones.
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


// The chunks <name>.NNNN.<md5> in `dir`: their total size, and whether they
// are numbered 0..count-1 with none missing.
static long long chunks_size(const char *dir, const char *name, int *count) {
	DIR *d = opendir(dir);
	if (!d) {
		return -1;
	}
	size_t nl = strlen(name);
	long long total = 0;
	int n = 0, max_idx = -1;
	struct dirent *e;
	char path[512];
	while ((e = readdir(d))) {
		int idx;
		char rest[64];
		if (strncmp(e->d_name, name, nl) != 0 || e->d_name[nl] != '.' ||
			sscanf(e->d_name + nl + 1, "%4d.%63s", &idx, rest) != 2 || strlen(rest) != 32) {
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		struct stat st;
		if (stat(path, &st) != 0) {
			closedir(d);
			return -1;
		}
		total += st.st_size;
		n++;
		if (idx > max_idx) {
			max_idx = idx;
		}
	}
	closedir(d);
	*count = (max_idx + 1 == n) ? n : -1;
	return total;
}

// md5 of the chunks joined in order. The names have a fixed-width index right
// after the image name, so the shell's sorted glob is the right order.
// With tee_to, the joined image is also written there, in the same pass.
static int chunks_md5(const char *dir, const char *name, const char *tee_to, char out[33]) {
	char cmd[1024], q[600];
	if (tee_to) {
		snprintf(cmd, sizeof(cmd), "cat %s/%s.[0-9][0-9][0-9][0-9].* | tee %s | md5sum", dir, name,
				 shq(tee_to, q, sizeof(q)));
	} else {
		snprintf(cmd, sizeof(cmd), "cat %s/%s.[0-9][0-9][0-9][0-9].* | md5sum", dir, name);
	}
	FILE *p = popen(cmd, "r");
	if (!p) {
		return -1;
	}
	char line[128] = {0};
	char *r = fgets(line, sizeof(line), p);
	int rc = pclose(p);
	if (!r || rc != 0 || sscanf(line, "%32s", out) != 1) {
		return -1;
	}
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
static bool ignore_compat; // update_start(): the user overrode the compatibility record

// The rootfs image joined on the card, only for as long as the model check
// takes: ~40 MB, more than the RAM can spare.
#define ROOTFS_CHECK_NAME ".sonix_rootfs_check.squashfs"
static char rootfs_check[512];

static void remove_rootfs_check(void) {
	if (rootfs_check[0]) {
		unlink(rootfs_check);
		rootfs_check[0] = '\0';
	}
}

static void cleanup(const char *card) {
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
	remove_rootfs_check();
	if (card && card[0]) {
		char dst[600], q[700];
		snprintf(dst, sizeof(dst), "%s/sonix_recovery.log", card);
		run("cp %s %s", LOG_FILE, shq(dst, q, sizeof(q)));
	}
	storage_unmount();
}

// The model check: the system image's /usr/resource/hostname must name this
// player. Every package seen so far carries it, stock and Sonix ("HiBy R1",
// "HiBy R3PROII"); one that does not is refused, as there is then nothing
// that says which player it is for.
static int check_rootfs_model(const char *image) {
	char buf[128];
	int n = squashfs_read_file(image, "usr/resource/hostname", buf, sizeof(buf));
	if (n < 0) {
		finish(UPDATE_FAILED, "The system image cannot be read to tell which player it is for.");
		return -1;
	}
	buf[strcspn(buf, "\r\n")] = '\0';
	if (strcmp(buf, g_model.rootfs_hostname) != 0) {
		finish(UPDATE_FAILED, "This package is for another player: its system is a '%s', this is a %s.", buf,
			   g_model.name);
		return -1;
	}
	LOG("update: the system image is a %s", buf);
	return 0;
}

// Checks the package. 0 when it can be written.
static int check_package(char *dir, size_t dir_size, const char *card) {
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

	snprintf(path, sizeof(path), "%s/ota_compatibility.in", dir);
	// The record is judged only after the system image has been looked at:
	// another player's package is then refused naming that player, and the
	// override (ignore_compat) is offered only for a package whose system is
	// this player's and whose record alone disagrees.
	char compat_problem[160] = "";
	if (file_key(path, "compatibility", v, sizeof(v)) == 0) {
		if (strncmp(v, g_model.compat, strlen(g_model.compat)) != 0) {
			snprintf(compat_problem, sizeof(compat_problem), "Its compatibility record says '%s'.", v);
		} else {
			LOG("update: compatibility %s", v);
		}
	} else if (g_model.compat_needed) {
		snprintf(compat_problem, sizeof(compat_problem), "It has no compatibility record.");
	}

	image_t img[MAX_IMAGES];
	snprintf(path, sizeof(path), "%s/ota_update.in", dir);
	int n = parse_images(path, img, MAX_IMAGES);
	if (n <= 0) {
		finish(UPDATE_FAILED, "The package lists no images (ota_update.in).");
		return -1;
	}

	int rootfs_seen = 0;
	for (int i = 0; i < n; i++) {
		const char *part;
		if (strcmp(img[i].type, "kernel") == 0) {
			part = "kernel";
		} else if (strcmp(img[i].type, "rootfs") == 0) {
			part = "rootfs"; // the first one: the normal system's
		} else {
			finish(UPDATE_FAILED, "Unsupported image type '%s'.", img[i].type);
			return -1;
		}
		if (!img[i].name[0] || img[i].size <= 0 || strlen(img[i].md5) != 32) {
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
		int count = 0;
		long long csize = chunks_size(dir, img[i].name, &count);
		if (csize != img[i].size || count <= 0) {
			finish(UPDATE_FAILED, "%s: chunks missing or wrong size (%lld of %lld bytes).", img[i].name, csize,
				   img[i].size);
			return -1;
		}
		bool is_rootfs = strcmp(img[i].type, "rootfs") == 0;
		if (is_rootfs) {
			snprintf(rootfs_check, sizeof(rootfs_check), "%s/" ROOTFS_CHECK_NAME, card);
		}
		char md5[33];
		if (chunks_md5(dir, img[i].name, is_rootfs ? rootfs_check : NULL, md5) != 0) {
			finish(UPDATE_FAILED, is_rootfs ? "%s cannot be read, or the card has no room for the check "
											  "(it needs about 50 MB free)."
											: "%s cannot be read.",
				   img[i].name);
			return -1;
		}
		if (strcmp(md5, img[i].md5) != 0) {
			finish(UPDATE_FAILED, "%s is corrupt (md5 mismatch). Copy the file to the card again.", img[i].name);
			return -1;
		}
		if (is_rootfs) {
			set_phase("Checking which player it is for", -1);
			int rc = check_rootfs_model(rootfs_check);
			remove_rootfs_check();
			if (rc != 0) {
				return -1;
			}
			rootfs_seen = 1;
		}
		LOG("update: %s: %d chunks, %lld bytes, md5 ok, fits %s (%lld bytes)", img[i].name, count, csize, dev,
			psize);
	}
	if (!rootfs_seen) {
		finish(UPDATE_FAILED, "The package has no system image, so nothing says which player it is for.");
		return -1;
	}
	if (compat_problem[0]) {
		if (ignore_compat) {
			LOG("update: compatibility check overridden by the user: %s", compat_problem);
		} else {
			pthread_mutex_lock(&lock);
			status.compat_overridable = 1;
			pthread_mutex_unlock(&lock);
			finish(UPDATE_FAILED, "%s Its system image is a %s, so it can be installed anyway.", compat_problem,
				   g_model.name);
			return -1;
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
		int fd = num >= 0 ? mtd_open_ro(num) : -1;
		uint8_t *buf = malloc(es);
		md5_ctx c;
		md5_init(&c);
		long long got = 0;
		bool read_ok = fd >= 0 && buf;
		for (long long off = 0; read_ok && got < im->size && off < psize; off += es) {
			if (mtd_block_bad(fd, off) == 1) {
				continue;
			}
			size_t want = (size_t)(im->size - got < es ? im->size - got : es);
			if (pread(fd, buf, want, (off_t)off) != (ssize_t)want) {
				read_ok = false;
				break;
			}
			md5_update(&c, buf, want);
			got += (long long)want;
			set_percent((int)(got * 100 / im->size));
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

#ifdef HOST_BUILD
// Simulated: the phases and progress the stock writer reports, and the
// images written into $SIM_DIR/mtdN the way nandwrite -p writes them (bad
// blocks skipped, the rest of the partition left as it was), so the read
// back after it is a real one. SIM_FAIL_WRITE=1 fails halfway through the
// system; SIM_CORRUPT_WRITE=1 writes the system with one byte wrong.
static int sim_write_image(const image_t *im) {
	char dev[32];
	int num = mtd_find(image_part(im), dev, sizeof(dev));
	unsigned es = part_erasesize(num);
	int fd = mtd_open_rw(num);
	if (fd < 0) {
		return -1;
	}
	char cmd[700];
	snprintf(cmd, sizeof(cmd), "cat %s/%s.[0-9][0-9][0-9][0-9].*", checked_dir, im->name);
	FILE *p = popen(cmd, "r");
	uint8_t *buf = malloc(es);
	long long off = 0, left = im->size;
	int rc = (p && buf) ? 0 : -1;
	while (rc == 0 && left > 0) {
		if (mtd_block_bad(fd, off) == 1) {
			off += es;
			continue;
		}
		size_t n = fread(buf, 1, (size_t)(left < es ? left : es), p);
		if (n == 0) {
			rc = -1;
			break;
		}
		if (off == 0x20000 * 3 && strcmp(im->type, "rootfs") == 0 && getenv("SIM_CORRUPT_WRITE")) {
			buf[100] ^= 0x01;
			LOG("sim: SIM_CORRUPT_WRITE: one byte of the system written wrong");
		}
		if (mtd_erase_block(fd, off, es) != 0 || pwrite(fd, buf, n, (off_t)off) != (ssize_t)n) {
			rc = -1;
		}
		off += es;
		left -= (long long)n;
	}
	if (p) {
		pclose(p);
	}
	free(buf);
	close(fd);
	LOG("sim: %s written to %s", im->name, dev);
	return rc;
}

static int write_package(void) {
	const char *phases[] = {"Writing kernel", "Writing system"};
	for (int i = 0; i < n_checked; i++) {
		if (sim_write_image(&checked_img[i]) != 0) {
			return 1;
		}
	}
	for (int ph = 0; ph < 2; ph++) {
		pthread_mutex_lock(&lock);
		status.system_touched = 1;
		pthread_mutex_unlock(&lock);
		set_phase(phases[ph], -1);
		for (int pct = ph * 10; pct <= (ph ? 100 : 10); pct++) {
			if (ph == 1 && pct == 55 && getenv("SIM_FAIL_WRITE")) {
				LOG("sim: SIM_FAIL_WRITE set, failing");
				return 1;
			}
			set_percent(pct);
			usleep(30 * 1000);
		}
	}
	return bootflag_write(BOOT_NORMAL) == 0 ? 0 : 1; // the script's local_set_next_boot_device
}
#else
// Runs the stock writer, following its progress lines.
static int write_package(void) {
	FILE *p = popen("cd /tmp && /etc/ota_bin/local_ota_update.sh " UPT_MOUNT " 2>&1", "r");
	if (!p) {
		return -1;
	}
	char line[512];
	while (fgets(line, sizeof(line), p)) {
		line[strcspn(line, "\r\n")] = '\0';
		LOG("ota| %s", line);
		int pct;
		if (strstr(line, "start update kernel")) {
			pthread_mutex_lock(&lock);
			status.system_touched = 1;
			pthread_mutex_unlock(&lock);
			set_phase("Writing kernel", -1);
		} else if (strstr(line, "start update rootfs")) {
			pthread_mutex_lock(&lock);
			status.system_touched = 1;
			pthread_mutex_unlock(&lock);
			set_phase("Writing system", -1);
		} else if (sscanf(line, "ota: data processed: %d%%", &pct) == 1) {
			set_percent(pct);
		}
	}
	int rc = pclose(p);
	return (rc != -1 && WIFEXITED(rc)) ? WEXITSTATUS(rc) : -1;
}
#endif

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

	if (check_package(dir, sizeof(dir), card) != 0) {
		goto out;
	}

	// See update.h: the writer targets the kernel the record does not name.
	if (bootflag_read() != BOOT_RECOVERY) {
		set_phase("Setting the boot record", -1);
		if (bootflag_write(BOOT_RECOVERY) != 0) {
			finish(UPDATE_FAILED, "Cannot write the boot record; nothing was changed.");
			goto out;
		}
	}

	set_phase("Writing", -1);
	int rc = write_package();
	if (rc != 0) {
		update_status_t s;
		update_get(&s);
		finish(UPDATE_FAILED, "Writing failed (%d).%s", rc,
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
	finish(UPDATE_DONE, "The update is installed and checked: kernel and system read back as in the package.");

out:
	cleanup(card);
	return NULL;
}

int update_start(const char *path, bool override_compat) {
	pthread_mutex_lock(&lock);
	if (status.state == UPDATE_RUNNING) {
		pthread_mutex_unlock(&lock);
		return -1;
	}
	snprintf(requested, sizeof(requested), "%s", path ? path : "");
	ignore_compat = override_compat;
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
