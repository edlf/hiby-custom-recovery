// The PC side of make host: the display and input that the framebuffer and
// evdev are on the device. Two ways to run it:
//
//   build/host/zees_recovery_sim
//       a 480x800 window (SDL2, when the build found libsdl2-dev). Mouse is
//       the touch panel; Up/Down (or Tab) move, Enter presses, Esc or
//       Backspace is the Power key.
//
//   build/host/zees_recovery_sim --shots DIR [--script "..."]
//       no window: runs a script of key presses against the real interface
//       and saves PNG screenshots into DIR. Script tokens, space separated:
//         n p e x      Next, Prev, Enter, Power (back)
//         s:NAME       screenshot DIR/NN-NAME.png
//         t:MS         let MS milliseconds pass
//         w            wait for a running update to end
//       Without --script it walks through every screen (DEFAULT_SCRIPT). It
//       starts on the automatic update countdown when $SIM_DIR/ota says
//       ota:kernel2, as it does at first.
//
// The device under it is sim_device.c.

#include "log.h"
#include "model.h"
#include "style.h"
#include "ui.h"
#include "update.h"

#include "lvgl/lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define DEFAULT_SCRIPT                                                                                                 \
	"s:auto-countdown e s:main e s:install-confirm p e t:1500 s:update-running w s:update-result x "                  \
	"n e s:factory-reset x n n e s:advanced e s:choose-file x n e s:backup x s:back-to-advanced x s:main"

// ---------------------------------------------------------------------------
// Keys from the script

static uint32_t queued_key;
static int queued_phase; // 0 nothing, 1 press pending, 2 release pending

static void script_keys_read(lv_indev_t *indev, lv_indev_data_t *data) {
	(void)indev;
	data->key = queued_key;
	if (queued_phase == 1) {
		data->state = LV_INDEV_STATE_PRESSED;
		queued_phase = 2;
	} else {
		data->state = LV_INDEV_STATE_RELEASED;
		queued_phase = 0;
	}
}

static void run_for(uint32_t ms) {
	uint32_t start = lv_tick_get();
	while (lv_tick_elaps(start) < ms) {
		lv_timer_handler();
		usleep(5 * 1000);
	}
}

static void press(uint32_t key) {
	queued_key = key;
	queued_phase = 1;
	run_for(300);
}

// ---------------------------------------------------------------------------
// Screenshots: the RGB565 frame as an RGB PNG

static void png_chunk(FILE *f, const char *type, const unsigned char *data, uint32_t len) {
	unsigned char be[4] = {len >> 24, len >> 16, len >> 8, len};
	fwrite(be, 1, 4, f);
	fwrite(type, 1, 4, f);
	if (len) {
		fwrite(data, 1, len, f);
	}
	uLong crc = crc32(crc32(0, (const Bytef *)type, 4), data, len);
	unsigned char c[4] = {crc >> 24, crc >> 16, crc >> 8, crc};
	fwrite(c, 1, 4, f);
}

static int save_png(const char *path, const uint16_t *px, int w, int h) {
	size_t raw_len = (size_t)h * (1 + 3 * w);
	unsigned char *raw = malloc(raw_len);
	uLongf z_len = compressBound(raw_len);
	unsigned char *z = malloc(z_len);
	if (!raw || !z) {
		free(raw);
		free(z);
		return -1;
	}
	unsigned char *o = raw;
	for (int y = 0; y < h; y++) {
		*o++ = 0; // filter: none
		for (int x = 0; x < w; x++) {
			uint16_t c = px[y * w + x];
			*o++ = (unsigned char)(((c >> 11) & 0x1f) * 255 / 31);
			*o++ = (unsigned char)(((c >> 5) & 0x3f) * 255 / 63);
			*o++ = (unsigned char)((c & 0x1f) * 255 / 31);
		}
	}
	int rc = -1;
	FILE *f = fopen(path, "wb");
	if (f && compress2(z, &z_len, raw, raw_len, 6) == Z_OK) {
		static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
		unsigned char ihdr[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0};
		fwrite(sig, 1, 8, f);
		png_chunk(f, "IHDR", ihdr, 13);
		png_chunk(f, "IDAT", z, (uint32_t)z_len);
		png_chunk(f, "IEND", NULL, 0);
		rc = ferror(f) ? -1 : 0;
	}
	if (f) {
		fclose(f);
	}
	free(raw);
	free(z);
	return rc;
}

// ---------------------------------------------------------------------------
// Headless display: LVGL renders straight into one full-screen buffer

static uint16_t *frame;

static void headless_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
	(void)area;
	(void)px;
	lv_display_flush_ready(disp);
}

static int run_script(lv_display_t *disp, const char *dir, const char *script) {
	mkdir(dir, 0755);
	char *s = strdup(script);
	int shot = 0, rc = 0;
	run_for(300);
	for (char *tok = strtok(s, " "); tok; tok = strtok(NULL, " ")) {
		if (strcmp(tok, "n") == 0) {
			press(LV_KEY_NEXT);
		} else if (strcmp(tok, "p") == 0) {
			press(LV_KEY_PREV);
		} else if (strcmp(tok, "e") == 0) {
			press(LV_KEY_ENTER);
		} else if (strcmp(tok, "x") == 0) {
			press(LV_KEY_ESC);
		} else if (strncmp(tok, "t:", 2) == 0) {
			run_for((uint32_t)atoi(tok + 2));
		} else if (strcmp(tok, "w") == 0) {
			update_status_t st;
			uint32_t start = lv_tick_get();
			do {
				run_for(100);
				update_get(&st);
			} while (st.state == UPDATE_RUNNING && lv_tick_elaps(start) < 300000);
			run_for(300); // the interface's 100 ms poll picks the result up
		} else if (strncmp(tok, "s:", 2) == 0) {
			lv_obj_invalidate(lv_screen_active());
			lv_refr_now(disp);
			char path[512];
			snprintf(path, sizeof(path), "%s/%02d-%s.png", dir, ++shot, tok + 2);
			if (save_png(path, frame, lv_display_get_horizontal_resolution(disp),
						 lv_display_get_vertical_resolution(disp)) == 0) {
				printf("shot %s\n", path);
			} else {
				fprintf(stderr, "cannot write %s\n", path);
				rc = 1;
			}
		} else {
			fprintf(stderr, "unknown script token '%s'\n", tok);
			rc = 2;
			break;
		}
	}
	free(s);
	return rc;
}

static lv_display_t *headless_display(void) {
	int w = g_model.panel_w, h = g_model.panel_h;
	size_t size = (size_t)w * h * 2;
	frame = calloc(1, size);
	lv_display_t *disp = lv_display_create(w, h);
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(disp, frame, NULL, (uint32_t)size, LV_DISPLAY_RENDER_MODE_DIRECT);
	lv_display_set_flush_cb(disp, headless_flush);
	return disp;
}

// ---------------------------------------------------------------------------
// Window

#if LV_USE_SDL
static lv_indev_read_cb_t sdl_keys_read;

// The SDL driver sends arrows as LV_KEY_UP/DOWN, which a focus group does not
// move on; the device's Volume keys are PREV/NEXT.
static void window_keys_read(lv_indev_t *indev, lv_indev_data_t *data) {
	sdl_keys_read(indev, data);
	switch (data->key) {
	case LV_KEY_UP:
	case LV_KEY_LEFT:
		data->key = LV_KEY_PREV;
		break;
	case LV_KEY_DOWN:
	case LV_KEY_RIGHT:
		data->key = LV_KEY_NEXT;
		break;
	case LV_KEY_BACKSPACE:
		data->key = LV_KEY_ESC;
		break;
	default:
		break;
	}
}

static int run_window(void) {
	lv_display_t *disp = lv_sdl_window_create(g_model.panel_w, g_model.panel_h);
	if (!disp) {
		fprintf(stderr, "cannot open an SDL window\n");
		return 1;
	}
	lv_indev_t *mouse = lv_sdl_mouse_create();
	lv_indev_set_display(mouse, disp);
	lv_indev_t *kbd = lv_sdl_keyboard_create();
	lv_indev_set_display(kbd, disp);
	sdl_keys_read = lv_indev_get_read_cb(kbd);
	lv_indev_set_read_cb(kbd, window_keys_read);
	ui_init(disp, kbd);
	for (;;) {
		uint32_t idle = lv_timer_handler();
		usleep((idle > 20 ? 20 : idle) * 1000);
	}
}
#endif

int sim_main(int argc, char **argv) {
	const char *shots = NULL, *script = DEFAULT_SCRIPT;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--shots") == 0 && i + 1 < argc) {
			shots = argv[++i];
		} else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
			script = argv[++i];
		} else {
			fprintf(stderr, "usage: %s [--shots DIR [--script \"s:main n e ...\"]]\n", argv[0]);
			return 2;
		}
	}
	LOG("sim: %s, SIM_DIR=%s", g_model.name, getenv("SIM_DIR") ? getenv("SIM_DIR") : "sim");
	style_init();

	if (shots) {
		lv_display_t *disp = headless_display();
		lv_indev_t *keys = lv_indev_create();
		lv_indev_set_type(keys, LV_INDEV_TYPE_KEYPAD);
		lv_indev_set_read_cb(keys, script_keys_read);
		lv_indev_set_display(keys, disp);
		ui_init(disp, keys);
		return run_script(disp, shots, script);
	}
#if LV_USE_SDL
	return run_window();
#else
	fprintf(stderr, "built without SDL2: install libsdl2-dev and run make host again for a window,\n"
					"or take screenshots with --shots DIR\n");
	return 2;
#endif
}
