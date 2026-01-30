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

/* PNG macro icons (loaded from files) */
static png_icon_t g_macro_icons[MACRO_ICON_COUNT];
static const char *g_macro_icon_paths[MACRO_ICON_COUNT] = {
    "/home/pi/pwnaui/assets/Protein.png",
    "/home/pi/pwnaui/assets/Fat.png",
    "/home/pi/pwnaui/assets/carbs.png"
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
    fprintf(stderr, "[icons] Initializing macro icons...\n");
    /* Load PNG macro icons */
    for (int i = 0; i < MACRO_ICON_COUNT; i++) {
        fprintf(stderr, "[icons] Loading icon %d: %s\n", i, g_macro_icon_paths[i]);
        if (load_png_icon(g_macro_icon_paths[i], &g_macro_icons[i]) != 0) {
            fprintf(stderr, "[icons] Warning: Failed to load macro icon %d\n", i);
        }
    }
    fprintf(stderr, "[icons] Macro icon init complete\n");
    return 0;
}

/*
 * Cleanup icon resources
 */
void icons_cleanup(void) {
    /* Free PNG macro icons */
    for (int i = 0; i < MACRO_ICON_COUNT; i++) {
        if (g_macro_icons[i].bitmap) {
            free(g_macro_icons[i].bitmap);
            g_macro_icons[i].bitmap = NULL;
            g_macro_icons[i].loaded = 0;
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
 * Get macro icon by index
 */
const png_icon_t *icons_get_macro(int index) {
    if (index < 0 || index >= MACRO_ICON_COUNT) {
        return NULL;
    }
    if (!g_macro_icons[index].loaded) {
        return NULL;
    }
    return &g_macro_icons[index];
}

/*
 * Draw a single macro icon to framebuffer
 */
int icons_draw_macro(uint8_t *framebuffer, int fb_width, int fb_height,
                     int icon_index, int x, int y, int invert) {
    const png_icon_t *icon = icons_get_macro(icon_index);
    if (!icon || !icon->bitmap) {
        return -1;
    }
    
    /* Render with same logic as theme_render_face */
    for (int iy = 0; iy < icon->height; iy++) {
        int screen_y = y + iy;
        if (screen_y < 0 || screen_y >= fb_height) continue;
        
        for (int ix = 0; ix < icon->width; ix++) {
            int screen_x = x + ix;
            if (screen_x < 0 || screen_x >= fb_width) continue;
            
            /* Get pixel from icon bitmap (1 = black in source) */
            int src_byte = iy * icon->stride + ix / 8;
            int src_bit = 7 - (ix % 8);
            int pixel = (icon->bitmap[src_byte] >> src_bit) & 1;
            
            /* Invert: PNG has 1=black, framebuffer needs 0=black for e-ink */
            pixel = !pixel;
            
            if (invert) {
                pixel = !pixel;
            }
            
            /* Set pixel in framebuffer */
            int fb_byte = (screen_y * fb_width + screen_x) / 8;
            int fb_bit = 7 - (screen_x % 8);
            
            if (pixel) {
                framebuffer[fb_byte] |= (1 << fb_bit);
            } else {
                framebuffer[fb_byte] &= ~(1 << fb_bit);
            }
        }
    }
    
    return 0;
}

/*
 * Draw a macro icon scaled to specific dimensions
 * Uses nearest-neighbor scaling for crisp e-ink output
 */
int icons_draw_macro_scaled(uint8_t *framebuffer, int fb_width, int fb_height,
                            int icon_index, int x, int y, 
                            int dst_w, int dst_h, int invert) {
    const png_icon_t *icon = icons_get_macro(icon_index);
    if (!icon || !icon->bitmap) {
        return -1;
    }
    
    int src_w = icon->width;
    int src_h = icon->height;
    
    /* Render with nearest-neighbor scaling */
    for (int dy = 0; dy < dst_h; dy++) {
        int screen_y = y + dy;
        if (screen_y < 0 || screen_y >= fb_height) continue;
        
        /* Map destination Y to source Y */
        int src_y = (dy * src_h) / dst_h;
        
        for (int dx = 0; dx < dst_w; dx++) {
            int screen_x = x + dx;
            if (screen_x < 0 || screen_x >= fb_width) continue;
            
            /* Map destination X to source X */
            int src_x = (dx * src_w) / dst_w;
            
            /* Get pixel from icon bitmap */
            int src_byte = src_y * icon->stride + src_x / 8;
            int src_bit = 7 - (src_x % 8);
            int pixel = (icon->bitmap[src_byte] >> src_bit) & 1;
            
            /* Invert for e-ink */
            pixel = !pixel;
            
            if (invert) {
                pixel = !pixel;
            }
            
            /* Set pixel in framebuffer */
            int fb_byte = (screen_y * fb_width + screen_x) / 8;
            int fb_bit = 7 - (screen_x % 8);
            
            if (pixel) {
                framebuffer[fb_byte] |= (1 << fb_bit);
            } else {
                framebuffer[fb_byte] &= ~(1 << fb_bit);
            }
        }
    }
    
    return 0;
}

/*
 * Draw macro icons based on overall macro percentage
 * 
 * Logic:
 *   >= 67% (2/3 full) -> Show 3 icons: 💪🥓🍞
 *   >= 34% (1/3 full) -> Show 2 icons: 💪🥓
 *   >= 10%            -> Show 1 icon:  💪
 *   < 10%             -> Flash 1 icon (if flash_state=1, show; else hide)
 *   0%                -> No icons
 *
 * Icons are bottom-aligned to sit 1-2px above a baseline.
 * Protein icon is scaled down ~15% to match Fat/Carbs height.
 */
void icons_draw_macro_indicator(uint8_t *framebuffer, int fb_width, int fb_height,
                                int x, int y, int macro_percent, int flash_state, int invert) {
    int num_icons = 0;
    int flashing = 0;
    
    if (macro_percent >= 67) {
        num_icons = 3;  /* Full - all 3 icons */
    } else if (macro_percent >= 34) {
        num_icons = 2;  /* 2/3 - 2 icons */
    } else if (macro_percent >= 10) {
        num_icons = 1;  /* Low - 1 icon */
    } else if (macro_percent > 0) {
        num_icons = 1;  /* Critical - flash 1 icon */
        flashing = 1;
    } else {
        num_icons = 0;  /* Empty - no icons */
    }
    
    /* If flashing and flash_state is 0, hide the icon */
    if (flashing && !flash_state) {
        return;  /* Don't draw anything during flash-off phase */
    }
    
    /* Target baseline height (use Fat icon as reference ~15px) */
    int baseline_height = 15;
    int spacing = 2;  /* Gap between icons */
    
    /* Draw the appropriate number of icons */
    int draw_x = x;
    
    /* Order: Protein, Fat, Carbs - all bottom-aligned */
    if (num_icons >= 1) {
        const png_icon_t *icon = icons_get_macro(MACRO_ICON_PROTEIN);
        if (icon && icon->bitmap) {
            /* Scale protein down ~15% (from 21 to ~18 height) */
            int scaled_h = (icon->height * 85) / 100;  /* 85% of original */
            int scaled_w = (icon->width * 85) / 100;
            int icon_y = y + (baseline_height - scaled_h);  /* Bottom align */
            icons_draw_macro_scaled(framebuffer, fb_width, fb_height,
                                   MACRO_ICON_PROTEIN, draw_x, icon_y, 
                                   scaled_w, scaled_h, invert);
            draw_x += scaled_w + spacing;
        }
    }
    
    if (num_icons >= 2) {
        const png_icon_t *icon = icons_get_macro(MACRO_ICON_FAT);
        if (icon && icon->bitmap) {
            int icon_y = y + (baseline_height - icon->height);  /* Bottom align */
            icons_draw_macro(framebuffer, fb_width, fb_height,
                            MACRO_ICON_FAT, draw_x, icon_y, invert);
            draw_x += icon->width + spacing;
        }
    }
    
    if (num_icons >= 3) {
        const png_icon_t *icon = icons_get_macro(MACRO_ICON_CARBS);
        if (icon && icon->bitmap) {
            int icon_y = y + (baseline_height - icon->height);  /* Bottom align */
            icons_draw_macro(framebuffer, fb_width, fb_height,
                            MACRO_ICON_CARBS, draw_x, icon_y, invert);
        }
    }
}
