#ifndef RESTORE_H
#define RESTORE_H

#include <stdbool.h>
#include <stdint.h>

// Writes partitions back from a backup made by backup.h (a zees-backup-*
// folder in the top of the card).
//
// Not offered: kernel2 and the second rootfs, the recovery that is running
// (its own programs would be erased under it). Those go back with fastboot.
//
// Before anything is written:
//   - info.txt must name this player ("HiBy R1, ...")
//   - mtd.txt must be the current partition table, line for line
//   - each chosen file: its md5 as md5sums.txt has it, the partition's size
//   - each chosen partition: a block gone bad since the backup that held
//     data in it refuses the partition (the data would land elsewhere)
//   - userdata: /usr/data unmounted and its UBI detached
// Then each partition is written with the mtd-utils, at the offsets the
// backup read it from: flash_erase, then nandwrite -k (pages that are all 0xff
// left erased, as UBI wants them) fed the backup without the blocks that are
// bad now, which nandwrite skips. Then it is read back with nanddump and
// compared block by block. The bootloader's current contents (read the same
// way) are kept in RAM and written back if its read back fails; the others
// are written once more.

#define RESTORE_MAX_PARTS 16

typedef struct {
	char dir[512];	// full path
	char name[128]; // folder name
	char model[96]; // from info.txt, "" when unreadable
	bool same_model;
	int n_files;
	long long bytes;
} backup_entry_t;

// The card's zees-backup-* folders, newest name first. -1 without a card.
int restore_list_backups(backup_entry_t *out, int max);

typedef struct {
	int num;
	char name[32];
	char file[64];
	long long size;
	bool restorable;
	char why_not[96]; // when not restorable
	bool default_on;  // kernel and system
} restore_part_t;

// What a backup holds, with what may be restored. -1 when it is unreadable.
int restore_scan(const char *dir, restore_part_t *out, int max);

typedef enum {
	RESTORE_IDLE = 0,
	RESTORE_RUNNING,
	RESTORE_DONE,
	RESTORE_FAILED,	  // nothing written, or what failed is said
	RESTORE_FAILED_BL // the bootloader matches neither the backup nor its old self
} restore_state_t;

typedef struct {
	restore_state_t state;
	char phase[96];
	int percent;
	char message[320];
	bool wrote; // something was written: a reboot is needed
} restore_status_t;

// `mask`: bit N set restores mtdN.
int restore_start(const char *dir, uint32_t mask);
void restore_get(restore_status_t *out);

#endif /* RESTORE_H */
