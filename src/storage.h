#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stddef.h>

// The microSD card. Nothing in the recovery mounts it at boot (mdev only
// handles USB sd[a-z], and sys_server mounts on request from the player), so
// it is mounted here, at SD_MOUNT, read-write so the log can be left on it.

#define SD_MOUNT "/tmp/mnt/sd"

// Whether a card is in: a /dev/mmcblk* node exists. Cheap, no mount.
bool storage_card_present(void);

// Mounts the card if it is not mounted yet, waiting up to timeout_s for it
// to appear. Fills `mnt` with where it is. 0 on success.
int storage_mount(int timeout_s, char *mnt, size_t mnt_size);

void storage_unmount(void);

#endif /* STORAGE_H */
