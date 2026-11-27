#ifndef PARTTABLE_H
#define PARTTABLE_H

#include <stddef.h>
#include <stdint.h>

// The burner partition table the SPL, U-Boot and the kernel read, in the
// "uboot" partition at 0x5800 (uboot/README.md, partitions/mkparttable.py):
// "nand", the number of entries (u32), then 48-byte entries of name[32],
// size, offset, mask_flags, manager_mode (u32 each, little endian); the rest
// of the 1 KiB is 0xff.
//
// Before Zee's bootloader is installed, the player's table is checked
// against the one built from partitions/<model>.txt, bundled beside the
// image: the same entries in the same order, with the same offsets, sizes,
// flags and manager. Names must match too, with one exception: the R1's
// firmware 2.0 wrote a table that calls the recovery root fs (entry 4)
// "rootfs" again, so "rootfs" appears twice; before 2.0 it was "rootfs2", as
// on the R3 Pro II. HiBy's OTA scripts look the recovery root fs up as
// "rootfs2" (/etc/ota_bin/ota_local_method.sh) and cannot find it in the 2.0
// table. That name alone is corrected; every other byte is kept.

#define PT_OFF 0x5800
#define PT_LEN 0x400

typedef enum {
	PT_SAME,	 // the player's table is the expected one
	PT_FIXED,	 // the same but for the duplicated "rootfs": out has it renamed
	PT_MISMATCH, // something else differs: why says what
} pt_result_t;

// Checks have (the player's table, PT_LEN bytes) against want (the expected
// one). out (PT_LEN bytes) gets the table to write: have as it is, or with
// the fix. why gets what differs or what was fixed.
pt_result_t pt_check(const uint8_t *have, const uint8_t *want, uint8_t *out, char *why, size_t why_len);

#endif /* PARTTABLE_H */
