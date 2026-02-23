/*
 * PwnaUI - Icons Implementation
 * Static bitmap icons for UI elements + PNG macro icons
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "icons.h"
#include "renderer.h"
#include "lodepng.h"

/* PNG stat icons with fill levels (loaded from files) */
/* Each stat has multiple fill-level PNGs: Food(10,40,75,100), Strength(10,40,75,100), Spirit(10,25,50,75,100) */
static png_icon_t g_stat_icons[STAT_ICON_COUNT][MAX_FILL_LEVELS];
static int g_stat_fill_values[STAT_ICON_COUNT][MAX_FILL_LEVELS] = {
    { 0, 11, 41, 76, -1 },    /* Food fill levels */
    { 0, 11, 41, 76, -1 },    /* Strength fill levels */
    { 0, 11, 26, 51, 76 },    /* Spirit fill levels */
};
static int g_stat_fill_count[STAT_ICON_COUNT] = {
    FOOD_FILL_LEVELS,
    STRENGTH_FILL_LEVELS,
    SPIRIT_FILL_LEVELS,
};
static const char *g_stat_icon_paths[STAT_ICON_COUNT][MAX_FILL_LEVELS] = {
    { "/home/pi/pwnaui/assets/Food_10.png", "/home/pi/pwnaui/assets/Food_40.png",
      "/home/pi/pwnaui/assets/Food_75.png", "/home/pi/pwnaui/assets/Food_100.png", NULL },
    { "/home/pi/pwnaui/assets/Strength_10.png", "/home/pi/pwnaui/assets/Strength_40.png",
      "/home/pi/pwnaui/assets/Strength_75.png", "/home/pi/pwnaui/assets/Strength_100.png", NULL },
    { "/home/pi/pwnaui/assets/Spirit_10.png", "/home/pi/pwnaui/assets/Spirit_25.png",
      "/home/pi/pwnaui/assets/Spirit_50.png", "/home/pi/pwnaui/assets/Spirit_75.png",
      "/home/pi/pwnaui/assets/Spirit_100.png" },
};

/*
 * Load a PNG file and convert to 1-bit bitmap (same as themes.c)
 */
static int load_png_icon(const char *path, png_icon_t *icon) {
    unsigned char *rgba = NULL;
    unsigned width, height;
    unsigned error;
    
    memset(icon, 0, sizeof(*icon));
    
    error = lodepng_decode32_file(&rgba, &width, &height, path);
    if (error) {
        fprintf(stderr, "[icons] PNG decode error %u: %s - %s\n", 
                error, lodepng_error_text(error), path);
        return -1;
    }
    
    /* Allocate 1-bit bitmap */
    int stride = (width + 7) / 8;
    size_t bitmap_size = stride * height;
    icon->bitmap = calloc(1, bitmap_size);
    if (!icon->bitmap) {
        free(rgba);
        return -1;
    }
    
    icon->width = width;
    icon->height = height;
    icon->stride = stride;
    
    /* Convert RGBA to 1-bit: black pixels (low luminance) = 1 */
    for (unsigned y = 0; y < height; y++) {
        for (unsigned x = 0; x < width; x++) {
            unsigned idx = (y * width + x) * 4;
            unsigned char r = rgba[idx + 0];
            unsigned char g = rgba[idx + 1];
            unsigned char b = rgba[idx + 2];
            unsigned char a = rgba[idx + 3];
            
            /* Luminance formula */
            unsigned lum = (299 * r + 587 * g + 114 * b) / 1000;
            
            /* Black pixel if opaque and dark */
            int is_black = (a >= 128) && (lum < 128);
            
            if (is_black) {
                int byte_idx = y * stride + x / 8;
                int bit_idx = 7 - (x % 8);
                icon->bitmap[byte_idx] |= (1 << bit_idx);
            }
        }
    }
    
    free(rgba);
    icon->loaded = 1;
    fprintf(stderr, "[icons] Loaded %s: %dx%d\n", path, width, height);
    return 0;
}

/*
 * Signal strength bars (4 levels)
 * 16x12 pixels each
 */
static const uint8_t icon_signal_0[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const uint8_t icon_signal_1[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00
};

static const uint8_t icon_signal_2[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x00,
    0x0C, 0x00, 0x0C, 0x00, 0x0F, 0x00, 0x0F, 0x00
};

static const uint8_t icon_signal_3[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x30, 0x00, 0x30, 0x00, 0x30, 0x00, 0x3C, 0x00,
    0x3C, 0x00, 0x3C, 0x00, 0x3F, 0x00, 0x3F, 0x00
};

static const uint8_t icon_signal_4[] = {
    0x00, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00,
    0xF0, 0x00, 0xF0, 0x00, 0xF0, 0x00, 0xFC, 0x00,
    0xFC, 0x00, 0xFC, 0x00, 0xFF, 0x00, 0xFF, 0x00
};

/*
 * WiFi icon
 * 16x12 pixels
 */
static const uint8_t icon_wifi[] = {
    0x07, 0xE0, 0x1F, 0xF8, 0x38, 0x1C, 0x63, 0xC6,
    0x0F, 0xF0, 0x1C, 0x38, 0x01, 0x80, 0x07, 0xE0,
    0x06, 0x60, 0x00, 0x00, 0x01, 0x80, 0x01, 0x80
};

/*
 * Battery icons (empty, low, med, high, full, charging)
 * 20x10 pixels each
 */
static const uint8_t icon_battery_empty[] = {
    0xFF, 0xFC, 0x00, 0x80, 0x02, 0x00, 0x80, 0x03,
    0x00, 0x80, 0x03, 0x00, 0x80, 0x03, 0x00, 0x80,
    0x02, 0x00, 0xFF, 0xFC, 0x00
};

static const uint8_t icon_battery_low[] = {
    0xFF, 0xFC, 0x00, 0x80, 0x02, 0x00, 0x9C, 0x03,
    0x00, 0x9C, 0x03, 0x00, 0x9C, 0x03, 0x00, 0x80,
    0x02, 0x00, 0xFF, 0xFC, 0x00
};

static const uint8_t icon_battery_med[] = {
    0xFF, 0xFC, 0x00, 0x80, 0x02, 0x00, 0x9E, 0x03,
    0x00, 0x9E, 0x03, 0x00, 0x9E, 0x03, 0x00, 0x80,
    0x02, 0x00, 0xFF, 0xFC, 0x00
};

static const uint8_t icon_battery_high[] = {
    0xFF, 0xFC, 0x00, 0x80, 0x02, 0x00, 0x9F, 0x83,
    0x00, 0x9F, 0x83, 0x00, 0x9F, 0x83, 0x00, 0x80,
    0x02, 0x00, 0xFF, 0xFC, 0x00
};

static const uint8_t icon_battery_full[] = {
    0xFF, 0xFC, 0x00, 0x80, 0x02, 0x00, 0x9F, 0xC3,
    0x00, 0x9F, 0xC3, 0x00, 0x9F, 0xC3, 0x00, 0x80,
    0x02, 0x00, 0xFF, 0xFC, 0x00
};

static const uint8_t icon_battery_charging[] = {
    0xFF, 0xFC, 0x00, 0x80, 0x02, 0x00, 0x82, 0x03,
    0x00, 0x84, 0x03, 0x00, 0x9F, 0x03, 0x00, 0x84,
    0x02, 0x00, 0xFF, 0xFC, 0x00
};

/*
 * Lock/unlock icons
 * 12x14 pixels
 */
static const uint8_t icon_locked[] = {
    0x1E, 0x00, 0x33, 0x00, 0x21, 0x00, 0x21, 0x00,
    0x7F, 0x80, 0x7F, 0x80, 0x7F, 0x80, 0x7B, 0x80,
    0x73, 0x80, 0x7F, 0x80, 0x7F, 0x80, 0x7F, 0x80,
    0x7F, 0x80, 0x00, 0x00
};

static const uint8_t icon_unlocked[] = {
    0x1E, 0x00, 0x33, 0x00, 0x21, 0x00, 0x01, 0x00,
    0x7F, 0x80, 0x7F, 0x80, 0x7F, 0x80, 0x7B, 0x80,
    0x73, 0x80, 0x7F, 0x80, 0x7F, 0x80, 0x7F, 0x80,
    0x7F, 0x80, 0x00, 0x00
};

/*
 * Bluetooth icon
 * 8x14 pixels
 */
static const uint8_t icon_bluetooth[] = {
    0x08, 0x0C, 0x0A, 0x49, 0x2A, 0x1C, 0x08, 0x1C,
    0x2A, 0x49, 0x0A, 0x0C, 0x08, 0x00
};

/*
 * Plugin/gear icon
 * 14x14 pixels
 */
static const uint8_t icon_plugin[] = {
    0x03, 0x00, 0x03, 0x00, 0x1F, 0xE0, 0x30, 0x30,
    0x60, 0x18, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0xCC, 0xCC, 0x60, 0x18, 0x30, 0x30, 0x1F, 0xE0,
    0x03, 0x00, 0x03, 0x00
};

/*
 * AI brain icon
 * 16x14 pixels
 */
static const uint8_t icon_ai[] = {
    0x07, 0xE0, 0x18, 0x18, 0x20, 0x04, 0x46, 0x62,
    0x49, 0x92, 0x49, 0x92, 0x46, 0x62, 0x40, 0x02,
    0x49, 0x92, 0x46, 0x62, 0x20, 0x04, 0x18, 0x18,
    0x07, 0xE0, 0x00, 0x00
};

/*
 * Icon table
 */
static const icon_t g_icons[] = {
    { "signal_0", 16, 12, icon_signal_0 },
    { "signal_1", 16, 12, icon_signal_1 },
    { "signal_2", 16, 12, icon_signal_2 },
    { "signal_3", 16, 12, icon_signal_3 },
    { "signal_4", 16, 12, icon_signal_4 },
    { "wifi", 16, 12, icon_wifi },
    { "battery_empty", 20, 10, icon_battery_empty },
    { "battery_low", 20, 10, icon_battery_low },
    { "battery_med", 20, 10, icon_battery_med },
    { "battery_high", 20, 10, icon_battery_high },
    { "battery_full", 20, 10, icon_battery_full },
    { "battery_charging", 20, 10, icon_battery_charging },
    { "locked", 12, 14, icon_locked },
    { "unlocked", 12, 14, icon_unlocked },
    { "bluetooth", 8, 14, icon_bluetooth },
    { "plugin", 14, 14, icon_plugin },
    { "ai", 16, 14, icon_ai },
};

#define NUM_ICONS (sizeof(g_icons) / sizeof(g_icons[0]))

/*
 * Initialize icon system
 */
int icons_init(void) {
    static const char *stat_names[] = { "Food", "Strength", "Spirit" };
    fprintf(stderr, "[icons] Initializing stat icons (Food/Strength/Spirit)...\n");
    /* Load all fill-level PNGs for each stat */
    for (int s = 0; s < STAT_ICON_COUNT; s++) {
        for (int f = 0; f < g_stat_fill_count[s]; f++) {
            const char *path = g_stat_icon_paths[s][f];
            if (!path) continue;
            fprintf(stderr, "[icons] Loading %s %d%%: %s\n", stat_names[s], g_stat_fill_values[s][f], path);
            if (load_png_icon(path, &g_stat_icons[s][f]) != 0) {
                fprintf(stderr, "[icons] Warning: Failed to load %s %d%%\n", stat_names[s], g_stat_fill_values[s][f]);
            }
        }
    }
    fprintf(stderr, "[icons] Stat icon init complete\n");
    return 0;
}

/*
 * Cleanup icon resources
 */
void icons_cleanup(void) {
    /* Free stat icon PNGs */
    for (int s = 0; s < STAT_ICON_COUNT; s++) {
        for (int f = 0; f < MAX_FILL_LEVELS; f++) {
            if (g_stat_icons[s][f].bitmap) {
                free(g_stat_icons[s][f].bitmap);
                g_stat_icons[s][f].bitmap = NULL;
                g_stat_icons[s][f].loaded = 0;
            }
        }
    }
}

/*
 * Get icon by name
 */
const icon_t *icons_get(const char *name) {
    if (!name) return NULL;
    
    for (size_t i = 0; i < NUM_ICONS; i++) {
        if (strcmp(g_icons[i].name, name) == 0) {
            return &g_icons[i];
        }
    }
    
    return NULL;
}

/*
 * Get icon by index
 */
const icon_t *icons_get_by_index(int index) {
    if (index < 0 || (size_t)index >= NUM_ICONS) {
        return NULL;
    }
    return &g_icons[index];
}

/*
 * Get total number of icons
 */
int icons_count(void) {
    return (int)NUM_ICONS;
}

/*
 * Draw icon to framebuffer
 */
void icons_draw(uint8_t *framebuffer, const char *name, int x, int y) {
    const icon_t *icon = icons_get(name);
    if (!icon) return;
    
    int width = renderer_get_width();
    int height = renderer_get_height();
    int row_bytes = (icon->width + 7) / 8;
    
    for (int iy = 0; iy < icon->height; iy++) {
        int dst_y = y + iy;
        if (dst_y < 0 || dst_y >= height) continue;
        
        for (int ix = 0; ix < icon->width; ix++) {
            int dst_x = x + ix;
            if (dst_x < 0 || dst_x >= width) continue;
            
            /* Get source pixel from icon bitmap */
            int src_byte = iy * row_bytes + ix / 8;
            int src_bit = 7 - (ix % 8);
            int pixel = (icon->bitmap[src_byte] >> src_bit) & 1;
            
            /* Set pixel in framebuffer (0 = black, 1 = white) */
            /* Icons are drawn as black-on-white */
            if (pixel) {
                renderer_set_pixel(framebuffer, width, dst_x, dst_y, 0);
            }
        }
    }
}

/*
 * Get the best fill-level icon for a stat at a given percentage.
 * Picks the closest fill-level that doesn't exceed the actual percentage.
 * E.g. 60% food → Food_40.png (next lower), 100% → Food_100.png
 */
const png_icon_t *icons_get_stat(int stat_index, int fill_percent) {
    if (stat_index < 0 || stat_index >= STAT_ICON_COUNT) return NULL;
    if (fill_percent <= 0) return NULL;

    int count = g_stat_fill_count[stat_index];
    int best = -1;
    for (int i = 0; i < count; i++) {
        if (g_stat_fill_values[stat_index][i] <= fill_percent) {
            best = i;  /* Keep the highest fill level <= requested */
        }
    }
    if (best < 0) best = 0;  /* If all levels are above, use lowest */
    if (!g_stat_icons[stat_index][best].loaded) return NULL;
    return &g_stat_icons[stat_index][best];
}

/* Legacy accessor — returns 100% fill icon */
const png_icon_t *icons_get_macro(int index) {
    return icons_get_stat(index, 100);
}

/*
 * Internal: draw a png_icon_t to framebuffer (unscaled)
 */
static int draw_png_icon(const png_icon_t *icon, uint8_t *framebuffer,
                         int fb_width, int fb_height, int x, int y, int invert) {
    if (!icon || !icon->bitmap) return -1;

    for (int iy = 0; iy < icon->height; iy++) {
        int screen_y = y + iy;
        if (screen_y < 0 || screen_y >= fb_height) continue;
        for (int ix = 0; ix < icon->width; ix++) {
            int screen_x = x + ix;
            if (screen_x < 0 || screen_x >= fb_width) continue;
            int src_byte = iy * icon->stride + ix / 8;
            int src_bit = 7 - (ix % 8);
            int pixel = (icon->bitmap[src_byte] >> src_bit) & 1;
            pixel = !pixel;  /* PNG 1=black → framebuffer 0=black */
            if (invert) pixel = !pixel;
            int fb_byte = (screen_y * fb_width + screen_x) / 8;
            int fb_bit = 7 - (screen_x % 8);
            if (pixel) framebuffer[fb_byte] |= (1 << fb_bit);
            else       framebuffer[fb_byte] &= ~(1 << fb_bit);
        }
    }
    return 0;
}

/*
 * Internal: draw a png_icon_t scaled to specific dimensions
 */
static int draw_png_icon_scaled(const png_icon_t *icon, uint8_t *framebuffer,
                                int fb_width, int fb_height,
                                int x, int y, int dst_w, int dst_h, int invert) {
    if (!icon || !icon->bitmap) return -1;
    int src_w = icon->width;
    int src_h = icon->height;

    for (int dy = 0; dy < dst_h; dy++) {
        int screen_y = y + dy;
        if (screen_y < 0 || screen_y >= fb_height) continue;
        int src_y = (dy * src_h) / dst_h;
        for (int dx = 0; dx < dst_w; dx++) {
            int screen_x = x + dx;
            if (screen_x < 0 || screen_x >= fb_width) continue;
            int src_x = (dx * src_w) / dst_w;
            int src_byte = src_y * icon->stride + src_x / 8;
            int src_bit = 7 - (src_x % 8);
            int pixel = (icon->bitmap[src_byte] >> src_bit) & 1;
            pixel = !pixel;
            if (invert) pixel = !pixel;
            int fb_byte = (screen_y * fb_width + screen_x) / 8;
            int fb_bit = 7 - (screen_x % 8);
            if (pixel) framebuffer[fb_byte] |= (1 << fb_bit);
            else       framebuffer[fb_byte] &= ~(1 << fb_bit);
        }
    }
    return 0;
}

/* Draw stat icon at fill level */
int icons_draw_stat(uint8_t *framebuffer, int fb_width, int fb_height,
                    int stat_index, int fill_percent, int x, int y, int invert) {
    const png_icon_t *icon = icons_get_stat(stat_index, fill_percent);
    return draw_png_icon(icon, framebuffer, fb_width, fb_height, x, y, invert);
}

/* Draw stat icon scaled */
int icons_draw_stat_scaled(uint8_t *framebuffer, int fb_width, int fb_height,
                           int stat_index, int fill_percent, int x, int y,
                           int dst_w, int dst_h, int invert) {
    const png_icon_t *icon = icons_get_stat(stat_index, fill_percent);
    return draw_png_icon_scaled(icon, framebuffer, fb_width, fb_height, x, y, dst_w, dst_h, invert);
}

/* Legacy draw functions (use icon by index at 100% fill) */
int icons_draw_macro(uint8_t *framebuffer, int fb_width, int fb_height,
                     int icon_index, int x, int y, int invert) {
    return icons_draw_stat(framebuffer, fb_width, fb_height, icon_index, 100, x, y, invert);
}

int icons_draw_macro_scaled(uint8_t *framebuffer, int fb_width, int fb_height,
                            int icon_index, int x, int y,
                            int dst_w, int dst_h, int invert) {
    return icons_draw_stat_scaled(framebuffer, fb_width, fb_height, icon_index, 100, x, y, dst_w, dst_h, invert);
}

/*
 * Draw all 3 stat icons with individual fill levels.
 * Exact same positions and sizes as the original Protein/Fat/Carbs layout:
 *   Position 1: Spirit   — native size (23x20)
 *   Position 2: Strength — native size (24x21)
 *   Position 3: Food     — native size (24x21)
 * All bottom-aligned to baseline_height=21, spacing=2.
 * Each icon's fill-level PNG is chosen by its stat percentage.
 * Icons flash if stat < 10%, hidden if stat == 0%.
 */
void icons_draw_stat_indicators(uint8_t *framebuffer, int fb_width, int fb_height,
                                int x, int y,
                                int food_pct, int strength_pct, int spirit_pct,
                                int flash_state, int invert) {
    int baseline_height = 21;
    /* Fixed positions so icons never shift when others are hidden */
    /* Spirit: 23px wide, Strength: 24px wide, Food: 24px wide, spacing: 2px */
    int spirit_x = x;            /* Position 1: x+0 */
    int strength_x = x + 25;     /* Position 2: x+23+2 */
    int food_x = x + 51;         /* Position 3: x+25+24+2 */

    /* Icon 1: Spirit  fixed position */
    if (spirit_pct > 0 && (spirit_pct >= 10 || flash_state)) {
        const png_icon_t *icon = icons_get_stat(STAT_ICON_SPIRIT, spirit_pct);
        if (icon && icon->bitmap) {
            int icon_y = y + (baseline_height - icon->height);
            draw_png_icon(icon, framebuffer, fb_width, fb_height,
                          spirit_x, icon_y, invert);
        }
    }

    /* Icon 2: Strength  fixed position */
    if (strength_pct > 0 && (strength_pct >= 10 || flash_state)) {
        const png_icon_t *icon = icons_get_stat(STAT_ICON_STRENGTH, strength_pct);
        if (icon && icon->bitmap) {
            int icon_y = y + (baseline_height - icon->height);
            draw_png_icon(icon, framebuffer, fb_width, fb_height,
                          strength_x, icon_y, invert);
        }
    }

    /* Icon 3: Food  fixed position */
    if (food_pct > 0 && (food_pct >= 10 || flash_state)) {
        const png_icon_t *icon = icons_get_stat(STAT_ICON_FOOD, food_pct);
        if (icon && icon->bitmap) {
            int icon_y = y + (baseline_height - icon->height);
            draw_png_icon(icon, framebuffer, fb_width, fb_height,
                          food_x, icon_y, invert);
        }
    }
}

