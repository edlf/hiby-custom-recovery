#ifndef LV_CONF_H
#define LV_CONF_H

/* Only what differs from LVGL's defaults (lv_conf_internal.h fills in the
 * rest). */

/* RGB565, what the panels scan out. */
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565

/* The C library's malloc, string and sprintf rather than LVGL's own pool. */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/* Built-in fonts, ASCII only: the recovery has no font files to load. */
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_DEFAULT &lv_font_montserrat_24

/* MiSans through FreeType (style.c), as in the player: glyphs rendered on
 * demand into a bounded cache. */
#define LV_USE_FREETYPE 1
#define LV_FREETYPE_USE_LVGL_PORT 0
#define LV_FREETYPE_CACHE_FT_GLYPH_CNT 256

/* The framebuffer and evdev on the device; on the PC (make host) an SDL
 * window when the build found SDL2 (SIM_HAVE_SDL), and the headless
 * screenshot mode either way. */
#ifdef HOST_BUILD
  #define LV_USE_LINUX_FBDEV 0
  #define LV_USE_EVDEV 0
  #if SIM_HAVE_SDL
    #define LV_USE_SDL 1
    #define LV_SDL_INCLUDE_PATH <SDL2/SDL.h>
  #endif
#else
  #define LV_USE_LINUX_FBDEV 1
  #define LV_LINUX_FBDEV_RENDER_MODE LV_DISPLAY_RENDER_MODE_PARTIAL
  #define LV_USE_EVDEV 1
#endif

/* Nothing here draws images, vectors or animations worth a thread. */
#define LV_USE_OS LV_OS_NONE
#define LV_USE_DEMO_WIDGETS 0
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0

#endif /* LV_CONF_H */
