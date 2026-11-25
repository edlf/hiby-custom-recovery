#ifndef STYLE_H
#define STYLE_H

#include "lvgl/lvgl.h"

#include <stdbool.h>

// The look of Sonix Player (sonix-player/src/gui/shell/theme.c): GNOME
// Adwaita, dark or light, with one of its six accents, and the MiSans face.
//
// Which theme and accent: what the player last chose, from the boot logo
// marker it keeps on the "ota" partition (13 bytes at 0x20000,
// "theme:T,aA,cC": T 1 light / 2 dark, A the accent 0-5, see the player's
// bootlogo.c). Anything else, the stock firmware's "theme:1"/"theme:2"
// included, is read as far as it goes; nothing at all is dark and blue.
//
// The font: MiSans Regular (the player's default.otf) through FreeType,
// from STYLE_FONT_FILE in the recovery root fs. Without it the built-in
// Montserrat is used. The built-in font is also every FreeType size's
// fallback, for the LV_SYMBOL_* icons MiSans does not have.

#define STYLE_FONT_FILE "/usr/share/sonix-recovery/default.otf"

typedef struct {
	lv_color_t bg;			 // page background
	lv_color_t card;		 // rows and buttons
	lv_color_t card_pressed; // the same under a finger
	lv_color_t text;
	lv_color_t dim; // secondary text
	lv_color_t accent;
	lv_color_t on_accent; // text on an accent or a destructive fill
	lv_color_t destructive;
	lv_color_t ok;
	lv_color_t warn;
	lv_color_t err;
	bool dark;
} style_palette_t;

extern style_palette_t S;
extern const lv_font_t *F20, *F24, *F32;

// Reads the marker and opens the fonts. After lv_init(), before any widget.
void style_init(void);

#endif /* STYLE_H */
