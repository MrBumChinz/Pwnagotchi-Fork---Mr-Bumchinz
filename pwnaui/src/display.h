/*
 * PwnaUI - Display Header
 * Hardware abstraction layer for e-ink and framebuffer displays
 */

#ifndef PWNAUI_DISPLAY_H
#define PWNAUI_DISPLAY_H

#include <stdint.h>
#include <stddef.h>

/*
 * Display driver types
 */
typedef enum {
    DISPLAY_DUMMY = 0,          /* No-op dummy display */
    DISPLAY_FRAMEBUFFER,        /* Linux framebuffer (/dev/fb0) */
    DISPLAY_WAVESHARE_2IN13_V2, /* Waveshare 2.13" V2 e-ink */
    DISPLAY_WAVESHARE_2IN13_V3, /* Waveshare 2.13" V3 e-ink */
    DISPLAY_WAVESHARE_2IN13_V4, /* Waveshare 2.13" V4 e-ink */
    DISPLAY_WAVESHARE_2IN7,     /* Waveshare 2.7" e-ink */
    DISPLAY_WAVESHARE_1IN54,    /* Waveshare 1.54" e-ink */
    DISPLAY_INKY_PHAT,          /* Pimoroni Inky pHAT */
} display_type_t;

/*
 * Initialize display subsystem with type and dimensions
 * Returns 0 on success, -1 on error
 */
int display_init(display_type_t type, int width, int height);

/*
 * Cleanup display resources
 */
void display_cleanup(void);

/*
 * Get current display type
 */
display_type_t display_get_type(void);

/*
 * Clear display to a color (0=white, 1=black)
 */
int display_clear(int color);

/*
 * Update display with framebuffer content
 * Returns 0 on success, -1 on error
 */
int display_update(const uint8_t *framebuffer);

/*
 * Partial update of a region
 */
int display_partial_update(const uint8_t *framebuffer, int x, int y, int w, int h);

/*
 * Get display dimensions
 */
int display_get_width(void);
int display_get_height(void);

/*
 * Check if display supports partial refresh
 */
int display_supports_partial(void);

/*
 * Check if display supports grayscale
 */
int display_supports_grayscale(void);

/*
 * Get bits per pixel
 */
int display_get_bpp(void);

/*
 * Calculate buffer size for given dimensions and bpp
 */
size_t display_calc_buffer_size(int width, int height, int bpp);

/*
 * Set SPI speed (for tuning)
 */
int display_set_spi_speed(int speed_hz);

/*
 * Sleep/wake display (power saving)
 */
int display_sleep(void);
int display_wake(void);

/*
 * Get display type name
 */
const char *display_type_name(display_type_t type);

#endif /* PWNAUI_DISPLAY_H */
