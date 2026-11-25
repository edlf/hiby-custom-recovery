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
// Before anything is erased the whole package is checked here: the
// compatibility record (the only thing that says which player it is for),
// that ota_update.in names this version, that each image is a kernel or a
// rootfs (once each) and fits its partition, and every chunk against its md5
// in ota_md5_<img>.<img_md5>, with the md5 of them all against img_md5. The
// card stays read-only throughout (storage.h).
//
// Then it writes, as HiBy's updater did but without its scripts: the boot
// record set to "ota:kernel2" first, so a write cut short starts the
// recovery again; then the kernel into "kernel" and the system into the
// first "rootfs" (never kernel2/rootfs2, which this recovery runs from),
// each with flash_erase and nandwrite -m -p (device.h), the chunks checked
// again on their way in. Then each image is read back from flash the way the
// boot reads it and compared with img_md5; only when both match does the
// record go back to "ota:kernel". A failure leaves it on the recovery, and
// the player comes back here until an update succeeds.

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
} update_status_t;

// Starts the worker on `path`, or on the card's <stem>.upt when NULL. -1
// when one is already running. Any .upt can be picked, whatever its name:
// the compatibility record decides (model.h).
int update_start(const char *path);

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
