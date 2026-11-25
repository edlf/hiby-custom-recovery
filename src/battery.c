#include "battery.h"

#include "style.h"

extern const lv_image_dsc_t icon_battery;
extern const lv_image_dsc_t icon_battery_charging_body;
extern const lv_image_dsc_t icon_battery_charging_bolt;

// As in the player's topbar.c: the bitmaps are 38x38 renderings of 24x24
// SVGs; the shell's cavity is 3..17 across and 7..17 down in SVG units.
#define ICON_SIZE 38
#define CAVITY_X 3
#define CAVITY_Y 7
#define CAVITY_W 14
#define CAVITY_H 10
#define LOW_PERCENT 15

// The widget is a row: the percentage, then the icon. The icon's children,
// in this order: fill, shell, bolt.
lv_obj_t *battery_widget_create(lv_obj_t *parent) {
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_SIZE_CONTENT, ICON_SIZE);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 4, 0);

	lv_obj_t *pct = lv_label_create(row);
	lv_obj_set_style_text_font(pct, F20, 0);
	lv_obj_set_style_text_color(pct, S.text, 0);
	lv_label_set_text(pct, "");

	lv_obj_t *w = lv_obj_create(row);
	lv_obj_remove_style_all(w);
	lv_obj_set_size(w, ICON_SIZE, ICON_SIZE);
	lv_obj_set_scrollable(w, false);

	// Scaled by the edges, as the player does, so rounding leaves no gap at
	// the bottom of the cavity.
	int cx = (CAVITY_X * ICON_SIZE) / 24, cy = (CAVITY_Y * ICON_SIZE) / 24;
	int cw = (((CAVITY_X + CAVITY_W) * ICON_SIZE + 23) / 24) - cx;
	int ch = (((CAVITY_Y + CAVITY_H) * ICON_SIZE + 23) / 24) - cy;

	lv_obj_t *fill = lv_obj_create(w);
	lv_obj_remove_style_all(fill);
	lv_obj_set_pos(fill, cx, cy);
	lv_obj_set_size(fill, cw, ch);
	lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(fill, 1, 0);
	lv_obj_set_user_data(fill, (void *)(intptr_t)cw); // full width, for the level

	lv_obj_t *shell = lv_image_create(w);
	lv_image_set_src(shell, &icon_battery);
	lv_obj_set_style_image_recolor_opa(shell, LV_OPA_COVER, 0);
	lv_obj_set_style_image_recolor(shell, S.dim, 0);

	lv_obj_t *bolt = lv_image_create(w);
	lv_image_set_src(bolt, &icon_battery_charging_bolt);
	lv_obj_set_style_image_recolor_opa(bolt, LV_OPA_COVER, 0);
	lv_obj_set_style_image_recolor(bolt, lv_color_make(245, 205, 60), 0);
	lv_obj_set_hidden(bolt, true);
	lv_obj_set_hidden(fill, true);
	return row;
}

void battery_widget_set(lv_obj_t *row, int percent, bool charging) {
	lv_obj_t *pct = lv_obj_get_child(row, 0), *w = lv_obj_get_child(row, 1);
	lv_obj_t *fill = lv_obj_get_child(w, 0), *shell = lv_obj_get_child(w, 1), *bolt = lv_obj_get_child(w, 2);
	if (percent < 0) {
		lv_label_set_text(pct, "");
	} else {
		lv_label_set_text_fmt(pct, "%d%%", percent);
	}
	lv_obj_set_style_text_color(pct, percent >= 0 && percent <= LOW_PERCENT && !charging ? S.err : S.text, 0);
	lv_image_set_src(shell, charging ? &icon_battery_charging_body : &icon_battery);
	lv_obj_set_hidden(bolt, !charging);
	lv_obj_set_style_image_recolor(shell, percent < 0 ? S.dim : S.text, 0);
	// While charging the notched shell would let a bar leak through: the bolt
	// says it all, as in the player.
	if (percent < 0 || charging) {
		lv_obj_set_hidden(fill, true);
		return;
	}
	int full = (int)(intptr_t)lv_obj_get_user_data(fill);
	int width = full * percent / 100;
	if (width < 3 && percent > 0) {
		width = 3; // a nearly flat battery still reads as a red sliver
	}
	lv_obj_set_width(fill, width);
	lv_obj_set_style_bg_color(fill, percent <= LOW_PERCENT ? lv_color_make(220, 60, 50) : lv_color_make(60, 190, 90),
							  0);
	lv_obj_set_hidden(fill, false);
}
