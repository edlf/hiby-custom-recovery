// Sonix Recovery: replaces HiBy's /usr/bin/recoveryd on the recovery root fs
// (rootfs2), started by /etc/init.d/S92_03_start_recovery. See README.md.

#include "input.h"
#include "log.h"
#include "model.h"
#include "style.h"
#include "ui.h"

#include "lvgl/lvgl.h"

#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

const model_t g_model = MODEL_INIT;

static uint32_t tick_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

#ifdef HOST_BUILD
int sim_main(int argc, char **argv); // sim_platform.c
#endif

int main(int argc, char **argv) {
	(void)argc;
	(void)argv;
	log_open();
	LOG("Sonix Recovery %s for the %s", SONIX_RECOVERY_VERSION, g_model.name);
	// A popen()ed writer that dies must not take this process with it.
	signal(SIGPIPE, SIG_IGN);

	lv_init();
	lv_tick_set_cb(tick_ms);

#ifdef HOST_BUILD
	return sim_main(argc, argv);
#else

	// S11jpeg_display_shell shows the logo and lights the backlight in the
	// background; the panel may not be there yet.
	for (int i = 0; i < 100 && access("/dev/fb0", F_OK) != 0; i++) {
		usleep(50 * 1000);
	}
	lv_display_t *disp = lv_linux_fbdev_create();
	if (!disp || lv_linux_fbdev_set_file(disp, "/dev/fb0") != LV_RESULT_OK) {
		LOG("no framebuffer, giving up");
		return 1;
	}
	int32_t w = lv_display_get_horizontal_resolution(disp), h = lv_display_get_vertical_resolution(disp);
	LOG("display %dx%d", (int)w, (int)h);
	if (w != g_model.panel_w || h != g_model.panel_h) {
		LOG("warning: the %s's panel is %dx%d: wrong model build?", g_model.name, g_model.panel_w, g_model.panel_h);
	}

	style_init();
	lv_indev_t *keys = input_init(disp);
	ui_init(disp, keys);

	for (;;) {
		uint32_t idle = lv_timer_handler();
		if (idle > 20) {
			idle = 20;
		}
		usleep(idle * 1000);
	}
#endif
}
