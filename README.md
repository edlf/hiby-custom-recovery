# Sonix Recovery

A replacement for HiBy's `/usr/bin/recoveryd`, the program the recovery
system (kernel2 + rootfs2) runs to install firmware from the microSD card.
It is a small LVGL app on the framebuffer, driven by the touch panel or the
side keys.

## How the recovery is entered

The `ota` MTD partition's first page holds the boot record, `ota:kernel`
(normal system) or `ota:kernel2` (recovery). The SPL boots kernel2/rootfs2
when the record says `ota:kernel2`, when Volume up is held at power-on, or
after `fastboot reboot recovery` (Sonix U-Boot). The player's firmware update
writes `ota:kernel2` and reboots.

The recovery root fs is the stock main root fs minus the music player.
BusyBox init runs `/etc/init.d/rcS`, which runs the `S??*` scripts: modules,
logo and backlight, `/usr/data` (UBI), the factory reset hook
(`S39_recovery.recovery`), `sys_server`, Wi-Fi/BT init, and last
`S92_03_start_recovery`, which used to start `recoveryd`. `pack_recovery.sh`
makes that script start `sonix_recovery` instead, and restarts it if it
exits.

## What it does

| Main menu | |
|---|---|
| Install r1.upt | `<model>.upt` (`r1.upt`, `r3proii.upt`, any case) from the card's top folder |
| Choose file on card | browse the card folder by folder; only folders and `.upt` files are shown (hidden ones skipped). Power goes up a folder |
| Reboot to system | sets the record to `ota:kernel`, reboots |
| Factory reset | `/usr/bin/recovery_all.sh`: all of `/data`, or all but `usrlocal_media.db` |
| Advanced | the menu below |
| Power off | |

| Advanced | |
|---|---|
| Back up partitions | every MTD partition (or all but user data), raw, to a new folder on the card; see below |
| Restore backup | partitions back from a `sonix-backup-*` folder; see below |
| Reboot to fastboot | `ota:kernel` + `ota:fastboot` on page 1; only shown with the Sonix bootloader |
| Install fastboot | instead of the above while HiBy's bootloader is installed (R1 only), see below |
| Start ADB | `/etc/init.d/T90adb start` |
| Back | to the main menu |

Back, Cancel and Power lead to the menu a screen was opened from.

Keys: Volume up / Volume down move, Play/Pause presses, Power goes back.
On the R1, Next also moves down.

### Look

The Sonix Player's (`src/gui/shell/theme.c`): GNOME Adwaita, cards with
12 px corners, MiSans through FreeType (`/usr/share/sonix-recovery/default.otf`,
bundled by `pack_recovery.sh`; the built-in Montserrat without it). Dark or
light and the accent colour follow what the player last chose, read from the
boot logo marker it keeps on the `ota` partition (`theme:T,aA,cC` at
0x20000); with no marker, dark and blue. The main menu's rows take what the
screen leaves (about 90 px on the R1, 80 on the R3 Pro II).

### Automatic update

When the boot record says `ota:kernel2` at start, the player asked for
this boot to install its update, and the recovery does that as the stock
one does: a 5 s countdown ("Install now" / "Cancel"; Play/Pause and Power
cancel), then the update, then a 5 s countdown to restarting into the
system ("Back" stays). Entered with Volume up the record still says
`ota:kernel`, and the menu comes up instead. A failed or cancelled update
leaves the record on `ota:kernel2`, so the next start offers it again;
"Reboot to system" sets it back.

### Backup

**Back up partitions** copies the flash, raw, to `sonix-backup-<date>-<time>/`
on the card (`sonix-backup-N/` when the clock is unset): `mtd0-uboot.bin` …
`mtd6-userdata.bin`, each at its partition's full size, plus `mtd.txt` (the
partition table), `md5sums.txt` and `info.txt`. Everything is 125 MB on the
R1, 80 MB without user data. It checks there is room first.

Each partition is read block by block, its md5 computed on the way; the
file on the card is then read back and must give the same md5. Bad blocks
are written as 0xff (as `nanddump` does); a block with uncorrectable ECC
errors keeps its data. Both are counted per partition in `info.txt` and
the result says which partitions had them. On a PC:

```sh
cd sonix-backup-*/ && md5sum -c md5sums.txt
```

**Restore backup** (Advanced) lists the `sonix-backup-*` folders in the top
of the card and the partitions in the chosen one, with checkboxes: kernel
and system ticked, bootloader, boot record and user data on request.
Kernel2 and the recovery's rootfs are not offered (the recovery is running
from them: use fastboot). Before anything is written:

- `info.txt` must name this player and `mtd.txt` must be its partition
  table, line for line
- each chosen file must have its md5 from `md5sums.txt` and the partition's
  size
- a block gone bad since the backup where the backup has data refuses that
  partition (the data would not land where it was)
- for user data, `/usr/data` is unmounted and its UBI detached (refused if
  it is in use)

Then each partition is erased and written block by block at the offsets it
was read from (bad blocks skipped, all-0xff pages left erased, as UBI wants
them), read back and compared; a partition that differs is written once
more. The bootloader's current contents are kept in RAM and written back if
its read back fails. A restart is needed afterwards; the result screen
offers "Reboot to system".

### The bootloader

The Sonix U-Boot is told from HiBy's by the string `ota:fastboot` in the
`uboot` partition (its SPL and U-Boot both look for that request; HiBy's
has no fastboot). With HiBy's, "Reboot to fastboot" would only start the
system, so the menu offers **Install fastboot** instead. It is not offered
on the R3 Pro II: its board has not been compared with its stock loader
yet (`uboot/README.md`).

`pack_recovery.sh` bundles `output/r1/u-boot-hiby_r1.bin`, the base image
with the partition table area blank, as
`/usr/share/sonix-recovery/u-boot-hiby_r1.bin` with its md5. The install:

1. checks the bundled image: md5, SPL header magic, size, blank table area
2. reads the whole `uboot` partition; refuses without a partition table
   (`nand` at 0x5800) or with a bad block
3. copies the player's own table (0x5800–0x5bff) into the image: the result
   is what `u-boot-hiby_r1-withparts.bin` is for a stock R1
4. saves the old partition on the card as `uboot-backup-N.bin`, if there
   is a card
5. erases, writes, reads back and compares; on a mismatch writes once more,
   then puts the old partition back and verifies it. If that fails too it
   says so and offers only "Try again" (working from the copy read before
   the first write, not from the flash).

The boot ROM has no fallback: a `uboot` partition without a working SPL
can only be fixed with Ingenic's USB boot mode.

### The update

1. Mounts the card (`/dev/mmcblk0p1` or the whole disk, vfat or exfat) at
   `/tmp/mnt/sd`, and loop-mounts the `.upt` (an ISO) at `/tmp/upt`.
2. Checks the package before anything is erased:
   - `ota_config.in`, `ota_vN/ota_vN.ok`
   - each image in `ota_update.in` is `kernel` or `rootfs` and fits its
     partition
   - the chunks join up to `img_size` bytes and `img_md5`
   - **which player it is for, from the system image.** The rootfs chunks
     are joined into `.sonix_rootfs_check.squashfs` on the card in the same
     pass as the md5 (about 40 MB, more than the RAM can spare), which is
     loop-mounted read-only. Its `/usr/resource/hostname` must be
     `HiBy R1` / `HiBy R3PROII` (true of the stock and Sonix packages
     alike). The file is then deleted. A package without a system image is
     refused.
   - `ota_vN/ota_compatibility.in` starts with `hiby_r1_` on the R1 (the
     R3 Pro II packages have none; if one is there it must match). This
     check alone can be overridden: when the system image is this player's
     and only the record is wrong or missing, the failure offers
     **Install anyway** (behind a confirmation). The automatic update never
     overrides it.
3. Sets the record to `ota:kernel2` if it is not already. The stock writer
   writes the kernel the record does *not* name, so if the recovery was
   entered with Volume up (record still `ota:kernel`) it would otherwise
   aim at the recovery's own partitions.
4. Runs the stock writer, `/etc/ota_bin/local_ota_update.sh /tmp/upt`,
   showing its `ota: data processed: N%` lines. It sets the record back to
   `ota:kernel` when it succeeds. If it fails the record stays on the
   recovery, so the player comes back here until an update succeeds.
5. Checks what was written: each image is read back from its partition
   the way the boot reads it (bad blocks skipped, which is also how
   `nandwrite` laid it out) and its md5 compared with `img_md5`. On a
   mismatch the record goes back to `ota:kernel2`, so the player starts in
   recovery instead of a broken system, and the update is reported as
   failed; otherwise it says "installed and checked".
6. Copies the log, `/tmp/sonix_recovery.log`, to the card as
   `sonix_recovery.log`.

An update is refused below 20% battery unless the player is charging.

## Building

One script does all of it, from the repository's top folder:

```sh
./build_recovery.sh                 # r1, with the Sonix kernel
./build_recovery.sh --no-build      # only the app and the image, from what output/ has
./build_recovery.sh --stock-kernel  # keep HiBy's kernel2: rootfs2 only
```

It writes `output/<model>/kernel2-recovery.xImage` (the Sonix kernel) and
`output/<model>/rootfs2-recovery.squashfs`. Its steps (`--help` has them
in full):

1. the stock recovery root fs, from a NAND dump (once, see below)
2. kernel and U-Boot: `./build_images.sh MODEL`
3. the Sonix firmware, `output/<model>/<model>.upt`, via `./pack_firmware.sh`
   when it is missing or was packed with another kernel (the kernel build
   makes a new `xImage` every time, so a full run repacks it, for both
   players)
4. the app: `make -C recovery MODEL=MODEL`
5. the image: `./pack_recovery.sh --kernel MODEL`

**The kernel.** The Sonix kernel has many drivers built in that HiBy's root
fs loads as modules (keys, backlight, GPIO, I2C…), and its modules are
built with it. So with it the recovery root fs takes `module_driver/`
(the `.ko` and their loader script) and `S43wifi_bcm_init_config` from the
Sonix firmware's root fs, which was packed with that same `xImage`
(`pack_recovery.sh` checks the md5). Flash the two together: these
modules with HiBy's kernel2, or HiBy's with this kernel, leave the
recovery without keys or a screen.

The app uses the player's cross toolchain and LVGL checkout
(`../sonix-player/sonix-player`); the script builds the player when
`pack_firmware.sh` needs it.

There is no stock R3 Pro II recovery yet (no NAND dump), so for now the
image can only be made for the R1. The app itself builds for both.

### The NAND dump

The stock recovery (kernel2 + rootfs2) is not in HiBy's `.upt` files; it
comes from the player's own flash. From a root shell (adb) on the stock
firmware:

```sh
adb shell cat /proc/mtd                            # mtd4 must be the second "rootfs"
mkdir -p ~/r1-mtd && adb exec-out cat /dev/mtd4 > ~/r1-mtd/mtd4
```

(`exec-out` streams straight to the PC; an adbd too old for it needs
`cat /dev/mtd4 > /mnt/sd_0/mtd4` on the player and the file copied off the
card, not `/tmp`, which is RAM.)

`build_recovery.sh` looks in `~/<model>-mtd/` (or `--dump DIR`) and only
needs `mtd4`, the second `rootfs`. Once Sonix Recovery is on the player,
**Back up partitions** writes the same files to the card.

## Simulator (PC)

The interface and the update checks run on the PC too, against a simulated
device: a folder stands in for the card and a file for the boot record.
Nothing outside that folder is touched. The package checks are the real
ones, and an update really writes the images into `$SIM_DIR/mtd1` and
`mtd2`, so the read back after it is real too.

```sh
make -C recovery host                  # needs libsdl2-dev for the window
mkdir -p recovery/build/sim/sd
ln -sf "$PWD/original_firmware/r1(v2.0).upt" recovery/build/sim/sd/r1.upt
SIM_DIR=recovery/build/sim recovery/build/host-r1/sonix_recovery_sim
```

Mouse = touch. Up/Down (or Tab) = Volume keys, Enter = Play/Pause,
Esc/Backspace = Power. Reboot and power off quit the simulator.

| | |
|---|---|
| `SIM_DIR` | the simulated device, default `./sim` (`recovery/build/sim` above stays out of git) |
| `$SIM_DIR/sd/` | the card; there when the folder exists |
| `$SIM_DIR/ota` | the boot record (`ota:kernel2` at first) |
| `$SIM_DIR/data/` | what a factory reset empties |
| `SIM_BATTERY=12` | battery level (default 80) |
| `SIM_CHARGING=1` | on USB power |
| `SIM_FAIL_WRITE=1` | the write fails halfway |
| `SIM_THEME=theme:1,a3,c0` | the boot logo marker: here light, green (or `$SIM_DIR/theme`) |
| `SIM_FONT=path` | another font than the player's `assets/fonts/default.otf` |
| `SIM_CORRUPT_WRITE=1` | the system is written with one byte wrong (the read back catches it) |
| `$SIM_DIR/mtd0` | the `uboot` partition; a stock stand-in is made when missing (copy `~/r1-mtd/mtd0` for the real one) |
| `$SIM_DIR/share/` | the bundled bootloader: `u-boot-hiby_r1.bin` and `u-boot-hiby_r1.bin.md5` (the md5 alone on a line) |
| `SIM_UBOOT_BAD=1` | the `uboot` partition has a bad block |
| `SIM_UBOOT_CORRUPT=N` | the first N bootloader writes read back wrong |
| `$SIM_DIR/mtd1` … `mtd6` | the other partitions, made empty (sparse) at their sizes when missing; link `~/r1-mtd/mtdN` there for real data |
| `SIM_MTD_BAD=N` | erase block 1 of mtdN is bad |
| `SIM_MTD_ECC=N` | reading mtdN hits an uncorrectable ECC error |
| `SIM_USERDATA_BUSY=1` | `/usr/data` cannot be unmounted (restore) |

A restore writes `$SIM_DIR/mtdN`; one that is a link (to `~/r1-mtd`, say) is
replaced by a copy first, so the dump itself is never written.

Without a window, `--shots DIR` runs a key script and saves PNG
screenshots: `make -C recovery shots` walks every screen into
`build/shots-r1/`. A script of your own looks like this:

```sh
SIM_FAIL_WRITE=1 recovery/build/host-r1/sonix_recovery_sim --shots /tmp/shots \
    --script "e p e w s:failed x s:menu"
```

(`n`/`p`/`e`/`x` = Next/Prev/Enter/Power, `s:NAME` = screenshot,
`t:MS` = wait, `w` = wait for the update to end.)

## Flashing

With the Sonix U-Boot in fastboot (hold Play/Pause at power-on), both
partitions together:

```sh
fastboot flash kernel2 output/r1/kernel2-recovery.xImage
fastboot flash rootfs2 output/r1/rootfs2-recovery.squashfs
fastboot reboot recovery
```

(With `--stock-kernel` there is no `kernel2-recovery.xImage`: flash
rootfs2 alone.)

Or from a root shell on the player (kernel2 is `mtd3`, rootfs2 the second
`rootfs` in `/proc/mtd`, `mtd4`, on the R1):

```sh
flash_erase /dev/mtd3 0 0 && nandwrite -p /dev/mtd3 kernel2-recovery.xImage
flash_erase /dev/mtd4 0 0 && nandwrite -p /dev/mtd4 rootfs2-recovery.squashfs
```

To go back, flash the stock dump the same way: `~/r1-mtd/mtd3` to kernel2
and `~/r1-mtd/mtd4` to rootfs2.

The console is on UART2 at 3 Mbaud, and the app logs there too.
