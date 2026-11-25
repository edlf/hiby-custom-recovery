#ifndef MODEL_H
#define MODEL_H

// What differs between the players. The recovery is built once per model
// (make MODEL=r1 / MODEL=r3proii): its root fs is that model's own anyway, and
// a compile-time model means an update file can never be checked against the
// wrong player's rules.
//
//   name          shown on screen
//   upt_stem      the update file is <upt_stem>.upt on the card, in any case.
//                 It is the file the automatic update installs; any other
//                 .upt can be picked by hand from the card (update.h).
//   compat        prefix that ota_v<N>/ota_compatibility.in must start with
//                 ("compatibility=hiby_r1_x1600_auto_rf_auto"). The R3 Pro II
//                 images carry no such file, so for it an absent file is
//                 accepted; a present one must still match.
//   compat_needed whether the file must be there
//   rootfs_hostname  what /usr/resource/hostname holds in this model's
//                 system image, stock and Sonix alike ("HiBy R1",
//                 "HiBy R3PROII"). The check that a package is for this
//                 player whatever its file name: see update.c.
//   uboot_image   the Sonix bootloader's file name in the recovery root fs
//                 (bootloader.h), the base image without a partition table
//   uboot_installable  whether the recovery offers to install it. Not on the
//                 R3 Pro II: its board has not been compared with its stock
//                 loader yet (uboot/README.md).
//   panel_w/h     the framebuffer the model has, only checked to warn

typedef struct {
	const char *name;
	const char *upt_stem;
	const char *compat;
	int compat_needed;
	const char *rootfs_hostname;
	const char *uboot_image;
	int uboot_installable;
	int panel_w, panel_h;
} model_t;

#if defined(MODEL_R1)
#define MODEL_INIT {"HiBy R1", "r1", "hiby_r1_", 1, "HiBy R1", "u-boot-hiby_r1.bin", 1, 480, 800}
#elif defined(MODEL_R3PROII)
#define MODEL_INIT {"HiBy R3 Pro II", "r3proii", "hiby_r3proii_", 0, "HiBy R3PROII", "u-boot-hiby_r3proii.bin", 0, 480, 720}
#else
#error "build with MODEL_R1 or MODEL_R3PROII (make MODEL=r1 or MODEL=r3proii)"
#endif

extern const model_t g_model;

#endif /* MODEL_H */
