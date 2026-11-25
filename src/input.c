#include "input.h"

#include "log.h"

#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAX_KEY_FDS 8
#define BITS_PER_LONG (8 * sizeof(long))
#define TEST_BIT(bit, arr) (((arr)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1UL)

static int key_fds[MAX_KEY_FDS];
static int n_key_fds;
static uint32_t pending_key;
static bool pending_pressed;

static uint32_t map_key(int code) {
	switch (code) {
	case KEY_VOLUMEUP:
	case KEY_PREVIOUSSONG:
	case KEY_UP:
		return LV_KEY_PREV;
	case KEY_VOLUMEDOWN:
	case KEY_NEXTSONG:
	case KEY_DOWN:
		return LV_KEY_NEXT;
	case KEY_PLAYPAUSE:
	case KEY_ENTER:
		return LV_KEY_ENTER;
	case KEY_POWER:
	case KEY_ESC:
		return LV_KEY_ESC;
	default:
		return 0;
	}
}

// One key event per call: LVGL reads again at once when data->continue_reading
// is set, so a press and its release in the same burst both get through.
static void keys_read(lv_indev_t *indev, lv_indev_data_t *data) {
	(void)indev;
	struct input_event ev;
	for (int i = 0; i < n_key_fds; i++) {
		while (read(key_fds[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
			if (ev.type != EV_KEY || ev.value == 2) { // 2: autorepeat
				continue;
			}
			uint32_t k = map_key(ev.code);
			if (!k) {
				continue;
			}
			pending_key = k;
			pending_pressed = ev.value != 0;
			data->key = pending_key;
			data->state = pending_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
			data->continue_reading = true;
			return;
		}
	}
	data->key = pending_key;
	data->state = pending_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

lv_indev_t *input_init(lv_display_t *disp) {
	bool have_touch = false;
	for (int n = 0; n < 16; n++) {
		char path[32];
		snprintf(path, sizeof(path), "/dev/input/event%d", n);
		int fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0) {
			continue;
		}
		char name[64] = "?";
		ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
		unsigned long evbits[(EV_MAX + BITS_PER_LONG) / BITS_PER_LONG] = {0};
		unsigned long absbits[(ABS_MAX + BITS_PER_LONG) / BITS_PER_LONG] = {0};
		ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits);
		if (TEST_BIT(EV_ABS, evbits)) {
			ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
		}
		bool touch = TEST_BIT(EV_ABS, evbits) && (TEST_BIT(ABS_X, absbits) || TEST_BIT(ABS_MT_POSITION_X, absbits));

		// The headphone remote (earpods_*) sends media keys too; it is left
		// out so a cable in the jack cannot press anything here.
		if (strncmp(name, "earpods_", 8) == 0) {
			LOG("input: %s (%s): headphone remote, ignored", path, name);
			close(fd);
		} else if (touch && !have_touch) {
			close(fd);
			lv_indev_t *t = lv_evdev_create(LV_INDEV_TYPE_POINTER, path);
			if (t) {
				lv_indev_set_display(t, disp);
				have_touch = true;
				LOG("input: %s (%s): touch", path, name);
			}
		} else if (TEST_BIT(EV_KEY, evbits) && !touch && n_key_fds < MAX_KEY_FDS) {
			key_fds[n_key_fds++] = fd;
			LOG("input: %s (%s): keys", path, name);
		} else {
			close(fd);
		}
	}
	if (!have_touch) {
		LOG("input: no touch panel found");
	}
	if (n_key_fds == 0) {
		LOG("input: no key device found");
		return NULL;
	}
	lv_indev_t *kp = lv_indev_create();
	lv_indev_set_type(kp, LV_INDEV_TYPE_KEYPAD);
	lv_indev_set_read_cb(kp, keys_read);
	lv_indev_set_display(kp, disp);
	return kp;
}
