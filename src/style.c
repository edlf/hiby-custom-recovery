#include "style.h"

#include "device.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Adwaita, as the player has it (theme.c). Dark: window #242424, cards
// #303030, pressed #404040. Light: the player's lighter window, white cards.
static const style_palette_t DARK = {
	.bg = LV_COLOR_MAKE(36, 36, 36),
	.card = LV_COLOR_MAKE(48, 48, 48),
	.card_pressed = LV_COLOR_MAKE(64, 64, 64),
	.text = LV_COLOR_MAKE(255, 255, 255),
	.dim = LV_COLOR_MAKE(160, 160, 160),
	.on_accent = LV_COLOR_MAKE(255, 255, 255),
	.destructive = LV_COLOR_MAKE(192, 28, 40), // Adwaita destructive #c01c28
	.ok = LV_COLOR_MAKE(143, 240, 164),		   // success, dark #8ff0a4
	.warn = LV_COLOR_MAKE(248, 228, 92),	   // warning, dark #f8e45c
	.err = LV_COLOR_MAKE(255, 123, 99),		   // error, dark #ff7b63
	.dark = true,
};
static const style_palette_t LIGHT = {
	.bg = LV_COLOR_MAKE(236, 237, 240),
	.card = LV_COLOR_MAKE(255, 255, 255),
	.card_pressed = LV_COLOR_MAKE(221, 223, 228),
	.text = LV_COLOR_MAKE(50, 50, 50),
	.dim = LV_COLOR_MAKE(115, 115, 115),
	.on_accent = LV_COLOR_MAKE(255, 255, 255),
	.destructive = LV_COLOR_MAKE(192, 28, 40),
	.ok = LV_COLOR_MAKE(27, 133, 83),	// success, light #1b8553
	.warn = LV_COLOR_MAKE(156, 110, 3), // warning, light #9c6e03
	.err = LV_COLOR_MAKE(192, 28, 40),	// error, light #c01c28
	.dark = false,
};

// The player's accent presets, same order (theme.c ACCENT_COLORS).
static const lv_color_t ACCENTS[6] = {
	LV_COLOR_MAKE(53, 132, 228), // blue, the default
	LV_COLOR_MAKE(255, 120, 0),	 // orange
	LV_COLOR_MAKE(229, 165, 10), // yellow
	LV_COLOR_MAKE(46, 194, 126), // green
	LV_COLOR_MAKE(145, 65, 172), // purple
	LV_COLOR_MAKE(213, 97, 153), // pink
};

style_palette_t S;
const lv_font_t *F20 = &lv_font_montserrat_20, *F24 = &lv_font_montserrat_24, *F32 = &lv_font_montserrat_32;

static lv_font_t *open_font(const char *path, int size, const lv_font_t *fallback) {
	lv_font_t *f = lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, (uint32_t)size,
										   LV_FREETYPE_FONT_STYLE_NORMAL);
	if (f) {
		f->fallback = fallback; // the LV_SYMBOL_* icons
	}
	return f;
}

static void init_fonts(void) {
	const char *path = STYLE_FONT_FILE;
#ifdef HOST_BUILD
	const char *env = getenv("SIM_FONT");
	path = env && env[0] ? env : SIM_FONT_FILE;
#endif
	if (access(path, R_OK) != 0) {
		LOG("style: no %s, built-in font", path);
		return;
	}
	lv_font_t *f20 = open_font(path, 20, &lv_font_montserrat_20);
	lv_font_t *f24 = open_font(path, 24, &lv_font_montserrat_24);
	lv_font_t *f32 = open_font(path, 32, &lv_font_montserrat_32);
	if (f20 && f24 && f32) {
		F20 = f20;
		F24 = f24;
		F32 = f32;
		LOG("style: font %s", path);
	} else {
		LOG("style: %s cannot be opened, built-in font", path);
	}
}

void style_init(void) {
	char m[16] = "";
	theme_marker_read(m, sizeof(m));
	// "theme:T" and, from the player, ",aA": anything missing stays dark/blue
	bool dark = true;
	int accent = 0;
	if (strncmp(m, "theme:", 6) == 0) {
		dark = m[6] != '1';
		if (m[7] == ',' && m[8] == 'a' && m[9] >= '0' && m[9] <= '5') {
			accent = m[9] - '0';
		}
	}
	S = dark ? DARK : LIGHT;
	S.accent = ACCENTS[accent];
	LOG("style: %s, accent %d (marker '%s')", dark ? "dark" : "light", accent, m);
	init_fonts();
}
