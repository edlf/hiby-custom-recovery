#ifndef UI_H
#define UI_H

#include "lvgl/lvgl.h"

// Builds the interface on `disp`; `keys` (may be NULL) gets the focus group.
void ui_init(lv_display_t *disp, lv_indev_t *keys);

#endif /* UI_H */
