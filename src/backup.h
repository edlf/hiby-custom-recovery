#ifndef BACKUP_H
#define BACKUP_H

#include <stdbool.h>

// Copies the flash partitions, raw, to a new folder on the card:
//
//   sonix-backup-20261125-1840/      (sonix-backup-N when the clock is unset)
//     mtd0-uboot.bin ... mtd6-userdata.bin   one per partition, its full size
//     mtd.txt         the partition table as /proc/mtd gave it
//     md5sums.txt     md5sum -c on a PC checks the copies
//     info.txt        model, recovery version, and per partition the bad
//                     blocks (written as 0xff) and uncorrectable ECC errors
//
// Each partition is read erase block by erase block (ECC corrected), its md5
// computed on the way, and the file on the card read back afterwards and
// checked against that md5. Bad blocks are not read, they are written as
// 0xff, as nanddump does. An uncorrectable ECC error does not stop the copy:
// the data is kept, and the partition is flagged in info.txt and on screen.
//
// userdata (UBI, /usr/data) is mounted while it is read; nothing in the
// recovery writes to it in the meantime.

typedef enum {
	BACKUP_IDLE = 0,
	BACKUP_RUNNING,
	BACKUP_DONE,
	BACKUP_FAILED,
} backup_state_t;

typedef struct {
	backup_state_t state;
	char phase[96];
	int percent;
	char message[320];
	bool warnings; // DONE, with bad blocks or ECC errors in some partition
} backup_status_t;

// Bytes the backup takes, with or without userdata.
long long backup_size(bool with_userdata);
int backup_start(bool with_userdata);
void backup_get(backup_status_t *out);

#endif /* BACKUP_H */
