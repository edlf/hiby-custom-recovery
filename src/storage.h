#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stddef.h>

// The microSD card. Nothing in the recovery mounts it at boot (mdev only
// handles USB sd[a-z], and sys_server mounts on request from the player), so
// it is mounted here, at SD_MOUNT.
//
// Always read-only: the card is only written while a backup is made (backup.h,
// and the copy of the old bootloader before "Install fastboot"), and those
// turn it writable with storage_set_writable() for as long as they write,
// then back.

#define SD_MOUNT "/tmp/mnt/sd"

// Whether a card is in: a /dev/mmcblk* node exists. Cheap, no mount.
bool storage_card_present(void);

// The card's capacity in bytes, from /sys/block/mmcblk*/size (the whole
// card, not the file system); -1 when there is no card or it cannot be read.
// Cheap, no mount.
long long storage_card_size(void);

// Mounts the card if it is not mounted yet, waiting up to timeout_s for it
// to appear. Fills `mnt` with where it is. 0 on success. Read-only: a card
// already mounted elsewhere (read-write) is remounted read-only.
int storage_mount(int timeout_s, char *mnt, size_t mnt_size);

// Remounts the mounted card read-write (true) or read-only (false). 0 on
// success. Going back to read-only syncs first; files open for writing must
// be closed by then.
int storage_set_writable(bool rw);

void storage_unmount(void);

// USB mass storage (device.h): the card goes to the PC. storage_export()
// unmounts it wherever it is mounted (0 when nothing is left mounted);
// while exported storage_mount() refuses, so nothing here writes to a card
// the PC is writing to. storage_card_disk() gives its whole-disk node
// (/dev/mmcblk0), NULL without a card.
int storage_export(void);
void storage_unexport(void);
bool storage_exported(void);
const char *storage_card_disk(void);

#endif /* STORAGE_H */
