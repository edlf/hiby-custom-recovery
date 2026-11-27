#include "storage.h"

#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

// Partition first, whole disk second (a card formatted without a partition
// table), card slot 0 before 1.
static const char *const CANDIDATES[] = {
	"/dev/mmcblk0p1", "/dev/mmcblk0", "/dev/mmcblk1p1", "/dev/mmcblk1",
};
static const char *const FSTYPES[] = {"vfat", "exfat"};

static bool mounted_here;
static bool exported; // on USB mass storage: the PC's
static char cur_mnt[256]; // where the card is mounted, "" when it is not

static int remount(const char *dir, bool rw) {
	if (!rw) {
		sync();
	}
	unsigned long flags = MS_REMOUNT | MS_NOATIME | (rw ? 0 : MS_RDONLY);
	if (mount(NULL, dir, NULL, flags, NULL) != 0) {
		LOG("storage: remount %s %s: %s", dir, rw ? "rw" : "ro", strerror(errno));
		return -1;
	}
	LOG("storage: %s %s", dir, rw ? "writable" : "read-only");
	return 0;
}

bool storage_card_present(void) {
	for (size_t i = 0; i < sizeof(CANDIDATES) / sizeof(CANDIDATES[0]); i++) {
		if (access(CANDIDATES[i], F_OK) == 0) {
			return true;
		}
	}
	return false;
}

long long storage_card_size(void) {
	static const char *const DISKS[] = {"mmcblk0", "mmcblk1"};
	for (size_t i = 0; i < sizeof(DISKS) / sizeof(DISKS[0]); i++) {
		char p[64];
		snprintf(p, sizeof(p), "/dev/%s", DISKS[i]);
		if (access(p, F_OK) != 0) {
			continue;
		}
		snprintf(p, sizeof(p), "/sys/block/%s/size", DISKS[i]);
		FILE *f = fopen(p, "r");
		if (!f) {
			continue;
		}
		long long sectors = -1;
		int ok = fscanf(f, "%lld", &sectors) == 1;
		fclose(f);
		if (ok && sectors > 0) {
			return sectors * 512; // always 512-byte units in sysfs
		}
	}
	return -1;
}

// Where a /dev/mmcblk* is already mounted, if it is.
static int find_existing(char *mnt, size_t mnt_size) {
	FILE *f = fopen("/proc/mounts", "r");
	if (!f) {
		return -1;
	}
	char dev[128], dir[256];
	int rc = -1;
	while (fscanf(f, "%127s %255s %*s %*s %*d %*d", dev, dir) == 2) {
		if (strncmp(dev, "/dev/mmcblk", 11) == 0) {
			snprintf(mnt, mnt_size, "%s", dir);
			rc = 0;
			break;
		}
	}
	fclose(f);
	return rc;
}

int storage_mount(int timeout_s, char *mnt, size_t mnt_size) {
	if (exported) {
		LOG("storage: the card is on USB, not mounted here");
		return -1;
	}
	if (find_existing(mnt, mnt_size) == 0) {
		LOG("storage: card already mounted at %s", mnt);
		snprintf(cur_mnt, sizeof(cur_mnt), "%s", mnt);
		remount(mnt, false); // whoever mounted it, read-only from here on
		return 0;
	}
	mkdir("/tmp/mnt", 0755);
	mkdir(SD_MOUNT, 0755);

	// The wait is only for the card's node to appear (the MMC driver probes
	// it a moment after boot); once there is one and nothing on it mounts,
	// waiting longer changes nothing.
	for (int waited = 0; waited <= timeout_s * 10; waited++) {
		bool any = false;
		for (size_t i = 0; i < sizeof(CANDIDATES) / sizeof(CANDIDATES[0]); i++) {
			if (access(CANDIDATES[i], F_OK) != 0) {
				continue;
			}
			any = true;
			for (size_t t = 0; t < sizeof(FSTYPES) / sizeof(FSTYPES[0]); t++) {
				if (mount(CANDIDATES[i], SD_MOUNT, FSTYPES[t], MS_NOATIME | MS_RDONLY, NULL) == 0) {
					LOG("storage: %s (%s) mounted read-only at %s", CANDIDATES[i], FSTYPES[t], SD_MOUNT);
					mounted_here = true;
					snprintf(cur_mnt, sizeof(cur_mnt), "%s", SD_MOUNT);
					snprintf(mnt, mnt_size, "%s", SD_MOUNT);
					return 0;
				}
			}
			LOG("storage: %s: cannot mount as vfat or exfat (%s)", CANDIDATES[i], strerror(errno));
		}
		if (any) {
			break;
		}
		usleep(100 * 1000);
	}
	LOG("storage: no card could be mounted");
	return -1;
}

int storage_set_writable(bool rw) {
	if (!cur_mnt[0]) {
		LOG("storage: no card mounted to make %s", rw ? "writable" : "read-only");
		return -1;
	}
	return remount(cur_mnt, rw);
}

void storage_unmount(void) {
	sync();
	if (cur_mnt[0]) {
		remount(cur_mnt, false); // a card left mounted (not ours) stays read-only
	}
	if (mounted_here) {
		if (umount(SD_MOUNT) == 0) {
			mounted_here = false;
		} else {
			LOG("storage: umount %s: %s", SD_MOUNT, strerror(errno));
		}
	}
	if (!mounted_here) {
		cur_mnt[0] = '\0';
	}
}

const char *storage_card_disk(void) {
	static const char *const DISKS[] = {"/dev/mmcblk0", "/dev/mmcblk1"};
	for (size_t i = 0; i < sizeof(DISKS) / sizeof(DISKS[0]); i++) {
		if (access(DISKS[i], F_OK) == 0) {
			return DISKS[i];
		}
	}
	return NULL;
}

int storage_export(void) {
	sync();
	char mnt[256];
	for (int i = 0; i < 4 && find_existing(mnt, sizeof(mnt)) == 0; i++) {
		if (umount(mnt) != 0) {
			LOG("storage: umount %s: %s", mnt, strerror(errno));
			return -1;
		}
		LOG("storage: %s unmounted for USB", mnt);
	}
	if (find_existing(mnt, sizeof(mnt)) == 0) {
		return -1;
	}
	mounted_here = false;
	cur_mnt[0] = '\0';
	exported = true;
	return 0;
}

void storage_unexport(void) { exported = false; }

bool storage_exported(void) { return exported; }
