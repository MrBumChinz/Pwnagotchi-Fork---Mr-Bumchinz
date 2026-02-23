/*
 * PwnaUI - Icons Header
 * Static bitmap icons for UI elements + PNG macro icons
 */

#ifndef PWNAUI_ICONS_H
#define PWNAUI_ICONS_H

#include <stdint.h>

/* Icon structure (static bitmaps) */
typedef struct {
    const char *name;       /* Icon name (for lookup) */
    uint8_t width;          /* Width in pixels */
    uint8_t height;         /* Height in pixels */
    const uint8_t *bitmap;  /* Packed 1-bit bitmap data */
} icon_t;

/* PNG icon structure (dynamically loaded) */
typedef struct {
    uint8_t *bitmap;        /* 1-bit packed bitmap data (allocated) */
    int width;              /* Width in pixels */
    int height;             /* Height in pixels */
    int stride;             /* Bytes per row */
    int loaded;             /* 1 if successfully loaded */
} png_icon_t;

/* Stat icon indices */
#define STAT_ICON_FOOD      0
#define STAT_ICON_STRENGTH  1
#define STAT_ICON_SPIRIT    2
#define STAT_ICON_COUNT     3

/* Fill levels per stat */
#define FOOD_FILL_LEVELS     4   /* 10, 40, 75, 100 */
#define STRENGTH_FILL_LEVELS 4   /* 10, 40, 75, 100 */
#define SPIRIT_FILL_LEVELS   5   /* 10, 25, 50, 75, 100 */
#define MAX_FILL_LEVELS      5   /* Maximum across all stats */

/* Legacy aliases for backward compatibility */
#define MACRO_ICON_PROTEIN  STAT_ICON_FOOD
#define MACRO_ICON_FAT      STAT_ICON_STRENGTH
#define MACRO_ICON_CARBS    STAT_ICON_SPIRIT
#define MACRO_ICON_COUNT    STAT_ICON_COUNT

/*
 * Initialize icon system (loads PNG macro icons)
 */
int icons_init(void);

/*
 * Cleanup icon resources
 */
void icons_cleanup(void);

/*
 * Get icon by name
 */
const icon_t *icons_get(const char *name);

/*
 * Get icon by index (for iteration)
 */
const icon_t *icons_get_by_index(int index);

/*
 * Get total number of static icons
 */
int icons_count(void);

/*
 * Draw static icon to framebuffer
 */
void icons_draw(uint8_t *framebuffer, const char *name, int x, int y);

/*
 * Get stat icon for a given stat at a given fill percentage.
 * Returns the closest fill-level PNG for that stat.
 * stat_index: STAT_ICON_FOOD, STAT_ICON_STRENGTH, or STAT_ICON_SPIRIT
 * fill_percent: 0-100
 */
const png_icon_t *icons_get_stat(int stat_index, int fill_percent);

/* Legacy accessor (returns 100% fill icon) */
const png_icon_t *icons_get_macro(int index);

/*
 * Draw a stat icon (with fill level) to framebuffer
 * Returns 0 on success, -1 if icon not loaded
 */
int icons_draw_stat(uint8_t *framebuffer, int fb_width, int fb_height,
                    int stat_index, int fill_percent, int x, int y, int invert);

/*
 * Draw a stat icon scaled to specific dimensions
 */
int icons_draw_stat_scaled(uint8_t *framebuffer, int fb_width, int fb_height,
                           int stat_index, int fill_percent, int x, int y,
                           int dst_w, int dst_h, int invert);

/* Legacy draw functions (use 100% fill) */
int icons_draw_macro(uint8_t *framebuffer, int fb_width, int fb_height,
                     int icon_index, int x, int y, int invert);
int icons_draw_macro_scaled(uint8_t *framebuffer, int fb_width, int fb_height,
                            int icon_index, int x, int y,
                            int dst_w, int dst_h, int invert);

/*
 * Draw all 3 stat icons (Food, Strength, Spirit) with individual fill levels.
 * Always shows all 3 icons; each at its own fill-level PNG.
 * Flashes any icon whose fill_percent < 10.
 *
 * food_pct, strength_pct, spirit_pct: 0-100 fill for each stat
 * flash_state: 0 or 1 for flashing toggle (caller tracks timing)
 */
void icons_draw_stat_indicators(uint8_t *framebuffer, int fb_width, int fb_height,
                                int x, int y,
                                int food_pct, int strength_pct, int spirit_pct,
                                int flash_state, int invert);

/* Legacy name */
#define icons_draw_macro_indicator(fb, w, h, x, y, pct, flash, inv) \
    icons_draw_stat_indicators(fb, w, h, x, y, pct, pct, pct, flash, inv)

#endif /* PWNAUI_ICONS_H */
