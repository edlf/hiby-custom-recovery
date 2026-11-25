#include "backup.h"

#include "device.h"
#include "log.h"
#include "md5.h"
#include "model.h"
#include "storage.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define MAX_PARTS 16

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static backup_status_t status;
static bool opt_userdata;

void backup_get(backup_status_t *out) {
	pthread_mutex_lock(&lock);
	*out = status;
	pthread_mutex_unlock(&lock);
}

static void set_phase(int percent, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_phase(int percent, const char *fmt, ...) {
	va_list ap;
	pthread_mutex_lock(&lock);
	va_start(ap, fmt);
	vsnprintf(status.phase, sizeof(status.phase), fmt, ap);
	va_end(ap);
	status.percent = percent;
	pthread_mutex_unlock(&lock);
}

static void finish(backup_state_t st, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void finish(backup_state_t st, const char *fmt, ...) {
	va_list ap;
	pthread_mutex_lock(&lock);
	va_start(ap, fmt);
	vsnprintf(status.message, sizeof(status.message), fmt, ap);
	va_end(ap);
	status.state = st;
	pthread_mutex_unlock(&lock);
	LOG("backup: %s: %s", st == BACKUP_DONE ? "done" : "FAILED", status.message);
}

static bool wanted(const mtd_part_t *p, bool with_userdata) {
	return with_userdata || strcmp(p->name, "userdata") != 0;
}

long long backup_size(bool with_userdata) {
	mtd_part_t parts[MAX_PARTS];
	int n = mtd_list(parts, MAX_PARTS);
	long long total = 0;
	for (int i = 0; i < n; i++) {
		if (wanted(&parts[i], with_userdata)) {
			total += parts[i].size;
		}
	}
	return n > 0 ? total : -1;
}

// A new folder on the card, named by the date when the clock has one.
static int make_folder(const char *card, char *out, size_t size) {
	time_t now = time(NULL);
	struct tm tm;
	if (localtime_r(&now, &tm) && tm.tm_year + 1900 >= 2024) {
		char stamp[32];
		strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
		snprintf(out, size, "%s/sonix-backup-%s", card, stamp);
		if (mkdir(out, 0755) == 0) {
			return 0;
		}
	}
	for (int i = 1; i < 1000; i++) {
		snprintf(out, size, "%s/sonix-backup-%d", card, i);
		if (mkdir(out, 0755) == 0) {
			return 0;
		}
		if (errno != EEXIST) {
			return -1;
		}
	}
	return -1;
}

static int file_md5(const char *path, char out[33]) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return -1;
	}
	static uint8_t buf[64 * 1024];
	md5_ctx c;
	md5_init(&c);
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		md5_update(&c, buf, n);
	}
	int err = ferror(f);
	fclose(f);
	if (err) {
		return -1;
	}
	md5_final_hex(&c, out);
	return 0;
}

typedef struct {
	int bad_blocks;
	long long ecc_failed;
	int read_errors;
	char md5[33];
} part_result_t;

// One partition into `path`. done/total: bytes for the overall percentage.
static int copy_part(const mtd_part_t *p, const char *path, long long done, long long total, part_result_t *r) {
	memset(r, 0, sizeof(*r));
	int fd = mtd_open_ro(p->num);
	if (fd < 0) {
		return -1;
	}
	FILE *out = fopen(path, "wb");
	if (!out) {
		close(fd);
		return -2;
	}
	unsigned es = p->erasesize ? p->erasesize : 0x20000;
	uint8_t *buf = malloc(es);
	if (!buf) {
		fclose(out);
		close(fd);
		return -1;
	}
	long long ecc0 = mtd_ecc_failed(fd);
	md5_ctx c;
	md5_init(&c);
	int rc = 0;
	for (long long off = 0; off < p->size; off += es) {
		size_t want = (size_t)((p->size - off) < es ? (p->size - off) : es);
		if (mtd_block_bad(fd, off) == 1) {
			memset(buf, 0xff, want);
			r->bad_blocks++;
			LOG("backup: mtd%d: bad block at 0x%llx, written as 0xff", p->num, off);
		} else {
			if (lseek(fd, (off_t)off, SEEK_SET) != (off_t)off) {
				rc = -1;
				break;
			}
			size_t got = 0;
			while (got < want) {
				ssize_t n = read(fd, buf + got, want - got);
				if (n <= 0) {
					break;
				}
				got += (size_t)n;
			}
			if (got < want) {
				memset(buf + got, 0xff, want - got);
				r->read_errors++;
				LOG("backup: mtd%d: read error at 0x%llx, rest of the block written as 0xff", p->num, off + got);
			}
		}
		md5_update(&c, buf, want);
		if (fwrite(buf, 1, want, out) != want) {
			rc = -2;
			break;
		}
		int pct = (int)((done + off + (long long)want) * 100 / total);
		set_phase(pct, "mtd%d %s: %lld of %lld MB", p->num, p->name, (off + (long long)want) >> 20, p->size >> 20);
	}
	long long ecc1 = mtd_ecc_failed(fd);
	if (ecc0 >= 0 && ecc1 > ecc0) {
		r->ecc_failed = ecc1 - ecc0;
	}
	free(buf);
	close(fd);
	if (fflush(out) != 0 || fsync(fileno(out)) != 0) {
		rc = rc ? rc : -2;
	}
	if (fclose(out) != 0) {
		rc = rc ? rc : -2;
	}
	md5_final_hex(&c, r->md5);
	return rc;
}

static void *worker(void *arg) {
	(void)arg;
	char card[256], folder[400], path[600];
	mtd_part_t parts[MAX_PARTS];
	FILE *sums = NULL, *info = NULL;
	bool card_mounted = false;

	set_phase(0, "Looking for the card");
	if (storage_mount(10, card, sizeof(card)) != 0) {
		finish(BACKUP_FAILED, "No microSD card could be read (FAT32 or exFAT).");
		goto out;
	}
	card_mounted = true;
	int n = mtd_list(parts, MAX_PARTS);
	if (n <= 0) {
		finish(BACKUP_FAILED, "The partition table (/proc/mtd) cannot be read.");
		goto out;
	}
	long long total = 0;
	for (int i = 0; i < n; i++) {
		if (wanted(&parts[i], opt_userdata)) {
			total += parts[i].size;
		}
	}
	struct statvfs vfs;
	if (statvfs(card, &vfs) == 0) {
		long long avail = (long long)vfs.f_bavail * (long long)vfs.f_frsize;
		if (avail < total + (1 << 20)) {
			finish(BACKUP_FAILED, "The backup needs %lld MB and the card has %lld MB free.", (total >> 20) + 1,
				   avail >> 20);
			goto out;
		}
	}
	if (make_folder(card, folder, sizeof(folder)) != 0) {
		finish(BACKUP_FAILED, "Cannot make a folder on the card.");
		goto out;
	}
	const char *fname = strrchr(folder, '/') + 1;
	LOG("backup: into %s, %lld bytes%s", folder, total, opt_userdata ? "" : " (no userdata)");

	snprintf(path, sizeof(path), "%s/mtd.txt", folder);
	FILE *t = fopen(path, "w");
	if (t) {
		fprintf(t, "dev:    size   erasesize  name\n");
		for (int i = 0; i < n; i++) {
			fprintf(t, "mtd%d: %08llx %08x \"%s\"\n", parts[i].num, parts[i].size, parts[i].erasesize, parts[i].name);
		}
		fclose(t);
	}
	snprintf(path, sizeof(path), "%s/md5sums.txt", folder);
	sums = fopen(path, "w");
	snprintf(path, sizeof(path), "%s/info.txt", folder);
	info = fopen(path, "w");
	if (!sums || !info) {
		finish(BACKUP_FAILED, "Cannot write to the card.");
		goto out;
	}
	fprintf(info, "%s, Sonix Recovery %s\n\n", g_model.name, SONIX_RECOVERY_VERSION);

	long long done = 0;
	int copied = 0, flagged = 0;
	char flagged_names[160] = "";
	for (int i = 0; i < n; i++) {
		if (!wanted(&parts[i], opt_userdata)) {
			continue;
		}
		char file[64];
		snprintf(file, sizeof(file), "mtd%d-%s.bin", parts[i].num, parts[i].name);
		snprintf(path, sizeof(path), "%s/%s", folder, file);
		part_result_t r;
		int rc = copy_part(&parts[i], path, done, total, &r);
		if (rc != 0) {
			if (rc == -2) {
				finish(BACKUP_FAILED, "Writing %s to the card failed (card full or removed?).", file);
			} else {
				finish(BACKUP_FAILED, "Reading mtd%d failed.", parts[i].num);
			}
			goto out;
		}
		set_phase((int)((done + parts[i].size) * 100 / total), "Checking %s on the card", file);
		char again[33];
		if (file_md5(path, again) != 0 || strcmp(again, r.md5) != 0) {
			finish(BACKUP_FAILED, "The copy of mtd%d on the card does not read back the same. Try another card.",
				   parts[i].num);
			goto out;
		}
		fprintf(sums, "%s  %s\n", r.md5, file);
		fprintf(info, "%-22s %10lld bytes  bad blocks %d  uncorrectable ECC %lld  read errors %d\n", file,
				parts[i].size, r.bad_blocks, r.ecc_failed, r.read_errors);
		if (r.bad_blocks || r.ecc_failed || r.read_errors) {
			flagged++;
			size_t l = strlen(flagged_names);
			snprintf(flagged_names + l, sizeof(flagged_names) - l, "%smtd%d", l ? ", " : "", parts[i].num);
		}
		LOG("backup: %s md5 %s, checked on the card", file, r.md5);
		done += parts[i].size;
		copied++;
	}
	fclose(sums);
	sums = NULL;
	fclose(info);
	info = NULL;
	sync();
	if (flagged) {
		pthread_mutex_lock(&lock);
		status.warnings = true;
		pthread_mutex_unlock(&lock);
		finish(BACKUP_DONE, "Saved %d partitions (%lld MB) in %s. %s had bad blocks or ECC/read errors; see info.txt.",
			   copied, total >> 20, fname, flagged_names);
	} else {
		finish(BACKUP_DONE, "Saved %d partitions (%lld MB) in %s, each checked on the card.", copied, total >> 20,
			   fname);
	}

out:
	if (sums) {
		fclose(sums);
	}
	if (info) {
		fclose(info);
	}
	if (card_mounted) {
		char dst[400], q[500];
		snprintf(dst, sizeof(dst), "%s/sonix_recovery.log", card);
		run("cp %s %s", LOG_FILE, shq(dst, q, sizeof(q)));
		storage_unmount();
	}
	return NULL;
}

int backup_start(bool with_userdata) {
	pthread_mutex_lock(&lock);
	if (status.state == BACKUP_RUNNING) {
		pthread_mutex_unlock(&lock);
		return -1;
	}
	memset(&status, 0, sizeof(status));
	status.state = BACKUP_RUNNING;
	opt_userdata = with_userdata;
	pthread_mutex_unlock(&lock);
	pthread_t t;
	if (pthread_create(&t, NULL, worker, NULL) != 0) {
		finish(BACKUP_FAILED, "Cannot start the backup thread.");
		return -1;
	}
	pthread_detach(t);
	return 0;
}
