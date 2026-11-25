#ifndef BATTERY_WIDGET_H
#define BATTERY_WIDGET_H

#include "lvgl/lvgl.h"

#include <stdbool.h>

// Sonix Player's battery indicator (sonix-player/src/gui/shell/topbar.c):
// the shell icon in the text colour, the level as a green bar inside it (red
// at 15% and under), and while charging the notched shell with a yellow bolt
// and no bar. An unknown level is an empty shell in the secondary colour.
// The percentage is written to the left of the icon (in the error colour at
// 15% and under, unless charging); nothing when the level is unknown.

lv_obj_t *battery_widget_create(lv_obj_t *parent);
// percent -1 when unknown.
void battery_widget_set(lv_obj_t *widget, int percent, bool charging);

#endif /* BATTERY_WIDGET_H */
