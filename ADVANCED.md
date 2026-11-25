# Zee's Recovery: advanced topics

How the recovery works inside, for developers. For everyday use see
[README.md](README.md).

- [How the recovery starts](#how-the-recovery-starts)
- [The recovery image](#the-recovery-image)
- [Look](#look)
- [Automatic update](#automatic-update)
- [How an update is installed](#how-an-update-is-installed)
- [Backup and restore](#backup-and-restore)
- [Installing the bootloader](#installing-the-bootloader)
- [Build details](#build-details)
- [Getting the NAND dump](#getting-the-nand-dump)
- [Simulator (PC)](#simulator-pc)
- [Flashing from the player](#flashing-from-the-player)

## How the recovery starts

The first page of the `ota` MTD partition is the **boot record**:

- `ota:kernel` boots the normal system.
- `ota:kernel2` boots the recovery (kernel2 + rootfs2).

The SPL boots the recovery when:

- the record says `ota:kernel2` (the player's own updater writes this, then reboots)
- Volume up is held at power-on
- you run `fastboot reboot recovery` (Zee's bootloader only)

Inside the recovery, BusyBox init runs `/etc/init.d/rcS`, which runs the
`S??*` scripts in order: modules, logo and backlight, `/usr/data` (UBI),
the factory reset hook (`S39_recovery.recovery`), Wi-Fi, and last
`S92_03_start_recovery`. That last script used to start `recoveryd`.
`pack_recovery.sh` changes it to start `zees_recovery` and restart it if
it exits.

## The recovery image

The recovery root fs is the stock main root fs without the music player.
`pack_recovery.sh` also removes what the app doesn't need:

- `recoveryd`
- Bluetooth, D-Bus (and HiBy's `sys_server`), `ntpd`, audio
- HiBy's OTA scripts (`etc/ota_bin/`, `etc/ota_info`), since the app does the update itself
- time zones, web pages, GLib/XML/locale data
- every shared library nothing links to anymore

Most of this couldn't run anyway: the stock recovery has no `dbus-daemon`,
`bluetoothd`, glib or `libasound`. Wi-Fi stays. On the R1 the image shrinks
from 19.3 MB to 15.0 MB.

## Look

- Sonix Player's layout (GNOME Adwaita, 12 px rounded cards) in a dark violet palette picked for contrast: background `#1b1525`, cards `#2c2340`, text `#f5f1fb` (16:1), secondary text `#bdb2d6` (8.9:1).
- Accent `#8257e5` for buttons, progress bars and the focus ring (white on it 4.7:1); text in the accent colour (titles, the backup or folder shown) uses the lighter `#c4a7ff` (8.7:1). Red stays for destructive buttons.
- Font is MiSans via FreeType (`/usr/share/zees-recovery/default.otf`). If it's missing, LVGL's built-in Montserrat is used.
- The battery icon is the player's own top-bar icon, with the percentage next to it (red at 15% and under, unless charging).
- Main menu rows grow to fill the screen (up to 100 px each).
- The red and blue LEDs breathe together, so the player glows purple instead of HiBy's red (`/sys/class/leds/red` and `blue`, `breathing` trigger).

## Automatic update

If the boot record says `ota:kernel2` at start, the player asked for an
update, and the recovery behaves like the stock one:

1. A 5 s countdown with "Install now" / "Cancel". Play/Pause and Power cancel.
2. The update runs.
3. A 5 s countdown, then a restart into the system. "Back" stays in the recovery.

If you entered with Volume up, the record still says `ota:kernel`, so the
menu shows instead.

A failed or cancelled update leaves the record on `ota:kernel2`, so the
next start offers it again. "Reboot to system" sets it back.

## How an update is installed

1. **Mount.** The card is mounted read-only (`/dev/mmcblk0p1` or the whole
   disk, vfat or exfat) at `/tmp/mnt/sd`. The `.upt` (an ISO image) is
   loop-mounted at `/tmp/upt`.
2. **Check the package** before anything is erased:
   - `ota_config.in`, `ota_vN/ota_vN.ok` and `ota_update.in` exist and
     name the same version.
   - Each image in `ota_update.in` is `kernel` or `rootfs` (once each) and
     fits its partition.
   - Every chunk matches its md5 in `ota_md5_<img>.<img_md5>`. Chunks are
     named `<img>.0000.<img_md5>`, then each after the md5 of the one
     before. Together they must add up to `img_size` and hash to `img_md5`.
   - `ota_vN/ota_compatibility.in` starts with `hiby_r1_` on the R1. This
     is the only check of which player a package is for. R3 Pro II
     packages don't have this file, so there it's optional, but if present
     it must match.
3. **Mark the update.** The record is set to `ota:kernel2`, so if the
   write is cut short the player starts the recovery again.
4. **Write.** The kernel goes to `kernel` and the system to the first
   `rootfs`. The recovery's own `kernel2`/`rootfs2` are never touched.
   This uses `flash_erase /dev/mtdN 0 0`, then streams the chunks into
   `nandwrite -m -p --input-size=<img_size>`. Bad blocks are skipped; a
   block that fails to write is marked bad and its data moves to the next
   one. Each chunk's md5 is checked again on the way in, so a card that
   suddenly reads differently stops the write.
5. **Verify.** Each image is read back with `nanddump`, taking the good
   blocks in order as the boot loader does, and compared with `img_md5`. Only if both match
   does the record go back to `ota:kernel`. Otherwise the player keeps
   starting the recovery instead of a broken system, and the update is
   reported as failed.

Updates are refused below 20% battery unless the player is charging.

## Backup and restore

### The card stays read-only

The card is mounted read-only (if something else already mounted it, it's
remounted read-only). It's only made writable during:

- **Back up partitions**, for its folder and log
- the copy of the old bootloader before **Install fastboot**

It goes back to read-only afterwards, whether that worked or not. Updates
and restores only read the card; their log stays in
`/tmp/zees_recovery.log`.

### Backup

**Back up partitions** copies the raw flash to
`zees-backup-<date>-<time>/` on the card (`zees-backup-N/` if the clock
isn't set). The folder holds:

- `mtd0-uboot.bin` … `mtd6-userdata.bin`, each at full partition size
- `mtd.txt`, the partition table
- `md5sums.txt`
- `info.txt`

On the R1 that's 125 MB, or 80 MB without user data. Free space is checked
first. `zees_recovery.log` is copied next to the folder at the end.

How each partition is read:

- With `nanddump --bb=padbad` (ECC corrected), computing the md5 as it
  goes. The file on the card is then read back and must give the same md5.
- Bad blocks come out as 0xff in their place, so every block is at its
  offset.
- Blocks with uncorrectable ECC errors keep their data.
- Both are counted per partition in `info.txt` (from `nanddump`'s
  messages), and the result screen lists affected partitions.
- A block `nanddump` can't read at all is written as 0xff and counted as a
  read error in `info.txt`; a new `nanddump` goes on from the next block.

To check a backup on a PC:

```sh
cd zees-backup-*/ && md5sum -c md5sums.txt
```

### Restore

**Restore backup** lists the `zees-backup-*` folders at the top of the
card, then the partitions in the chosen one, with checkboxes. Kernel and
system are ticked by default; bootloader, boot record and user data are
opt-in. Ticking the bootloader first shows a warning (a failed or wrong
bootloader leaves only Ingenic's USB boot tool); it stays ticked only after
**Include bootloader**. The confirmation lists just the partitions to be
written. Kernel2 and rootfs2 aren't offered, because the recovery is running
from them (use fastboot instead).

Before anything is written, it checks:

- `info.txt` names this player, and `mtd.txt` matches its partition table line for line.
- Each chosen file matches its md5 in `md5sums.txt` and the partition's size.
- No block has gone bad since the backup where the backup has data (that partition is refused, since the data wouldn't land in the right place).
- For user data: `/usr/data` is unmounted and its UBI detached (refused if in use).

Then each partition is:

1. erased with `flash_erase`, then written with `nandwrite -k` (all-0xff
   pages left erased, as UBI expects). The backup is fed to it without the
   blocks that are bad now (those hold only 0xff, as checked above), and
   `nandwrite` skips the bad blocks, so every other block lands at the
   offset it was read from. No `-m`: a block that fails to write would push
   the rest one block on, so the write fails instead.
2. read back with `nanddump` and compared block by block; if it differs,
   written once more

The bootloader's current contents (read with `nanddump` too) are kept in RAM
and written back if its read-back fails. A restart is needed afterwards; the result screen offers
"Reboot to system".

## Installing the bootloader

Zee's bootloader is recognized by the string `ota:fastboot` in the `uboot`
partition (its SPL and U-Boot both look for that request). HiBy's
bootloader has no fastboot, so with it "Reboot to fastboot" would just
start the system. That's why the menu shows **Install fastboot** instead.

It's not offered on the R3 Pro II yet: its board hasn't been compared with
its stock loader (see `uboot/README.md`).

`pack_recovery.sh` bundles `output/r1/u-boot-hiby_r1.bin` (the base image,
with a blank partition table area) as
`/usr/share/zees-recovery/u-boot-hiby_r1.bin`, plus its md5.

The install:

1. Checks the bundled image: md5, SPL header magic, size, blank table area.
2. Reads the whole `uboot` partition. Refuses if there's no partition table
   (`nand` at 0x5800) or a bad block.
3. Copies the player's own partition table (0x5800–0x5bff) into the image.
   For a stock R1 the result equals `u-boot-hiby_r1-withparts.bin`.
4. Saves the old partition to the card as `uboot-backup-N.bin`, if a card
   is present.
5. Erases, writes, reads back and compares. On a mismatch it writes once
   more. If that fails, it restores the old partition and verifies it. If
   *that* fails, it says so and offers only "Try again", which works from
   the copy in RAM, not the flash.

> **Warning:** the boot ROM has no fallback. A `uboot` partition without a
> working SPL can only be fixed with Ingenic's USB boot mode.

## Build details

`build_recovery.sh` runs these steps (`--help` lists them all):

1. extract the stock recovery root fs from a NAND dump (first time only, see below)
2. build kernel and U-Boot: `./build_images.sh MODEL`
3. build the app: `make -C recovery MODEL=MODEL`
4. pack the image: `./pack_recovery.sh --kernel MODEL`

### Kernel modules

The Sonix kernel builds in many drivers that HiBy's root fs loads as
modules (keys, backlight, GPIO, I2C…), and its own modules are built with
it. So `pack_recovery.sh` assembles `module_driver/` itself, like the Sonix
firmware packer does for the main system:

1. start from the stock recovery's folder
2. add the changed loader scripts and `S43wifi_bcm_init_config` from
   `overlay/<model>/` in this repo (HiBy's scripts as the Sonix firmware
   ships them: the module list, the Wi-Fi/BT power and touch settings)
3. add every `.ko` from `output/<model>/modules/`
4. drop the modules listed in `output/<model>/modules.builtin`, and the
   scripts that only load them

Nothing comes from sonix-player.

> **Always flash kernel2 and rootfs2 together.** These modules with HiBy's
> kernel2, or HiBy's modules with this kernel, leave the recovery without
> keys or a screen.

### Dependencies

The app needs nothing outside `recovery/`. On first use `make` fetches or
builds into `deps/` (not in git):

- LVGL 9.6.0 (cloned)
- FreeType 2.13.3 (downloaded; built static for the device, and for the PC in `build/host-freetype/`)
- the cross toolchain (GCC 9, glibc 2.27), built by `rockboxdev/rockboxdev.sh`

The toolchain takes longest. `make toolchain` builds only it, and
`TOOLCHAIN_DIR=/path/to/rockbox-toolchain` uses one built elsewhere.

Other notes:

- `src/glibc_compat.h` pins glibc symbol versions to what the device's glibc 2.22 has.
- The font, MiSans Regular, is `assets/fonts/default.otf`.
- `make clean` removes `build/`; `make distclean` also removes `deps/`.

## Getting the NAND dump

The stock recovery (kernel2 + rootfs2) isn't in HiBy's `.upt` files; it has
to come from the player's flash. From a root adb shell on stock firmware:

```sh
adb shell cat /proc/mtd                            # mtd4 must be the second "rootfs"
mkdir -p ~/r1-mtd && adb exec-out cat /dev/mtd4 > ~/r1-mtd/mtd4
```

If adbd is too old for `exec-out`, run `cat /dev/mtd4 > /mnt/sd_0/mtd4` on
the player and copy the file off the card. Don't use `/tmp`, it's RAM.

`build_recovery.sh` looks in `~/<model>-mtd/` (or `--dump DIR`) and only
needs `mtd4`. Once Zee's Recovery is installed, **Back up partitions**
writes the same files to the card.

## Simulator (PC)

The interface and update logic also run on a PC against a simulated device:
a folder acts as the card and a file as the boot record. Nothing outside
that folder is touched. The package checks are the real ones, and an update
really writes into `$SIM_DIR/mtd1` and `mtd2`, so the read-back check is
real too.

```sh
make -C recovery host                  # needs libsdl2-dev for the window
mkdir -p recovery/build/sim/sd
ln -sf "$PWD/original_firmware/r1(v2.0).upt" recovery/build/sim/sd/r1.upt
SIM_DIR=recovery/build/sim recovery/build/host-r1/zees_recovery_sim
```

Controls:

| PC | Player |
|---|---|
| Mouse | touch |
| Up / Down / Tab | Volume keys |
| Enter | Play/Pause |
| Esc / Backspace | Power |

Reboot and power off quit the simulator.

### Simulated device files

| Path | Meaning |
|---|---|
| `SIM_DIR` | the simulated device, default `./sim` (`recovery/build/sim` is ignored by git) |
| `$SIM_DIR/sd/` | the card; present if the folder exists |
| `$SIM_DIR/ota` | the boot record (`ota:kernel2` at first) |
| `$SIM_DIR/data/` | what a factory reset empties |
| `$SIM_DIR/mtd0` | the `uboot` partition; a stock stand-in is created if missing (copy `~/r1-mtd/mtd0` for the real one) |
| `$SIM_DIR/mtd1` … `mtd6` | other partitions, created empty (sparse) if missing; link `~/r1-mtd/mtdN` for real data |
| `$SIM_DIR/share/` | the bundled bootloader: `u-boot-hiby_r1.bin` and `u-boot-hiby_r1.bin.md5` (just the md5 on one line) |

A restore writes `$SIM_DIR/mtdN`. If that's a link (to `~/r1-mtd`, say),
it's replaced by a copy first, so your dump is never overwritten.

### Test switches

| Variable | Effect |
|---|---|
| `SIM_BATTERY=12` | battery level (default 80) |
| `SIM_CHARGING=1` | on USB power |
| `SIM_CARD_SIZE=bytes` | card capacity in the main menu (default 31914983424, 32 GB) |
| `SIM_FONT=path` | use a different font |
| `SIM_FAIL_WRITE=1` | the update write fails halfway |
| `SIM_CORRUPT_WRITE=1` | the system is written with one wrong byte (read-back catches it) |
| `SIM_UBOOT_BAD=1` | the `uboot` partition has a bad block |
| `SIM_UBOOT_CORRUPT=N` | the first N bootloader writes read back wrong |
| `SIM_MTD_BAD=N` | erase block 1 of mtdN is bad |
| `SIM_MTD_ECC=N` | reading mtdN hits an uncorrectable ECC error |
| `SIM_USERDATA_BUSY=1` | `/usr/data` can't be unmounted (restore) |

### Screenshots without a window

`--shots DIR` runs a key script and saves PNG screenshots.
`make -C recovery shots` walks every screen into `build/shots-r1/`. Your own
script:

```sh
SIM_FAIL_WRITE=1 recovery/build/host-r1/zees_recovery_sim --shots /tmp/shots \
    --script "e p e w s:failed x s:menu"
```

| Command | Meaning |
|---|---|
| `n` / `p` / `e` / `x` | Next / Prev / Enter / Power |
| `s:NAME` | take a screenshot |
| `t:MS` | wait MS milliseconds |
| `w` | wait for the update to finish |

## Flashing from the player

Instead of fastboot, you can flash from a root shell on the player. On the
R1, kernel2 is `mtd3` and rootfs2 is `mtd4` (the second `rootfs` in
`/proc/mtd`):

```sh
flash_erase /dev/mtd3 0 0 && nandwrite -p /dev/mtd3 kernel2-recovery.xImage
flash_erase /dev/mtd4 0 0 && nandwrite -p /dev/mtd4 rootfs2-recovery.squashfs
```
