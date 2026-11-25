#ifndef INPUT_H
#define INPUT_H

#include "lvgl/lvgl.h"

// Touch and side keys, from /dev/input/event*, each node sorted by what it
// reports: absolute X/Y is the touch panel (LVGL's evdev driver), keys are
// read here and turned into LVGL navigation keys:
//
//   Volume up, Previous   LV_KEY_PREV    move up
//   Volume down, Next     LV_KEY_NEXT    move down
//   Play/Pause            LV_KEY_ENTER   press
//   Power                 LV_KEY_ESC     back / cancel
//
// Returns the keypad indev (NULL when no key node was found), for the group.
lv_indev_t *input_init(lv_display_t *disp);

#endif /* INPUT_H */
