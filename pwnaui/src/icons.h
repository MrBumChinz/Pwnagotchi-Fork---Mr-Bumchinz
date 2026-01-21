/*
 * PwnaUI - Icons Header
 * Static bitmap icons for UI elements
 */

#ifndef PWNAUI_ICONS_H
#define PWNAUI_ICONS_H

#include <stdint.h>

/* Icon structure */
typedef struct {
    const char *name;       /* Icon name (for lookup) */
    uint8_t width;          /* Width in pixels */
    uint8_t height;         /* Height in pixels */
    const uint8_t *bitmap;  /* Packed 1-bit bitmap data */
} icon_t;

/*
 * Initialize icon system
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
 * Get total number of icons
 */
int icons_count(void);

/*
 * Draw icon to framebuffer
 */
void icons_draw(uint8_t *framebuffer, const char *name, int x, int y);

#endif /* PWNAUI_ICONS_H */
