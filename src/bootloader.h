#ifndef BOOTLOADER_H
#define BOOTLOADER_H

#include <stdbool.h>

// The "uboot" partition (mtd0): the SPL the boot ROM loads, and U-Boot.
//
// Sonix's (uboot/ in the tree) is told from HiBy's by the string
// "ota:fastboot", the fastboot request its SPL and U-Boot both look for
// (device.h); the stock loader has no fastboot at all. Without it, "Reboot to
// fastboot" would just start the system, so the menu offers it only with
// the Sonix loader, and otherwise offers to install it (on the models where
// that is allowed, model.h) from the copy pack_recovery.sh puts in the
// recovery root fs at BOOTLOADER_DIR/<model's image>, with its .md5 beside.
//
// The install, on a worker thread:
//   1. the bundled image: md5 as recorded at packing time, SPL header magic,
//      fits the partition, its partition table area blank
//   2. the partition as it is: read whole; refused unless it has a partition
//      table ("nand" at 0x5800) and no bad block
//   3. the image gets the player's own table (0x5800-0x5bff) copied in, so
//      the table is never replaced by a guess
//   4. a copy of the old partition goes to the card (uboot-backup-N.bin)
//      when one can be mounted
//   5. erase, write, read back and compare; on a mismatch written once more;
//      if that fails too the old bytes are written back and verified
//
// The boot ROM has no fallback: a partition without a working SPL can only
// be fixed with Ingenic's USB boot mode, hence the checks and step 5.

#define BOOTLOADER_DIR "/usr/share/sonix-recovery"

// Reads the partition (once; again after an install).
bool bootloader_is_sonix(void);
// The bundled image is there and this model may install it.
bool bootloader_can_install(void);

typedef enum {
	BL_IDLE = 0,
	BL_RUNNING,
	BL_DONE,
	BL_FAILED,	   // nothing changed, or the old loader was put back and verified
	BL_FAILED_BAD, // the partition could not be made to match either: do not power off
} bl_state_t;

typedef struct {
	bl_state_t state;
	char phase[64];
	char message[256];
} bl_status_t;

int bootloader_install_start(void);
void bootloader_get(bl_status_t *out);

#endif /* BOOTLOADER_H */
