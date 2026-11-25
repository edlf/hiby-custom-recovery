#ifndef DEVICE_H
#define DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The boot selector. The "ota" MTD partition's first page holds HiBy's
// record, "ota:kernel" (normal system) or "ota:kernel2" (this recovery),
// padded with spaces to 256 bytes. The SPL reads it at every power-on.
// With the Sonix U-Boot, page 1 (offset 2048) starting with "ota:fastboot"
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

// All MTD partitions, as /proc/mtd lists them, for the backup. Returns
// how many, -1 when /proc/mtd cannot be read.
typedef struct {
	int num; // /dev/mtd<num>
	char name[32];
	long long size;
	unsigned erasesize;
} mtd_part_t;
int mtd_list(mtd_part_t *out, int max);
// Read-only file descriptor of /dev/mtd<num>; read() gives ECC-corrected
// data.
int mtd_open_ro(int num);
// Whether the erase block at `off` is marked bad: 1, 0, -1 when unknown.
int mtd_block_bad(int fd, long long off);
// Uncorrectable ECC errors the partition has counted so far (ECCGETSTATS),
// -1 when unknown. Read before and after, the difference is what a read hit.
long long mtd_ecc_failed(int fd);

// For restore.c: /dev/mtd<num> read-write, one erase block erased (0 on
// success), the page size (the smallest write; 0 when unknown). Writes are
// pwrite() of whole pages at page-aligned offsets.
int mtd_open_rw(int num);
int mtd_erase_block(int fd, long long off, unsigned size);
unsigned mtd_writesize(int fd);
// userdata before it is written: /usr/data unmounted and its UBI device
// detached (it is not attached again; a reboot does that). 0 on success.
int userdata_release(int num);

// The player's boot logo marker, 13 bytes at 0x20000 of "ota"
// ("theme:T,aA,cC", style.h), NUL-terminated into out; "" when unreadable.
void theme_marker_read(char *out, size_t size);

// The "uboot" partition, for bootloader.c. read: the first len bytes (ECC
// corrected). write: erases the whole partition and writes the file from
// its start. bad_blocks: how many of its erase blocks are marked bad, -1
// when that cannot be told. bootloader_dir: where the bundled image is
// (BOOTLOADER_DIR on the device).
int uboot_part_read(uint8_t *buf, size_t len);
int uboot_part_write(const char *file);
int uboot_part_bad_blocks(void);
const char *bootloader_dir(void);

// Reads `path` (relative, "usr/resource/hostname") out of the squashfs
// image file `image` into out (NUL-terminated). Bytes read, -1 when it
// cannot. The device loop-mounts it read-only; the simulator uses
// unsquashfs.
int squashfs_read_file(const char *image, const char *path, char *out, size_t size);

// `s` in single quotes for a shell command line, any ' in it escaped. Returns
// out, or "''" when it does not fit.
const char *shq(const char *s, char *out, size_t size);

// system() with the command logged and its exit status returned (-1 when it
// did not run).
int run(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* DEVICE_H */
