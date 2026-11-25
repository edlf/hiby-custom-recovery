#ifndef UPDATE_H
#define UPDATE_H

#include <stdbool.h>

// Installs <model>.upt from the card into the normal system's kernel and
// rootfs partitions, on a worker thread. The interface polls update_get().
//
// A .upt is an ISO image:
//   ota_config.in                       current_version=N
//   ota_vN/ota_vN.ok                    present when the package is complete
//   ota_vN/ota_compatibility.in         compatibility=hiby_r1_x1600_... (R1)
//   ota_vN/ota_update.in                img_type/img_name/img_size/img_md5 per image
//   ota_vN/<img>.NNNN.<md5>             the image in 512 KiB chunks
//   ota_vN/ota_md5_<img>.<md5 of chunk 0>  one md5 per chunk
//
// Before anything is erased the whole package is checked here: the model's
// file name, the compatibility record, that each image fits its partition and
// that the chunks join up to the size and md5 ota_update.in gives. The writing
// itself is left to the stock /etc/ota_bin/local_ota_update.sh, which is what
// the stock recovery runs too.
//
// That script writes to whichever kernel the boot record does NOT name: run
// with "ota:kernel2" it writes kernel/rootfs, the normal system. Entered with
// Volume up the record still says "ota:kernel", and the script would aim at
// this recovery's own partitions -- so the record is set to "ota:kernel2"
// first. The script puts "ota:kernel" back when it succeeds; when it fails
// the record stays on the recovery, and the player comes back here until an
// update succeeds.

typedef enum {
	UPDATE_IDLE = 0,
	UPDATE_RUNNING,
	UPDATE_DONE,
	UPDATE_FAILED,
} update_state_t;

typedef struct {
	update_state_t state;
	char phase[64];	   // what it is doing now
	int percent;	   // of the current phase, -1 when unknown
	char message[256]; // the outcome, when DONE or FAILED
	int system_touched; // the normal system's flash was erased at some point
	int compat_overridable; // FAILED only on the compatibility record: the
							// system image is this player's (update_start(.., true))
} update_status_t;

// Starts the worker on `path`, or on the card's <stem>.upt when NULL. -1
// when one is already running.
//
// Any .upt can be picked, whatever its name: the model is checked inside the
// package, from its system image (rootfs.squashfs). The chunks are joined
// into a temporary file on the card in the same pass as the md5, that file is
// loop-mounted read-only and its /usr/resource/hostname must name this
// player ("HiBy R1"); the R1's compatibility record is checked as before. A
// package with no system image is refused.
//
// override_compat: the compatibility record (ota_compatibility.in) may be
// wrong or missing. The user's choice, offered when a run fails with
// compat_overridable; the system image check still applies.
int update_start(const char *path, bool override_compat);

// One folder of the card for the file browser: its sub-folders and .upt
// files (nothing else; hidden ones and the system's skipped), folders first,
// each sorted by name. `rel` is the folder on the card, "" for the top.
// Mounts the card if needed. Returns how many, -1 when it cannot be read.
typedef struct {
	char path[800]; // full path
	char name[256];
	bool is_dir;
	long long size;
} upt_entry_t;
int update_list_dir(const char *rel, upt_entry_t *out, int max);
void update_get(update_status_t *out);

#endif /* UPDATE_H */
