#include "style.h"

#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// A dark violet theme, chosen for reading: every text colour has at least
// 7:1 against the background (WCAG AAA), white on the accent 4.7:1 (AA), and
// the accent focus ring 3:1 against a card.
//   window #1b1525, cards #2c2340, pressed #3c3157
//   text #f5f1fb (16:1), secondary #bdb2d6 (8.9:1)
//   accent #8257e5 for fills and the focus ring; text in the accent colour
//   (headings, the folder shown) is the lighter #c4a7ff (8.7:1)
static const style_palette_t PURPLE = {
	.bg = LV_COLOR_MAKE(0x1b, 0x15, 0x25),
	.card = LV_COLOR_MAKE(0x2c, 0x23, 0x40),
	.card_pressed = LV_COLOR_MAKE(0x3c, 0x31, 0x57),
	.text = LV_COLOR_MAKE(0xf5, 0xf1, 0xfb),
	.dim = LV_COLOR_MAKE(0xbd, 0xb2, 0xd6),
	.accent = LV_COLOR_MAKE(0x82, 0x57, 0xe5),
	.accent_text = LV_COLOR_MAKE(0xc4, 0xa7, 0xff),
	.on_accent = LV_COLOR_MAKE(255, 255, 255),
	.destructive = LV_COLOR_MAKE(192, 28, 40), // red, kept apart from the accent; white on it 6.1:1
	.ok = LV_COLOR_MAKE(143, 240, 164),		   // 12.8:1
	.warn = LV_COLOR_MAKE(248, 228, 92),	   // 13.8:1
	.err = LV_COLOR_MAKE(255, 123, 99),		   // 7.0:1
	.dark = true,
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
	S = PURPLE;
	init_fonts();
}
