#include "restore.h"

#include "device.h"
#include "log.h"
#include "md5.h"
#include "model.h"
#include "storage.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static restore_status_t status;
static char req_dir[512];
static uint32_t req_mask;

void restore_get(restore_status_t *out) {
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

static void finish(restore_state_t st, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void finish(restore_state_t st, const char *fmt, ...) {
	va_list ap;
	pthread_mutex_lock(&lock);
	va_start(ap, fmt);
	vsnprintf(status.message, sizeof(status.message), fmt, ap);
	va_end(ap);
	status.state = st;
	pthread_mutex_unlock(&lock);
	LOG("restore: %s: %s", st == RESTORE_DONE ? "done" : "FAILED", status.message);
}

static void mark_wrote(void) {
	pthread_mutex_lock(&lock);
	status.wrote = true;
	pthread_mutex_unlock(&lock);
}

// "HiBy R1" out of info.txt's first line, "HiBy R1, Zee's Recovery ...".
static void read_model(const char *dir, char *out, size_t size) {
	char path[600];
	snprintf(path, sizeof(path), "%s/info.txt", dir);
	out[0] = '\0';
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	char line[160];
	if (fgets(line, sizeof(line), f)) {
		line[strcspn(line, ",\r\n")] = '\0';
		snprintf(out, size, "%s", line);
	}
	fclose(f);
}

static int entry_cmp(const void *a, const void *b) {
	return -strcmp(((const backup_entry_t *)a)->name, ((const backup_entry_t *)b)->name);
}

int restore_list_backups(backup_entry_t *out, int max) {
	char card[256];
	if (storage_mount(3, card, sizeof(card)) != 0) {
		return -1;
	}
	DIR *d = opendir(card);
	if (!d) {
		return -1;
	}
	int n = 0;
	struct dirent *e;
	while (n < max && (e = readdir(d))) {
		if (strncmp(e->d_name, "zees-backup-", 12) != 0) {
			continue;
		}
		backup_entry_t *b = &out[n];
		snprintf(b->dir, sizeof(b->dir), "%s/%s", card, e->d_name);
		snprintf(b->name, sizeof(b->name), "%s", e->d_name);
		restore_part_t parts[RESTORE_MAX_PARTS];
		int np = restore_scan(b->dir, parts, RESTORE_MAX_PARTS);
		if (np <= 0) {
			continue; // not a complete backup
		}
		read_model(b->dir, b->model, sizeof(b->model));
		b->same_model = strcmp(b->model, g_model.name) == 0;
		b->n_files = np;
		b->bytes = 0;
		for (int i = 0; i < np; i++) {
			b->bytes += parts[i].size;
		}
		n++;
	}
	closedir(d);
	qsort(out, (size_t)n, sizeof(out[0]), entry_cmp);
	return n;
}

// The recovery's root fs is "rootfs2" in the R3 Pro II's stock table and a
// second "rootfs" in the R1's (where it may be renamed "rootfs2" too): the two
// names are the same partition, for telling the partitions apart and for
// matching a backup against the table it was made with.
static const char *canon_name(const char *name) { return strcmp(name, "rootfs2") == 0 ? "rootfs" : name; }

static bool same_name(const char *a, const char *b) { return strcmp(canon_name(a), canon_name(b)) == 0; }

// The running recovery: kernel2, and its root fs: "rootfs2", or a "rootfs"
// that is not the first one (the first is the system's).
static bool is_recovery_part(const mtd_part_t *p) {
	char dev[32];
	if (strcmp(p->name, "kernel2") == 0 || strcmp(p->name, "rootfs2") == 0) {
		return true;
	}
	return strcmp(p->name, "rootfs") == 0 && mtd_find("rootfs", dev, sizeof(dev)) != p->num;
}

int restore_scan(const char *dir, restore_part_t *out, int max) {
	char path[600];
	snprintf(path, sizeof(path), "%s/md5sums.txt", dir);
	FILE *f = fopen(path, "r");
	if (!f) {
		return -1;
	}
	mtd_part_t cur[RESTORE_MAX_PARTS];
	int nc = mtd_list(cur, RESTORE_MAX_PARTS);
	char line[200];
	int n = 0;
	while (n < max && fgets(line, sizeof(line), f)) {
		char md5[40], file[64];
		int num;
		if (sscanf(line, "%39s %63s", md5, file) != 2 || sscanf(file, "mtd%d-", &num) != 1) {
			continue;
		}
		restore_part_t *r = &out[n];
		memset(r, 0, sizeof(*r));
		r->num = num;
		snprintf(r->file, sizeof(r->file), "%s", file);
		const char *nm = strchr(file, '-') + 1;
		snprintf(r->name, sizeof(r->name), "%.*s", (int)(strlen(nm) > 4 ? strlen(nm) - 4 : 0), nm); // "...bin"
		snprintf(path, sizeof(path), "%s/%s", dir, file);
		struct stat st;
		if (stat(path, &st) != 0) {
			continue;
		}
		r->size = (long long)st.st_size;
		const mtd_part_t *c = NULL;
		for (int i = 0; i < nc; i++) {
			if (cur[i].num == num) {
				c = &cur[i];
			}
		}
		if (!c || !same_name(c->name, r->name) || c->size != r->size) {
			snprintf(r->why_not, sizeof(r->why_not), "not this player's mtd%d", num);
		} else if (is_recovery_part(c)) {
			snprintf(r->why_not, sizeof(r->why_not), "the running recovery: use fastboot");
		} else {
			r->restorable = true;
			r->default_on = strcmp(r->name, "kernel") == 0 || strcmp(r->name, "rootfs") == 0;
		}
		n++;
	}
	fclose(f);
	return n;
}

// ---------------------------------------------------------------------------
// The worker

static bool all_ff(const uint8_t *p, size_t n) {
	for (size_t i = 0; i < n; i++) {
		if (p[i] != 0xff) {
			return false;
		}
	}
	return true;
}

static int file_md5(const char *path, char out[33], long long total, long long *done) {
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return -1;
	}
	static uint8_t buf[128 * 1024];
	md5_ctx c;
	md5_init(&c);
	ssize_t n;
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		md5_update(&c, buf, (size_t)n);
		*done += n;
		pthread_mutex_lock(&lock);
		status.percent = (int)(*done * 100 / total);
		pthread_mutex_unlock(&lock);
	}
	close(fd);
	if (n < 0) {
		return -1;
	}
	md5_final_hex(&c, out);
	return 0;
}

static bool mtd_txt_matches(const char *dir) {
	char path[600];
	snprintf(path, sizeof(path), "%s/mtd.txt", dir);
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	mtd_part_t cur[RESTORE_MAX_PARTS];
	int nc = mtd_list(cur, RESTORE_MAX_PARTS), i = 0;
	char line[160];
	bool ok = nc > 0 && fgets(line, sizeof(line), f) != NULL; // header
	while (ok && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!line[0]) {
			continue;
		}
		if (i >= nc) {
			ok = false;
			break;
		}
		// the same line, but "rootfs2" may stand where the other has "rootfs"
		unsigned long long sz;
		unsigned int es;
		int num;
		char name[64];
		ok = sscanf(line, "mtd%d: %llx %x \"%63[^\"]\"", &num, &sz, &es, name) == 4 && num == cur[i].num &&
			 (long long)sz == cur[i].size && es == cur[i].erasesize && same_name(name, cur[i].name);
		i++;
	}
	fclose(f);
	return ok && i == nc;
}

// Writes `file` onto mtd<num> with the mtd-utils, then reads it back with
// them (device.h): flash_erase, then nandwrite -k fed the backup without the
// blocks that are bad on the flash now. Those hold only 0xff in the backup
// (check_bad_blocks), and nandwrite skips them, so every other block lands at
// the offset it was read from. No -m: a block that fails to write would push
// the rest one block on, so the write fails instead (and is tried once more
// by the worker). Then nanddump --bb=padbad, compared block by block with the
// backup, the bad blocks left out. 0, or -1 with the reason logged. With
// `want` the data comes from there instead (the bootloader's old contents).
static int write_part(const restore_part_t *p, unsigned es, const char *file, const uint8_t *want, long long done,
					  long long total) {
	int in = want ? -1 : open(file, O_RDONLY);
	int ro = mtd_open_ro(p->num); // for which blocks are bad (an ioctl, no data read)
	uint8_t *src = malloc(es), *back = malloc(es);
	mtd_image_writer_t *w = NULL;
	mtd_dump_t *d = NULL;
	int rc = 0;
	if ((!want && in < 0) || ro < 0 || !src || !back) {
		LOG("restore: mtd%d: cannot open", p->num);
		rc = -1;
		goto out;
	}
	long long good = 0;
	int nbad = 0;
	for (long long off = 0; off < p->size; off += es) {
		if (mtd_block_bad(ro, off) == 1) {
			nbad++;
		} else {
			good += es;
		}
	}
	if (nbad) {
		LOG("restore: mtd%d: %d bad blocks, skipped (the backup has only 0xff there)", p->num, nbad);
	}

	w = mtd_image_begin(p->num, good, MTD_IMAGE_SKIP_FF);
	if (!w) {
		LOG("restore: mtd%d: flash_erase or nandwrite cannot be started", p->num);
		rc = -1;
		goto out;
	}
	for (long long off = 0; off < p->size && rc == 0; off += es) {
		if (mtd_block_bad(ro, off) == 1) {
			continue;
		}
		if (want) {
			memcpy(src, want + off, es);
		} else if (pread(in, src, es, (off_t)off) != (ssize_t)es) {
			LOG("restore: mtd%d: the file cannot be read at 0x%llx", p->num, off);
			rc = -1;
			break;
		}
		if (mtd_image_write(w, src, es) != 0) {
			LOG("restore: mtd%d: nandwrite stopped taking data at 0x%llx", p->num, off);
			rc = -1;
			break;
		}
		set_phase((int)((done + off + es) * 100 / total), "Writing mtd%d %s: %lld of %lld MB", p->num, p->name,
				  (off + es) >> 20, p->size >> 20);
	}
	if (mtd_image_end(w) != 0 && rc == 0) {
		LOG("restore: mtd%d: nandwrite failed", p->num);
		rc = -1;
	}
	w = NULL;
	if (rc != 0) {
		goto out;
	}

	set_phase((int)((done + p->size) * 100 / total), "Checking mtd%d %s", p->num, p->name);
	d = mtd_dump_begin(p->num, p->size);
	if (!d) {
		LOG("restore: mtd%d: nanddump cannot be started", p->num);
		rc = -1;
		goto out;
	}
	for (long long off = 0; off < p->size; off += es) {
		if (mtd_dump_read(d, back, es) != 0) {
			LOG("restore: mtd%d: cannot be read back at 0x%llx", p->num, off);
			rc = -1;
			break;
		}
		if (mtd_block_bad(ro, off) == 1) {
			continue; // given as 0xff, and not written
		}
		if (want) {
			memcpy(src, want + off, es);
		} else if (pread(in, src, es, (off_t)off) != (ssize_t)es) {
			rc = -1;
			break;
		}
		if (memcmp(back, src, es) != 0) {
			LOG("restore: mtd%d: read back differs at 0x%llx", p->num, off);
			rc = -1;
			break;
		}
	}
	if (mtd_dump_end(d, NULL) != 0 && rc == 0) {
		LOG("restore: mtd%d: nanddump failed", p->num);
		rc = -1;
	}
out:
	if (in >= 0) {
		close(in);
	}
	if (ro >= 0) {
		close(ro);
	}
	free(src);
	free(back);
	return rc;
}

// A block that has gone bad on the flash where the backup has data refuses
// the partition: written elsewhere, the data would not be where it was.
static int check_bad_blocks(const restore_part_t *p, unsigned es, const char *file) {
	int fd = mtd_open_ro(p->num), in = open(file, O_RDONLY);
	uint8_t *src = malloc(es);
	int rc = 0;
	if (fd < 0 || in < 0 || !src) {
		rc = -1;
	}
	for (long long off = 0; rc == 0 && off < p->size; off += es) {
		int bad = mtd_block_bad(fd, off);
		if (bad < 0) {
			rc = -1;
		} else if (bad == 1) {
			if (pread(in, src, es, (off_t)off) != (ssize_t)es || !all_ff(src, es)) {
				LOG("restore: mtd%d: block at 0x%llx is bad now and holds data in the backup", p->num, off);
				rc = 1;
			}
		}
	}
	if (fd >= 0) {
		close(fd);
	}
	if (in >= 0) {
		close(in);
	}
	free(src);
	return rc;
}

static void *worker(void *arg) {
	(void)arg;
	restore_part_t parts[RESTORE_MAX_PARTS], *sel[RESTORE_MAX_PARTS];
	mtd_part_t cur[RESTORE_MAX_PARTS];
	int ns = 0;
	long long total = 0;
	uint8_t *bl_old = NULL;
	char path[700];
	const char *dir = req_dir;
	const char *fname = strrchr(dir, '/') ? strrchr(dir, '/') + 1 : dir;

	set_phase(0, "Reading %s", fname);
	char card[256];
	if (storage_mount(5, card, sizeof(card)) != 0) {
		finish(RESTORE_FAILED, "No microSD card could be read.");
		goto out;
	}
	char model[96];
	read_model(dir, model, sizeof(model));
	if (strcmp(model, g_model.name) != 0) {
		finish(RESTORE_FAILED, "This backup is from a '%s', not this %s. Nothing was written.",
			   model[0] ? model : "unknown player", g_model.name);
		goto out;
	}
	if (!mtd_txt_matches(dir)) {
		finish(RESTORE_FAILED, "The backup's partition table (mtd.txt) is not this player's. Nothing was written.");
		goto out;
	}
	int np = restore_scan(dir, parts, RESTORE_MAX_PARTS);
	int nc = mtd_list(cur, RESTORE_MAX_PARTS);
	for (int i = 0; i < np; i++) {
		if (parts[i].num < 32 && (req_mask & (1u << parts[i].num))) {
			if (!parts[i].restorable) {
				finish(RESTORE_FAILED, "mtd%d cannot be restored here (%s). Nothing was written.", parts[i].num,
					   parts[i].why_not);
				goto out;
			}
			sel[ns++] = &parts[i];
			total += parts[i].size;
		}
	}
	if (ns == 0) {
		finish(RESTORE_FAILED, "Nothing was chosen.");
		goto out;
	}

	// md5 of every chosen file, against md5sums.txt
	long long done = 0;
	for (int i = 0; i < ns; i++) {
		set_phase((int)(done * 100 / total), "Checking %s", sel[i]->file);
		char want[40] = "", have[33];
		snprintf(path, sizeof(path), "%s/md5sums.txt", dir);
		FILE *f = fopen(path, "r");
		char line[200], m[40], fl[64];
		while (f && fgets(line, sizeof(line), f)) {
			if (sscanf(line, "%39s %63s", m, fl) == 2 && strcmp(fl, sel[i]->file) == 0) {
				snprintf(want, sizeof(want), "%s", m);
			}
		}
		if (f) {
			fclose(f);
		}
		snprintf(path, sizeof(path), "%s/%s", dir, sel[i]->file);
		if (file_md5(path, have, total, &done) != 0 || strcmp(want, have) != 0) {
			finish(RESTORE_FAILED, "%s does not match its md5 in md5sums.txt. Nothing was written.", sel[i]->file);
			goto out;
		}
	}

	unsigned es_of[RESTORE_MAX_PARTS];
	for (int i = 0; i < ns; i++) {
		es_of[i] = 0x20000;
		for (int j = 0; j < nc; j++) {
			if (cur[j].num == sel[i]->num && cur[j].erasesize) {
				es_of[i] = cur[j].erasesize;
			}
		}
		snprintf(path, sizeof(path), "%s/%s", dir, sel[i]->file);
		int bad = check_bad_blocks(sel[i], es_of[i], path);
		if (bad != 0) {
			finish(RESTORE_FAILED,
				   bad > 0 ? "mtd%d has a block gone bad where the backup has data: it cannot be put back exactly. "
							 "Nothing was written."
						   : "mtd%d's blocks cannot be checked. Nothing was written.",
				   sel[i]->num);
			goto out;
		}
	}

	for (int i = 0; i < ns; i++) {
		if (strcmp(sel[i]->name, "userdata") == 0) {
			set_phase(0, "Unmounting the user data");
			if (userdata_release(sel[i]->num) != 0) {
				finish(RESTORE_FAILED, "/usr/data is in use and cannot be unmounted. Nothing was written.");
				goto out;
			}
		}
		if (strcmp(sel[i]->name, "uboot") == 0) {
			bl_old = malloc((size_t)sel[i]->size);
			if (!bl_old || uboot_part_read(bl_old, (size_t)sel[i]->size) != 0) {
				finish(RESTORE_FAILED, "The current bootloader cannot be read. Nothing was written.");
				goto out;
			}
		}
	}

	done = 0;
	char failed[160] = "";
	for (int i = 0; i < ns; i++) {
		snprintf(path, sizeof(path), "%s/%s", dir, sel[i]->file);
		LOG("restore: mtd%d %s from %s", sel[i]->num, sel[i]->name, path);
		mark_wrote();
		int rc = write_part(sel[i], es_of[i], path, NULL, done, total);
		if (rc != 0) {
			set_phase((int)(done * 100 / total), "mtd%d read back differs, writing again", sel[i]->num);
			rc = write_part(sel[i], es_of[i], path, NULL, done, total);
		}
		if (rc != 0 && bl_old && strcmp(sel[i]->name, "uboot") == 0) {
			set_phase((int)(done * 100 / total), "Putting the old bootloader back");
			if (write_part(sel[i], es_of[i], NULL, bl_old, done, total) != 0 &&
				write_part(sel[i], es_of[i], NULL, bl_old, done, total) != 0) {
				finish(RESTORE_FAILED_BL, "The bootloader partition does not read back correctly. Do not turn the "
										  "player off; restore it again.");
				goto out;
			}
			LOG("restore: the old bootloader is back");
		}
		if (rc != 0) {
			size_t l = strlen(failed);
			snprintf(failed + l, sizeof(failed) - l, "%smtd%d", l ? ", " : "", sel[i]->num);
		}
		done += sel[i]->size;
	}
	sync();
	if (failed[0]) {
		finish(RESTORE_FAILED, "%s did not read back correctly (see the log); the rest was restored.%s", failed,
			   bl_old ? " The bootloader is as it was or as in the backup." : "");
	} else {
		finish(RESTORE_DONE, "Restored %d partition%s from %s. Restart the player.", ns, ns == 1 ? "" : "s", fname);
	}

out:
	free(bl_old);
	storage_unmount();
	return NULL;
}

int restore_start(const char *dir, uint32_t mask) {
	pthread_mutex_lock(&lock);
	if (status.state == RESTORE_RUNNING) {
		pthread_mutex_unlock(&lock);
		return -1;
	}
	memset(&status, 0, sizeof(status));
	status.state = RESTORE_RUNNING;
	snprintf(req_dir, sizeof(req_dir), "%s", dir);
	req_mask = mask;
	pthread_mutex_unlock(&lock);
	pthread_t t;
	if (pthread_create(&t, NULL, worker, NULL) != 0) {
		finish(RESTORE_FAILED, "Cannot start the restore thread.");
		return -1;
	}
	pthread_detach(t);
	return 0;
}
