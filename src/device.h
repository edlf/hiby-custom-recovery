#ifndef DEVICE_H
#define DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The boot selector. The "ota" MTD partition's first page holds HiBy's
// record, "ota:kernel" (normal system) or "ota:kernel2" (this recovery),
// padded with spaces to 256 bytes. The SPL reads it at every power-on.
// With Zee's bootloader, page 1 (offset 2048) starting with "ota:fastboot"
// starts fastboot instead, once.
typedef enum {
	BOOT_UNKNOWN = 0,
	BOOT_NORMAL,   // ota:kernel
	BOOT_RECOVERY, // ota:kernel2
} boot_target_t;

boot_target_t bootflag_read(void);
// Rewrites the record. Returns 0 on success.
int bootflag_write(boot_target_t target);
// Record "ota:kernel" plus the fastboot request on page 1.
int bootflag_request_fastboot(void);
const char *boot_target_name(boot_target_t t);

// /dev/mtdN of the partition called `name` (the first one with that name:
// the stock table calls both root fs "rootfs"). -1 when there is none.
int mtd_find(const char *name, char *dev, size_t dev_size);
// Size in bytes of /dev/mtd<num>, -1 when unknown.
long long mtd_part_size(int num);

// Battery, from /sys/class/power_supply. capacity is -1 when unknown.
typedef struct {
	int capacity;
	bool charging; // charging, full, or on external power
} battery_t;
void battery_read(battery_t *out);

// sync(), then reboot / power off. Do not return on the device.
void device_reboot(void);
void device_poweroff(void);

// /usr/bin/recovery_all.sh: wipes /data (all of it, or all but
// usrlocal_media.db when keep_media_db). Returns its exit status.
int device_factory_reset(bool keep_media_db);

// Starts adbd through the stock (T90, so not started at boot) init script.
int device_start_adb(void);

// The red and blue LEDs (/sys/class/leds/red, blue: the R1's, from
// leds_pwm_add) breathing together, so the player glows purple while the
// recovery runs instead of the stock red. Both are taken off their trigger
// first and then put on "breathing" one right after the other: each trigger
// starts its breath when it is set, so they stay in step. An LED that is not
// there is left out.
void device_leds_purple(void);

// All MTD partitions, as /proc/mtd lists them, for the backup. Returns
// how many, -1 when /proc/mtd cannot be read.
typedef struct {
	int num; // /dev/mtd<num>
	char name[32];
	long long size;
	unsigned erasesize;
} mtd_part_t;
int mtd_list(mtd_part_t *out, int max);
// Read-only file descriptor of /dev/mtd<num>, for mtd_block_bad(). The data
// itself is read with nanddump (mtd_dump_*) and written with nandwrite
// (mtd_image_*), below.
int mtd_open_ro(int num);
// Whether the erase block at `off` is marked bad: 1, 0, -1 when unknown.
int mtd_block_bad(int fd, long long off);

// Data written into partition `num` with the mtd-utils, as HiBy's updater
// wrote its images: begin erases the whole partition (flash_erase, which
// skips bad blocks), write streams the data into `nandwrite -p
// --input-size=SIZE` (bad blocks skipped: the data meant for one goes into
// the next good block; the last page padded), end waits for nandwrite: 0
// when it took all `size` bytes. end also frees the writer, after a failed
// write too. Nothing is read back here.
//   MTD_IMAGE_MARKBAD  -m: a block whose write fails is marked bad and its
//                      data written into the next one (the update: an image
//                      only has to be whole, not at a given offset)
//   MTD_IMAGE_SKIP_FF  -k: pages that are all 0xff are left erased (the
//                      restore: UBI wants empty pages empty)
// The simulator writes into $SIM_DIR/mtdN the same way.
#define MTD_IMAGE_MARKBAD 1u
#define MTD_IMAGE_SKIP_FF 2u
typedef struct mtd_image_writer mtd_image_writer_t;
mtd_image_writer_t *mtd_image_begin(int num, long long size, unsigned flags);
int mtd_image_write(mtd_image_writer_t *w, const void *buf, size_t len);
int mtd_image_end(mtd_image_writer_t *w);

// The first `length` bytes of partition `num` (all of it when length <= 0),
// read with `nanddump --bb=padbad`: ECC corrected, a bad block given as 0xff
// in its place, so every block is at its offset. An uncorrectable ECC error
// does not stop it (the data is given as read). read: exactly len bytes, 0 on
// success. end waits for nanddump (0 when it gave everything and exited 0),
// fills in what it reported, and frees the reader, after a failure too.
typedef struct {
	int bad_blocks;			// "Bad block at ..." lines
	long long ecc_failed;	// uncorrectable ECC errors met
} mtd_dump_stats_t;
typedef struct mtd_dump mtd_dump_t;
mtd_dump_t *mtd_dump_begin(int num, long long length);
// The same from offset `start` (an erase block boundary) for `length` bytes:
// for going on past a block nanddump could not read at all (backup.c).
mtd_dump_t *mtd_dump_begin_at(int num, long long start, long long length);
int mtd_dump_read(mtd_dump_t *d, void *buf, size_t len);
int mtd_dump_end(mtd_dump_t *d, mtd_dump_stats_t *st);

// userdata before it is written: /usr/data unmounted and its UBI device
// detached (it is not attached again; a reboot does that). 0 on success.
int userdata_release(int num);

// The "uboot" partition, for bootloader.c. read: the first len bytes (ECC
// corrected). write: erases the whole partition and writes the file from
// its start. bad_blocks: how many of its erase blocks are marked bad, -1
// when that cannot be told. bootloader_dir: where the bundled image is
// (BOOTLOADER_DIR on the device).
int uboot_part_read(uint8_t *buf, size_t len);
int uboot_part_write(const char *file);
int uboot_part_bad_blocks(void);
const char *bootloader_dir(void);


// `s` in single quotes for a shell command line, any ' in it escaped. Returns
// out, or "''" when it does not fit.
const char *shq(const char *s, char *out, size_t size);

// system() with the command logged and its exit status returned (-1 when it
// did not run).
int run(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* DEVICE_H */
