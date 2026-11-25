#ifndef STYLE_H
#define STYLE_H

#include "lvgl/lvgl.h"

#include <stdbool.h>

// The look: Sonix Player's layout (GNOME Adwaita: cards with 12 px corners,
// sonix-player/src/gui/shell/theme.c) in a dark violet palette chosen for
// contrast (style.c), and the MiSans face.
//
// The font: MiSans Regular (the player's default.otf) through FreeType,
// from STYLE_FONT_FILE in the recovery root fs. Without it the built-in
// Montserrat is used. The built-in font is also every FreeType size's
// fallback, for the LV_SYMBOL_* icons MiSans does not have.

#define STYLE_FONT_FILE "/usr/share/zees-recovery/default.otf"

typedef struct {
	lv_color_t bg;			 // page background
	lv_color_t card;		 // rows and buttons
	lv_color_t card_pressed; // the same under a finger
	lv_color_t text;
	lv_color_t dim; // secondary text
	lv_color_t accent;		 // fills and the focus ring
	lv_color_t accent_text; // text in the accent colour: lighter, to read on bg
	lv_color_t on_accent; // text on an accent or a destructive fill
	lv_color_t destructive;
	lv_color_t ok;
	lv_color_t warn;
	lv_color_t err;
	bool dark;
} style_palette_t;

extern style_palette_t S;
extern const lv_font_t *F20, *F24, *F32;

// Sets the palette and opens the fonts. After lv_init(), before any widget.
void style_init(void);

#endif /* STYLE_H */
