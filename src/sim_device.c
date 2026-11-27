// The device, simulated for the PC build (make host). Replaces device.c and
// storage.c; nothing here touches the machine beyond $SIM_DIR.
//
//   $SIM_DIR            default ./sim (relative to where it is started)
//   $SIM_DIR/sd/        the microSD card: present when the folder exists.
//                       Put r1.upt (or r3proii.upt) in it to try an update;
//                       the package checks run for real.
//   $SIM_DIR/ota        the boot record, "ota:kernel" or "ota:kernel2"
//                       (created as ota:kernel2: you are in recovery)
//   $SIM_DIR/data/      what a factory reset empties
//   SIM_BATTERY=N       battery percentage (default 80)
//   SIM_CHARGING=1      on USB power
//   SIM_FAIL_WRITE=1    the write fails halfway (update.c)
//
// Reboot and power off log what the device would do and quit.

#include "device.h"
#include "log.h"
#include "storage.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *sim_dir(void) {
	const char *d = getenv("SIM_DIR");
	return d && d[0] ? d : "sim";
}

static void sim_path(char *out, size_t size, const char *name) { snprintf(out, size, "%s/%s", sim_dir(), name); }

// The stock tables: the same layout on both players; the recovery's root fs
// is a second "rootfs" on the R1 and "rootfs2" on the R3 Pro II.
#ifdef MODEL_R3PROII
#define SIM_ROOTFS2 "rootfs2"
#else
#define SIM_ROOTFS2 "rootfs"
#endif
static const struct {
	const char *name;
	long long size;
} PARTS[] = {
	{"uboot", 0x80000},		 {"kernel", 0x500000}, {"rootfs", 0x2d00000}, {"kernel2", 0x500000},
	{SIM_ROOTFS2, 0x1800000}, {"ota", 0x80000},	   {"userdata", 0x2d00000}, // truncated to the chip, as on the R1
};
#define N_PARTS (int)(sizeof(PARTS) / sizeof(PARTS[0]))

int mtd_find(const char *name, char *dev, size_t dev_size) {
	for (int i = 0; i < N_PARTS; i++) {
		if (strcmp(PARTS[i].name, name) == 0) {
			snprintf(dev, dev_size, "/dev/mtd%d", i);
			return i;
		}
	}
	return -1;
}

long long mtd_part_size(int num) { return (num >= 0 && num < N_PARTS) ? PARTS[num].size : -1; }

static int write_record(const char *record) {
	char p[512];
	mkdir(sim_dir(), 0755);
	sim_path(p, sizeof(p), "ota");
	FILE *f = fopen(p, "w");
	if (!f) {
		LOG("sim: cannot write %s", p);
		return -1;
	}
	fprintf(f, "%s\n", record);
	fclose(f);
	LOG("sim: boot record -> %s", record);
	return 0;
}

boot_target_t bootflag_read(void) {
	char p[512], buf[64] = "";
	sim_path(p, sizeof(p), "ota");
	FILE *f = fopen(p, "r");
	if (!f) {
		write_record("ota:kernel2");
		return BOOT_RECOVERY;
	}
	if (!fgets(buf, sizeof(buf), f)) {
		buf[0] = '\0';
	}
	fclose(f);
	if (strncmp(buf, "ota:kernel2", 11) == 0) {
		return BOOT_RECOVERY;
	}
	if (strncmp(buf, "ota:kernel", 10) == 0) {
		return BOOT_NORMAL;
	}
	return BOOT_UNKNOWN;
}

int bootflag_write(boot_target_t target) {
	if (target == BOOT_UNKNOWN) {
		return -1;
	}
	return write_record(target == BOOT_RECOVERY ? "ota:kernel2" : "ota:kernel");
}

int bootflag_request_fastboot(void) {
	LOG("sim: page 1 of ota <- ota:fastboot");
	return write_record("ota:kernel");
}

// The record file empty (an erased page), and $SIM_DIR/mtd5 all 0xff.
int ota_wipe(void) {
	char p[512];
	sim_path(p, sizeof(p), "ota");
	FILE *f = fopen(p, "w");
	if (!f) {
		return -1;
	}
	fclose(f);
	char dev[32];
	int num = mtd_find("ota", dev, sizeof(dev));
	if (num >= 0) {
		snprintf(dev, sizeof(dev), "mtd%d", num);
		sim_path(p, sizeof(p), dev);
		FILE *m = fopen(p, "wb");
		if (m) {
			for (long long i = 0; i < PARTS[num].size; i++) {
				fputc(0xff, m);
			}
			fclose(m);
		}
	}
	LOG("sim: flash_erase ota 0 0");
	return bootflag_read() == BOOT_UNKNOWN ? 0 : -1;
}

void battery_read(battery_t *out) {
	const char *b = getenv("SIM_BATTERY");
	out->capacity = b ? atoi(b) : 80;
	out->charging = getenv("SIM_CHARGING") != NULL;
}

void device_reboot(void) {
	LOG("sim: the device would reboot now, into %s", boot_target_name(bootflag_read()));
	exit(0);
}

void device_poweroff(void) {
	LOG("sim: the device would power off now");
	exit(0);
}

int device_factory_reset(bool keep_media_db) {
	char p[512];
	sim_path(p, sizeof(p), "data");
	LOG("sim: factory reset of %s%s", p, keep_media_db ? ", keeping usrlocal_media.db" : "");
	sleep(1);
	return run(keep_media_db ? "mkdir -p '%s' && find '%s' -mindepth 1 -maxdepth 1 ! -name usrlocal_media.db "
							   "-exec rm -rf {} +"
							 : "mkdir -p '%s' && find '%s' -mindepth 1 -maxdepth 1 -exec rm -rf {} +",
			   p, p);
}

void device_leds_purple(void) { LOG("sim: leds red and blue breathing (purple)"); }

int device_start_adb(void) {
	LOG("sim: /etc/init.d/T90adb start");
	return 0;
}

// The uboot partition: $SIM_DIR/mtd0, 512 KiB. Created as a stand-in for
// the stock loader (SPL magic, the stock partition table, no "ota:fastboot")
// when missing; copy a real dump there (~/r1-mtd/mtd0) to try with that. The
// table is the R1's firmware 2.0 one ("rootfs" twice) on the R1, the R3 Pro
// II's on that, so "Install fastboot" checks and fixes it as on the players.
// SIM_UBOOT_BAD=1 marks a bad block; SIM_UBOOT_CORRUPT=N spoils the first N
// writes (a read back that differs).
static void mtd0_path(char *out, size_t size) { sim_path(out, size, "mtd0"); }

static int ensure_mtd0(void) {
	char p[512];
	mtd0_path(p, sizeof(p));
	if (access(p, F_OK) == 0) {
		return 0;
	}
	static uint8_t img[0x80000];
	memset(img, 0xff, sizeof(img));
	static const uint8_t magic[8] = {0x06, 0x05, 0x04, 0x03, 0x02, 0x55, 0xaa, 0x55};
	memcpy(img, magic, sizeof(magic));
	// name, size, offset, mask_flags, manager (1: ubi), as parttable.h reads
	static const struct {
		const char *name;
		uint32_t size, offset, manager;
	} table[] = {
		{"uboot", 0x80000, 0, 0},			  {"kernel", 0x500000, 0x80000, 0},
		{"rootfs", 0x2d00000, 0x580000, 0},	  {"kernel2", 0x500000, 0x3280000, 0},
		{SIM_ROOTFS2, 0x1800000, 0x3780000, 0}, {"ota", 0x80000, 0x4f80000, 0},
		{"userdata", 0x3000000, 0x5000000, 1},
	};
	uint8_t *t = img + 0x5800;
	memcpy(t, "nand", 4);
	t[4] = sizeof(table) / sizeof(table[0]);
	t[5] = t[6] = t[7] = 0;
	for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		uint8_t *e = t + 8 + i * 48;
		uint32_t v[4] = {table[i].size, table[i].offset, 0, table[i].manager};
		memset(e, 0, 32);
		memcpy(e, table[i].name, strlen(table[i].name));
		for (int k = 0; k < 16; k++) {
			e[32 + k] = (uint8_t)(v[k / 4] >> (8 * (k % 4)));
		}
	}
	mkdir(sim_dir(), 0755);
	FILE *f = fopen(p, "wb");
	if (!f) {
		return -1;
	}
	fwrite(img, 1, sizeof(img), f);
	fclose(f);
	LOG("sim: created a stand-in stock loader in %s", p);
	return 0;
}

int uboot_part_read(uint8_t *buf, size_t len) {
	char p[512];
	if (ensure_mtd0() != 0) {
		return -1;
	}
	mtd0_path(p, sizeof(p));
	FILE *f = fopen(p, "rb");
	if (!f) {
		return -1;
	}
	size_t n = fread(buf, 1, len, f);
	fclose(f);
	return n == len ? 0 : -1;
}

int uboot_part_write(const char *file) {
	static int writes;
	char p[512];
	mtd0_path(p, sizeof(p));
	LOG("sim: flash_erase mtd0, nandwrite -p mtd0 %s", file);
	usleep(800 * 1000);
	static uint8_t img[0x80000];
	memset(img, 0xff, sizeof(img));
	FILE *in = fopen(file, "rb");
	if (!in) {
		return -1;
	}
	size_t n = fread(img, 1, sizeof(img), in);
	fclose(in);
	const char *c = getenv("SIM_UBOOT_CORRUPT");
	if (c && ++writes <= atoi(c)) {
		LOG("sim: SIM_UBOOT_CORRUPT: spoiling write %d", writes);
		img[100] ^= 0xff;
	}
	FILE *f = fopen(p, "wb");
	if (!f) {
		return -1;
	}
	fwrite(img, 1, sizeof(img), f);
	fclose(f);
	return n > 0 ? 0 : -1;
}

int uboot_part_bad_blocks(void) { return getenv("SIM_UBOOT_BAD") ? 1 : 0; }

// The bundled image: $SIM_DIR/share/ (copy output/<model>/u-boot-hiby_<model>.bin
// and its .md5 there; make sim-share does).
const char *bootloader_dir(void) {
	static char p[512];
	sim_path(p, sizeof(p), "share");
	return p;
}

// The other partitions: $SIM_DIR/mtd<N>, made (sparse, zeros) at their
// size when missing; mtd0 as above. SIM_MTD_BAD=N marks erase block 1 of
// mtdN bad; SIM_MTD_ECC=N gives mtdN an uncorrectable ECC error.
int mtd_list(mtd_part_t *out, int max) {
	int n = 0;
	for (int i = 0; i < N_PARTS && n < max; i++, n++) {
		out[n].num = i;
		snprintf(out[n].name, sizeof(out[n].name), "%s", PARTS[i].name);
		out[n].size = PARTS[i].size;
		out[n].erasesize = 0x20000;
	}
	return n;
}

static int sim_open_num = -1;

int mtd_open_ro(int num) {
	if (num < 0 || num >= N_PARTS) {
		return -1;
	}
	char p[512], name[16];
	snprintf(name, sizeof(name), "mtd%d", num);
	sim_path(p, sizeof(p), name);
	if (num == 0) {
		ensure_mtd0();
	} else if (access(p, F_OK) != 0) {
		mkdir(sim_dir(), 0755);
		int fd = open(p, O_WRONLY | O_CREAT, 0644);
		if (fd < 0 || ftruncate(fd, PARTS[num].size) != 0) {
			if (fd >= 0) {
				close(fd);
			}
			return -1;
		}
		// something to tell the partitions apart in the copies
		char tag[64];
		int l = snprintf(tag, sizeof(tag), "sim mtd%d %s", num, PARTS[num].name);
		if (write(fd, tag, (size_t)l) != l) {
			LOG("sim: cannot tag %s", p);
		}
		close(fd);
	}
	sim_open_num = num;
	return open(p, O_RDONLY);
}

int mtd_block_bad(int fd, long long off) {
	(void)fd;
	const char *b = getenv("SIM_MTD_BAD");
	return (b && atoi(b) == sim_open_num && off == 0x20000) ? 1 : 0;
}

static int sim_open_rw(int num) {
	int fd = mtd_open_ro(num); // makes the file when missing
	if (fd < 0) {
		return -1;
	}
	close(fd);
	char p[512], name[16];
	snprintf(name, sizeof(name), "mtd%d", num);
	sim_path(p, sizeof(p), name);
	// a link to a real dump is never written through: a copy takes its place
	struct stat st;
	if (lstat(p, &st) == 0 && S_ISLNK(st.st_mode)) {
		char tmp[530];
		snprintf(tmp, sizeof(tmp), "%s.copy", p);
		if (run("cp %s %s && mv %s %s", p, tmp, tmp, p) != 0) {
			return -1;
		}
	}
	sim_open_num = num;
	return open(p, O_RDWR);
}

static int sim_erase_block(int fd, long long off, unsigned size) {
	static uint8_t ff[0x20000];
	memset(ff, 0xff, sizeof(ff));
	return (size <= sizeof(ff) && pwrite(fd, ff, size, (off_t)off) == (ssize_t)size) ? 0 : -1;
}

// Images: as nandwrite -p lays them out. begin erases every good block, the
// data goes block by block into the good ones in order, and the last block
// is padded with 0xff (-k makes no difference here: an erased page is 0xff).
// SIM_FAIL_WRITE=1 makes the write into the first "rootfs" fail halfway;
// SIM_CORRUPT_WRITE=1 writes one byte of it wrong (block 3), for the read
// back to catch.
struct mtd_image_writer {
	int fd, num;
	long long off, size, sent;
	uint8_t blk[0x20000];
	size_t fill;
	bool rootfs, failed;
};

static int sim_image_flush(mtd_image_writer_t *w) {
	if (w->fill == 0) {
		return 0;
	}
	memset(w->blk + w->fill, 0xff, sizeof(w->blk) - w->fill);
	while (w->off < PARTS[w->num].size && mtd_block_bad(w->fd, w->off) == 1) {
		w->off += sizeof(w->blk); // nandwrite skips it
	}
	if (w->off >= PARTS[w->num].size) {
		LOG("sim: image does not fit mtd%d", w->num);
		return -1;
	}
	if (w->rootfs && w->off == 3 * (long long)sizeof(w->blk) && getenv("SIM_CORRUPT_WRITE")) {
		w->blk[100] ^= 0x01;
		LOG("sim: SIM_CORRUPT_WRITE: one byte of the system written wrong");
	}
	if (pwrite(w->fd, w->blk, sizeof(w->blk), (off_t)w->off) != (ssize_t)sizeof(w->blk)) {
		return -1;
	}
	w->off += sizeof(w->blk);
	w->fill = 0;
	return 0;
}

mtd_image_writer_t *mtd_image_begin(int num, long long size, unsigned flags) {
	if (num < 0 || num >= N_PARTS) {
		return NULL;
	}
	mtd_image_writer_t *w = calloc(1, sizeof(*w));
	if (!w) {
		return NULL;
	}
	w->fd = sim_open_rw(num);
	if (w->fd < 0) {
		free(w);
		return NULL;
	}
	w->num = num;
	w->size = size;
	char dev[32];
	w->rootfs = mtd_find("rootfs", dev, sizeof(dev)) == num;
	for (long long off = 0; off < PARTS[num].size; off += sizeof(w->blk)) {
		if (mtd_block_bad(w->fd, off) != 1 && sim_erase_block(w->fd, off, sizeof(w->blk)) != 0) {
			close(w->fd);
			free(w);
			return NULL;
		}
	}
	LOG("sim: flash_erase mtd%d; nandwrite%s%s -p --input-size=%lld", num, (flags & MTD_IMAGE_MARKBAD) ? " -m" : "",
		(flags & MTD_IMAGE_SKIP_FF) ? " -k" : "", size);
	return w;
}

int mtd_image_write(mtd_image_writer_t *w, const void *buf, size_t len) {
	const uint8_t *p = buf;
	while (len > 0 && !w->failed) {
		if (w->rootfs && getenv("SIM_FAIL_WRITE") && w->sent >= w->size / 2) {
			LOG("sim: SIM_FAIL_WRITE: nandwrite fails halfway through the system");
			w->failed = true;
			break;
		}
		size_t n = sizeof(w->blk) - w->fill;
		n = n < len ? n : len;
		memcpy(w->blk + w->fill, p, n);
		w->fill += n;
		w->sent += (long long)n;
		p += n;
		len -= n;
		if (w->fill == sizeof(w->blk) && sim_image_flush(w) != 0) {
			w->failed = true;
		}
	}
	return w->failed ? -1 : 0;
}

int mtd_image_end(mtd_image_writer_t *w) {
	if (!w) {
		return -1;
	}
	int rc = (!w->failed && w->sent == w->size && sim_image_flush(w) == 0) ? 0 : -1;
	close(w->fd);
	LOG("sim: nandwrite mtd%d: %lld of %lld bytes, %s", w->num, w->sent, w->size, rc ? "failed" : "ok");
	free(w);
	return rc;
}

// Reading: as nanddump --bb=padbad gives it. A block SIM_MTD_BAD marks is
// given as 0xff and counted; SIM_MTD_ECC=N reports one uncorrectable ECC
// error in mtdN (its data given as it is).
struct mtd_dump {
	int fd, num;
	long long off, length, end;
	uint8_t blk[0x20000];
	size_t have, pos;
	mtd_dump_stats_t st;
	bool failed;
};

mtd_dump_t *mtd_dump_begin(int num, long long length) { return mtd_dump_begin_at(num, 0, length); }

// SIM_MTD_EIO=N: erase block 2 of mtdN cannot be read at all (nanddump stops).
mtd_dump_t *mtd_dump_begin_at(int num, long long start, long long length) {
	if (num < 0 || num >= N_PARTS || start < 0 || start >= PARTS[num].size) {
		return NULL;
	}
	mtd_dump_t *d = calloc(1, sizeof(*d));
	if (!d) {
		return NULL;
	}
	d->fd = mtd_open_ro(num);
	if (d->fd < 0) {
		free(d);
		return NULL;
	}
	d->num = num;
	d->off = start;
	d->length = length > 0 && start + length < PARTS[num].size ? length : PARTS[num].size - start;
	d->end = start + d->length;
	const char *e = getenv("SIM_MTD_ECC");
	if (e && atoi(e) == num) {
		d->st.ecc_failed = 1;
		LOG("sim: nanddump mtd%d: ECC: 1 uncorrectable bitflip(s) at offset 0x00040000", num);
	}
	LOG("sim: nanddump -a --bb=padbad -s %lld -l %lld mtd%d", start, d->length, num);
	return d;
}

int mtd_dump_read(mtd_dump_t *d, void *buf, size_t len) {
	uint8_t *out = buf;
	while (len > 0 && !d->failed) {
		if (d->pos == d->have) {
			if (d->off >= d->end) {
				d->failed = true;
				break;
			}
			const char *eio = getenv("SIM_MTD_EIO");
			if (eio && atoi(eio) == d->num && d->off == 0x40000) {
				LOG("sim: nanddump mtd%d: pread at 0x40000: Input/output error", d->num);
				d->failed = true;
				break;
			}
			long long left = d->end - d->off;
			size_t want = (size_t)(left < (long long)sizeof(d->blk) ? left : (long long)sizeof(d->blk));
			if (mtd_block_bad(d->fd, d->off) == 1) {
				memset(d->blk, 0xff, want);
				d->st.bad_blocks++;
				LOG("sim: nanddump mtd%d: Bad block at %llx (given as 0xff)", d->num, d->off);
			} else if (pread(d->fd, d->blk, want, (off_t)d->off) != (ssize_t)want) {
				d->failed = true;
				break;
			}
			d->off += (long long)want;
			d->have = want;
			d->pos = 0;
		}
		size_t n = d->have - d->pos < len ? d->have - d->pos : len;
		memcpy(out, d->blk + d->pos, n);
		d->pos += n;
		out += n;
		len -= n;
	}
	return d->failed ? -1 : 0;
}

int mtd_dump_end(mtd_dump_t *d, mtd_dump_stats_t *st) {
	if (!d) {
		return -1;
	}
	int rc = d->failed ? -1 : 0;
	if (st) {
		*st = d->st;
	}
	close(d->fd);
	free(d);
	return rc;
}

int userdata_release(int num) {
	LOG("sim: umount /usr/data; ubidetach -m %d", num);
	return getenv("SIM_USERDATA_BUSY") ? -1 : 0;
}

// --- storage.h

bool storage_card_present(void) {
	char p[512];
	struct stat st;
	sim_path(p, sizeof(p), "sd");
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

// SIM_CARD_SIZE=bytes, default a 32 GB card's 31914983424.
long long storage_card_size(void) {
	if (!storage_card_present()) {
		return -1;
	}
	const char *e = getenv("SIM_CARD_SIZE");
	return e && e[0] ? atoll(e) : 31914983424LL;
}

// Read-only as on the player: the card folder (not what is in it) loses its
// write bit, so making a file or folder in its top fails as a read-only mount
// would. Writes deeper down are not caught.
static int sim_card_mode(bool rw) {
	char p[512];
	sim_path(p, sizeof(p), "sd");
	if (chmod(p, rw ? 0755 : 0555) != 0) {
		LOG("sim: chmod %s: %s", p, strerror(errno));
		return -1;
	}
	LOG("sim: card %s", rw ? "writable" : "read-only");
	return 0;
}

static bool sim_exported;
int storage_export(void) {
	sim_exported = true;
	LOG("sim: card unmounted for USB");
	return 0;
}
void storage_unexport(void) { sim_exported = false; }
bool storage_exported(void) { return sim_exported; }
const char *storage_card_disk(void) { return storage_card_present() ? "/dev/mmcblk0" : NULL; }

int storage_mount(int timeout_s, char *mnt, size_t mnt_size) {
	(void)timeout_s;
	if (sim_exported) {
		LOG("sim: the card is on USB, not mounted here");
		return -1;
	}
	if (!storage_card_present()) {
		LOG("sim: no card (%s/sd does not exist)", sim_dir());
		return -1;
	}
	sim_path(mnt, mnt_size, "sd");
	sim_card_mode(false);
	return 0;
}

int storage_set_writable(bool rw) { return storage_card_present() ? sim_card_mode(rw) : -1; }

void storage_unmount(void) {
	if (storage_card_present()) {
		sim_card_mode(false);
	}
}

// USB mass storage: the card is handed over and taken back, nothing more.
static bool sim_usb_storage;
bool device_usb_storage_on(void) { return sim_usb_storage; }
int device_usb_storage(bool on) {
	if (on == sim_usb_storage) {
		return 0;
	}
	if (on && (!storage_card_present() || getenv("SIM_USB_STORAGE_FAIL"))) {
		LOG("sim: usb storage: %s", storage_card_present() ? "SIM_USB_STORAGE_FAIL" : "no card");
		return -1;
	}
	if (on) {
		storage_export();
	} else {
		storage_unexport();
	}
	sim_usb_storage = on;
	LOG("sim: usb_dev_mass_storage.sh %s /dev/mmcblk0", on ? "start" : "stop");
	return 0;
}
