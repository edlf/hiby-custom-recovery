#include "device.h"

#include "bootloader.h"
#include "log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mtd/mtd-user.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int mtd_find(const char *name, char *dev, size_t dev_size) {
	FILE *f = fopen("/proc/mtd", "r");
	if (!f) {
		return -1;
	}
	// dev:    size   erasesize  name
	// mtd5: 00080000 00020000 "ota"
	char line[128], want[64];
	snprintf(want, sizeof(want), "\"%s\"", name);
	int found = -1;
	while (fgets(line, sizeof(line), f)) {
		int n;
		char *q = strchr(line, '"');
		if (q && sscanf(line, "mtd%d:", &n) == 1 && strncmp(q, want, strlen(want)) == 0) {
			snprintf(dev, dev_size, "/dev/mtd%d", n);
			found = n;
			break;
		}
	}
	fclose(f);
	return found;
}

// Size of /dev/mtdN, from /proc/mtd.
long long mtd_part_size(int num) {
	FILE *f = fopen("/proc/mtd", "r");
	if (!f) {
		return -1;
	}
	char line[128];
	long long size = -1;
	while (fgets(line, sizeof(line), f)) {
		int n;
		unsigned long long s;
		if (sscanf(line, "mtd%d: %llx", &n, &s) == 2 && n == num) {
			size = (long long)s;
			break;
		}
	}
	fclose(f);
	return size;
}

boot_target_t bootflag_read(void) {
	char dev[32];
	if (mtd_find("ota", dev, sizeof(dev)) < 0) {
		LOG("bootflag: no \"ota\" partition in /proc/mtd");
		return BOOT_UNKNOWN;
	}
	int fd = open(dev, O_RDONLY);
	if (fd < 0) {
		LOG("bootflag: cannot open %s", dev);
		return BOOT_UNKNOWN;
	}
	char buf[257] = {0};
	ssize_t n = read(fd, buf, 256);
	close(fd);
	if (n < 11) {
		return BOOT_UNKNOWN;
	}
	// "ota:kernel2" first: "ota:kernel" is its prefix.
	if (strncmp(buf, "ota:kernel2", 11) == 0) {
		return BOOT_RECOVERY;
	}
	if (strncmp(buf, "ota:kernel", 10) == 0) {
		return BOOT_NORMAL;
	}
	return BOOT_UNKNOWN;
}

// Same two commands as /usr/bin/bootmode.sh and ota_utils.sh's
// mtd_write_str, on the partition found by name rather than a fixed mtd5.
static int write_record(const char *dev, const char *record) {
	if (run("flash_erase %s 0 1", dev) != 0) {
		return -1;
	}
	return run("printf '%%-256s' '%s' | nandwrite -s 0 -p %s -", record, dev) == 0 ? 0 : -1;
}

int bootflag_write(boot_target_t target) {
	char dev[32];
	if (target == BOOT_UNKNOWN || mtd_find("ota", dev, sizeof(dev)) < 0) {
		return -1;
	}
	int rc = write_record(dev, target == BOOT_RECOVERY ? "ota:kernel2" : "ota:kernel");
	boot_target_t now = bootflag_read();
	LOG("bootflag: now %s", boot_target_name(now));
	return (rc == 0 && now == target) ? 0 : -1;
}

int bootflag_request_fastboot(void) {
	char dev[32];
	if (mtd_find("ota", dev, sizeof(dev)) < 0) {
		return -1;
	}
	// Page 0 back to the normal system, so that whatever fastboot does, the
	// next plain boot is the system; then the request on page 1 of the same,
	// freshly erased block.
	if (write_record(dev, "ota:kernel") != 0) {
		return -1;
	}
	return run("printf '%%-256s' 'ota:fastboot' | nandwrite -s 2048 -p %s -", dev) == 0 ? 0 : -1;
}

static int read_int_file(const char *path, int *out) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return -1;
	}
	int ok = fscanf(f, "%d", out) == 1;
	fclose(f);
	return ok ? 0 : -1;
}

static int read_str_file(const char *path, char *out, size_t size) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return -1;
	}
	char *r = fgets(out, (int)size, f);
	fclose(f);
	if (!r) {
		return -1;
	}
	out[strcspn(out, "\r\n")] = '\0';
	return 0;
}

#ifndef POWER_SUPPLY_DIR // overridable for testing on a PC against a made-up tree
#define POWER_SUPPLY_DIR "/sys/class/power_supply"
#endif

// A battery's level: the capacity node, or POWER_SUPPLY_CAPACITY= in uevent
// for a gauge that publishes it only there (the stock player reads both).
// -1 when it has none.
static int supply_capacity(const char *name) {
	char path[300];
	int v;
	snprintf(path, sizeof(path), POWER_SUPPLY_DIR "/%s/capacity", name);
	if (read_int_file(path, &v) == 0) {
		return v < 0 ? 0 : v > 100 ? 100 : v;
	}
	snprintf(path, sizeof(path), POWER_SUPPLY_DIR "/%s/uevent", name);
	FILE *f = fopen(path, "r");
	if (!f) {
		return -1;
	}
	char line[128];
	v = -1;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "POWER_SUPPLY_CAPACITY=", 22) == 0) {
			v = atoi(line + 22);
			v = v < 0 ? 0 : v > 100 ? 100 : v;
			break;
		}
	}
	fclose(f);
	return v;
}

// Every supply, once, into the log: which node is the gauge differs between
// kernels (stock and Sonix), and a level of 0 has to be told apart from a
// node that only ever says 0.
static void log_supplies(void) {
	DIR *d = opendir(POWER_SUPPLY_DIR);
	if (!d) {
		LOG("battery: no " POWER_SUPPLY_DIR);
		return;
	}
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char path[300], type[32] = "?", status[32] = "-";
		int online = -1, uv = -1;
		snprintf(path, sizeof(path), POWER_SUPPLY_DIR "/%s/type", e->d_name);
		read_str_file(path, type, sizeof(type));
		snprintf(path, sizeof(path), POWER_SUPPLY_DIR "/%s/status", e->d_name);
		read_str_file(path, status, sizeof(status));
		snprintf(path, sizeof(path), POWER_SUPPLY_DIR "/%s/online", e->d_name);
		read_int_file(path, &online);
		snprintf(path, sizeof(path), POWER_SUPPLY_DIR "/%s/voltage_now", e->d_name);
		read_int_file(path, &uv);
		LOG("battery: %-18s type=%-8s capacity=%d status=%s online=%d voltage_now=%d", e->d_name, type,
			supply_capacity(e->d_name), status, online, uv);
	}
	closedir(d);
}

// Chosen as the player does it (system.c): the first battery reporting a
// level above 0, a 0 only when none does -- some kernels have a second
// battery node that answers 0 to everything. A 0 from a battery whose own
// voltage says otherwise (over 3.6 V) is taken as no reading at all: a gauge
// that has not settled, or the wrong node, must not block an update.
void battery_read(battery_t *out) {
	static bool logged;
	if (!logged) {
		logged = true;
		log_supplies();
	}
	out->capacity = -1;
	out->charging = false;
	DIR *d = opendir(POWER_SUPPLY_DIR);
	if (!d) {
		return;
	}
	struct dirent *e;
	char path[300], buf[32], zero_from[64] = "";
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') {
			continue;
		}
		snprintf(path, sizeof(path), POWER_SUPPLY_DIR "/%s/type", e->d_name);
		if (read_str_file(path, buf, sizeof(buf)) < 0) {
			continue;
		}
		if (strcmp(buf, "Battery") == 0) {
			int v = supply_capacity(e->d_name);
			if (v > 0 && out->capacity <= 0) {
				out->capacity = v;
			} else if (v == 0 && out->capacity < 0) {
				out->capacity = 0;
				snprintf(zero_from, sizeof(zero_from), "%s", e->d_name);
			}
			snprintf(path, sizeof(path), POWER_SUPPLY_DIR "/%s/status", e->d_name);
			if (read_str_file(path, buf, sizeof(buf)) == 0 &&
				(strcmp(buf, "Charging") == 0 || strcmp(buf, "Full") == 0)) {
				out->charging = true;
			}
		} else {
			int online;
			snprintf(path, sizeof(path), POWER_SUPPLY_DIR "/%s/online", e->d_name);
			if (read_int_file(path, &online) == 0 && online) {
				out->charging = true;
			}
		}
	}
	closedir(d);
	if (out->capacity == 0 && zero_from[0]) {
		int uv;
		snprintf(path, sizeof(path), POWER_SUPPLY_DIR "/%s/voltage_now", zero_from);
		if (read_int_file(path, &uv) == 0 && uv > 3600000) {
			static bool said;
			if (!said) {
				said = true;
				LOG("battery: %s says 0%% at %d uV: not believed, level unknown", zero_from, uv);
			}
			out->capacity = -1;
		}
	}
}

void device_reboot(void) {
	LOG("rebooting");
	sync();
	reboot(RB_AUTOBOOT);
}

void device_poweroff(void) {
	LOG("powering off");
	sync();
	reboot(RB_POWER_OFF);
}

int device_factory_reset(bool keep_media_db) {
	// The note S39_recovery.recovery acts on at boot would only run this
	// again on the next start; it goes with the rest of /data.
	int rc = run(keep_media_db ? "/usr/bin/recovery_all.sh 1" : "/usr/bin/recovery_all.sh 0");
	sync();
	return rc;
}

int mtd_list(mtd_part_t *out, int max) {
	FILE *f = fopen("/proc/mtd", "r");
	if (!f) {
		return -1;
	}
	char line[128];
	int n = 0;
	while (n < max && fgets(line, sizeof(line), f)) {
		int num;
		unsigned long long size;
		unsigned erasesize;
		char name[64];
		if (sscanf(line, "mtd%d: %llx %x \"%63[^\"]\"", &num, &size, &erasesize, name) == 4) {
			out[n].num = num;
			snprintf(out[n].name, sizeof(out[n].name), "%s", name);
			out[n].size = (long long)size;
			out[n].erasesize = erasesize;
			n++;
		}
	}
	fclose(f);
	return n;
}

int mtd_open_ro(int num) {
	char dev[32];
	snprintf(dev, sizeof(dev), "/dev/mtd%d", num);
	return open(dev, O_RDONLY);
}

int mtd_block_bad(int fd, long long off) {
	loff_t o = off;
	int r = ioctl(fd, MEMGETBADBLOCK, &o);
	return r < 0 ? -1 : r > 0;
}

long long mtd_ecc_failed(int fd) {
	struct mtd_ecc_stats st;
	return ioctl(fd, ECCGETSTATS, &st) == 0 ? (long long)st.failed : -1;
}

void theme_marker_read(char *out, size_t size) {
	char dev[32];
	out[0] = '\0';
	int num = mtd_find("ota", dev, sizeof(dev));
	int fd = num >= 0 ? open(dev, O_RDONLY) : -1;
	if (fd < 0) {
		return;
	}
	size_t want = size - 1 < 13 ? size - 1 : 13;
	ssize_t n = pread(fd, out, want, 0x20000);
	close(fd);
	out[n > 0 ? n : 0] = '\0';
	for (char *p = out; *p; p++) {
		if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e) { // erased flash, 0xff
			*p = '\0';
			break;
		}
	}
}

int mtd_open_rw(int num) {
	char dev[32];
	snprintf(dev, sizeof(dev), "/dev/mtd%d", num);
	return open(dev, O_RDWR);
}

int mtd_erase_block(int fd, long long off, unsigned size) {
	struct erase_info_user ei = {.start = (uint32_t)off, .length = size};
	return ioctl(fd, MEMERASE, &ei) == 0 ? 0 : -1;
}

unsigned mtd_writesize(int fd) {
	struct mtd_info_user info;
	return ioctl(fd, MEMGETINFO, &info) == 0 ? info.writesize : 0;
}

int userdata_release(int num) {
	sync();
	FILE *f = fopen("/proc/mounts", "r");
	bool mounted = false;
	if (f) {
		char dev[128], dir[256];
		while (fscanf(f, "%127s %255s %*s %*s %*d %*d", dev, dir) == 2) {
			if (strcmp(dir, "/usr/data") == 0) {
				mounted = true;
			}
		}
		fclose(f);
	}
	if (mounted && umount("/usr/data") != 0) {
		LOG("userdata: cannot unmount /usr/data: %s", strerror(errno));
		return -1;
	}
	// detached by mtd number; "not attached" is fine too
	run("ubidetach -m %d", num);
	return 0;
}

static int uboot_dev(char *dev, size_t size) { return mtd_find("uboot", dev, size) == 0 ? 0 : -1; }

int uboot_part_read(uint8_t *buf, size_t len) {
	char dev[32];
	if (uboot_dev(dev, sizeof(dev)) != 0) {
		return -1;
	}
	int fd = open(dev, O_RDONLY);
	if (fd < 0) {
		return -1;
	}
	size_t got = 0;
	while (got < len) {
		ssize_t n = read(fd, buf + got, len - got);
		if (n <= 0) {
			break;
		}
		got += (size_t)n;
	}
	close(fd);
	return got == len ? 0 : -1;
}

int uboot_part_write(const char *file) {
	char dev[32], q[600];
	if (uboot_dev(dev, sizeof(dev)) != 0) {
		return -1;
	}
	if (run("flash_erase %s 0 0", dev) != 0) {
		return -1;
	}
	return run("nandwrite -p %s %s", dev, shq(file, q, sizeof(q))) == 0 ? 0 : -1;
}

int uboot_part_bad_blocks(void) {
	char dev[32];
	if (uboot_dev(dev, sizeof(dev)) != 0) {
		return -1;
	}
	int fd = open(dev, O_RDONLY);
	if (fd < 0) {
		return -1;
	}
	struct mtd_info_user info;
	int bad = -1;
	if (ioctl(fd, MEMGETINFO, &info) == 0 && info.erasesize > 0) {
		bad = 0;
		for (uint32_t off = 0; off < info.size; off += info.erasesize) {
			loff_t o = off;
			int r = ioctl(fd, MEMGETBADBLOCK, &o);
			if (r < 0) {
				bad = -1;
				break;
			}
			bad += r > 0;
		}
	}
	close(fd);
	return bad;
}

const char *bootloader_dir(void) { return BOOTLOADER_DIR; }

#define SQ_MOUNT "/tmp/sqcheck"

int squashfs_read_file(const char *image, const char *path, char *out, size_t size) {
	char q[600];
	mkdir(SQ_MOUNT, 0755);
	if (run("mount -o loop,ro -t squashfs %s " SQ_MOUNT, shq(image, q, sizeof(q))) != 0) {
		return -1;
	}
	char full[300];
	snprintf(full, sizeof(full), SQ_MOUNT "/%s", path);
	int n = -1;
	FILE *f = fopen(full, "r");
	if (f) {
		n = (int)fread(out, 1, size - 1, f);
		out[n] = '\0';
		fclose(f);
	}
	if (umount(SQ_MOUNT) != 0) {
		LOG("squashfs: umount " SQ_MOUNT " failed");
	}
	return n;
}

int device_start_adb(void) { return run("/etc/init.d/T90adb start"); }
