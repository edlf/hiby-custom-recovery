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

bool storage_card_present(void) {
	for (size_t i = 0; i < sizeof(CANDIDATES) / sizeof(CANDIDATES[0]); i++) {
		if (access(CANDIDATES[i], F_OK) == 0) {
			return true;
		}
	}
	return false;
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
	if (find_existing(mnt, mnt_size) == 0) {
		LOG("storage: card already mounted at %s", mnt);
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
				if (mount(CANDIDATES[i], SD_MOUNT, FSTYPES[t], MS_NOATIME, NULL) == 0) {
					LOG("storage: %s (%s) mounted at %s", CANDIDATES[i], FSTYPES[t], SD_MOUNT);
					mounted_here = true;
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

void storage_unmount(void) {
	sync();
	if (mounted_here) {
		if (umount(SD_MOUNT) == 0) {
			mounted_here = false;
		} else {
			LOG("storage: umount %s: %s", SD_MOUNT, strerror(errno));
		}
	}
}
