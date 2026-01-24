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

/* Macro icon indices */
#define MACRO_ICON_PROTEIN  0
#define MACRO_ICON_FAT      1
#define MACRO_ICON_CARBS    2
#define MACRO_ICON_COUNT    3

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
 * Get macro icon by index (MACRO_ICON_PROTEIN, etc.)
 */
const png_icon_t *icons_get_macro(int index);

/*
 * Draw macro icon to framebuffer
 * Returns 0 on success, -1 if icon not loaded
 */
int icons_draw_macro(uint8_t *framebuffer, int fb_width, int fb_height,
                     int icon_index, int x, int y, int invert);

/*
 * Draw macro icon scaled to specific dimensions
 * Returns 0 on success, -1 if icon not loaded
 */
int icons_draw_macro_scaled(uint8_t *framebuffer, int fb_width, int fb_height,
                            int icon_index, int x, int y,
                            int dst_w, int dst_h, int invert);

/*
 * Draw macro icons based on overall macro percentage
 * Shows 3/2/1/0 icons based on fill level, with flashing at <10%
 * 
 * macro_percent: Average macro fill (0-100)
 * flash_state: 0 or 1 for flashing toggle (caller tracks timing)
 */
void icons_draw_macro_indicator(uint8_t *framebuffer, int fb_width, int fb_height,
                                int x, int y, int macro_percent, int flash_state, int invert);

#endif /* PWNAUI_ICONS_H */
