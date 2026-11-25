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

// The stock table, the same on both players.
static const struct {
	const char *name;
	long long size;
} PARTS[] = {
	{"uboot", 0x80000},	   {"kernel", 0x500000}, {"rootfs", 0x2d00000}, {"kernel2", 0x500000},
	{"rootfs", 0x1800000}, {"ota", 0x80000},	 {"userdata", 0x2d00000}, // truncated to the chip, as on the R1
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

int device_start_adb(void) {
	LOG("sim: /etc/init.d/T90adb start");
	return 0;
}

int squashfs_read_file(const char *image, const char *path, char *out, size_t size) {
	char q[600], cmd[800];
	snprintf(cmd, sizeof(cmd), "unsquashfs -cat %s %s 2>/dev/null", shq(image, q, sizeof(q)), path);
	FILE *p = popen(cmd, "r");
	if (!p) {
		return -1;
	}
	int n = (int)fread(out, 1, size - 1, p);
	out[n] = '\0';
	return pclose(p) == 0 ? n : -1;
}

// The uboot partition: $SIM_DIR/mtd0, 512 KiB. Created as a stand-in for
// the stock loader (SPL magic, a partition table, no "ota:fastboot") when
// missing; copy a real dump there (~/r1-mtd/mtd0) to try with that.
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
	memcpy(img + 0x5800, "nand\x07\0\0\0uboot", 13);
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

long long mtd_ecc_failed(int fd) {
	(void)fd;
	static int reads[16];
	const char *e = getenv("SIM_MTD_ECC");
	// first call per partition: 0; the next ones: 1 for the chosen one
	int n = sim_open_num;
	return (e && atoi(e) == n && n >= 0 && n < 16 && reads[n]++ > 0) ? 1 : 0;
}

// SIM_THEME="theme:1,a3,c0" (light, green) or $SIM_DIR/theme; dark and
// blue without either.
void theme_marker_read(char *out, size_t size) {
	const char *t = getenv("SIM_THEME");
	out[0] = '\0';
	if (t) {
		snprintf(out, size, "%s", t);
		return;
	}
	char p[512];
	sim_path(p, sizeof(p), "theme");
	FILE *f = fopen(p, "r");
	if (f) {
		if (!fgets(out, (int)size, f)) {
			out[0] = '\0';
		}
		out[strcspn(out, "\r\n")] = '\0';
		fclose(f);
	}
}

int mtd_open_rw(int num) {
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

int mtd_erase_block(int fd, long long off, unsigned size) {
	static uint8_t ff[0x20000];
	memset(ff, 0xff, sizeof(ff));
	return (size <= sizeof(ff) && pwrite(fd, ff, size, (off_t)off) == (ssize_t)size) ? 0 : -1;
}

unsigned mtd_writesize(int fd) {
	(void)fd;
	return 2048;
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

int storage_mount(int timeout_s, char *mnt, size_t mnt_size) {
	(void)timeout_s;
	if (!storage_card_present()) {
		LOG("sim: no card (%s/sd does not exist)", sim_dir());
		return -1;
	}
	sim_path(mnt, mnt_size, "sd");
	return 0;
}

void storage_unmount(void) {}
