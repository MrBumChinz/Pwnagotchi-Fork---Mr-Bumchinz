/*
 * PwnaUI - Renderer Implementation
 * Text, icons, and layout rendering engine
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "renderer.h"
#include "font.h"
#include "icons.h"
#include "display.h"
#include "faces.h"
#include "themes.h"

/* Current display dimensions */
static int g_display_width = 250;
static int g_display_height = 122;

/* Layout presets matching Pwnagotchi display configurations */
typedef struct {
    const char *name;
    int width;
    int height;
    /* Widget positions */
    int face_x, face_y;
    int name_x, name_y;
    int channel_x, channel_y;
    int aps_x, aps_y;
    int uptime_x, uptime_y;
    int line1_x1, line1_y1, line1_x2, line1_y2;
    int line2_x1, line2_y1, line2_x2, line2_y2;
    int friend_x, friend_y;
    int shakes_x, shakes_y;
    int mode_x, mode_y;
    int status_x, status_y;
    int status_max;  /* Max chars per status line */
    /* Plugin widget positions */
    int bluetooth_x, bluetooth_y;
    int gps_x, gps_y;
    int memtemp_x, memtemp_y;
    int memtemp_data_x, memtemp_data_y;
    int battery_x, battery_y;
} layout_t;

/* Predefined layouts matching Pwnagotchi's hardware drivers */
static const layout_t g_layouts[] = {
    /* Waveshare 2.13" V4 - 250x122 - USER'S DISPLAY */
    {
        .name = "waveshare2in13_v4",
        .width = 250, .height = 122,
        .face_x = 2, .face_y = 30,
        .name_x = 5, .name_y = 20,
        .channel_x = 2, .channel_y = 0,
        .aps_x = 35, .aps_y = 0,
        .uptime_x = 175, .uptime_y = 0,
        .line1_x1 = 1, .line1_y1 = 14, .line1_x2 = 249, .line1_y2 = 14,
        .line2_x1 = 1, .line2_y1 = 108, .line2_x2 = 249, .line2_y2 = 108,
        .friend_x = 40, .friend_y = 94,
        .shakes_x = 2, .shakes_y = 111,
        .mode_x = 200, .mode_y = 111,
        .status_x = 125, .status_y = 20,
        .status_max = 20,
        .bluetooth_x = 75, .bluetooth_y = 0,
        .gps_x = 147, .gps_y = 0,
        .memtemp_x = 178, .memtemp_y = 85,
        .memtemp_data_x = 178, .memtemp_data_y = 95,
        .battery_x = 96, .battery_y = 0
    },
    /* Waveshare 2.13" V3 - 250x122 */
    {
        .name = "waveshare2in13_v3",
        .width = 250, .height = 122,
        .face_x = 5, .face_y = 40,
        .name_x = 5, .name_y = 20,
        .channel_x = 0, .channel_y = 0,
        .aps_x = 28, .aps_y = 0,
        .uptime_x = 185, .uptime_y = 0,
        .line1_x1 = 0, .line1_y1 = 14, .line1_x2 = 250, .line1_y2 = 14,
        .line2_x1 = 0, .line2_y1 = 108, .line2_x2 = 250, .line2_y2 = 108,
        .friend_x = 40, .friend_y = 94,
        .shakes_x = 0, .shakes_y = 109,
        .mode_x = 145, .mode_y = 109,
        .status_x = 125, .status_y = 20,
        .status_max = 20,
        .bluetooth_x = 178, .bluetooth_y = 109,
        .gps_x = 213, .gps_y = 109,
        .memtemp_x = 80, .memtemp_y = 109,
        .memtemp_data_x = 80, .memtemp_data_y = 109,
        .battery_x = 220, .battery_y = 0
    },
    /* Waveshare 2.7" - 264x176 */
    {
        .name = "waveshare2in7",
        .width = 264, .height = 176,
        .face_x = 0, .face_y = 50,
        .name_x = 5, .name_y = 25,
        .channel_x = 0, .channel_y = 0,
        .aps_x = 40, .aps_y = 0,
        .uptime_x = 195, .uptime_y = 0,
        .line1_x1 = 0, .line1_y1 = 18, .line1_x2 = 264, .line1_y2 = 18,
        .line2_x1 = 0, .line2_y1 = 158, .line2_x2 = 264, .line2_y2 = 158,
        .friend_x = 50, .friend_y = 140,
        .shakes_x = 0, .shakes_y = 159,
        .mode_x = 155, .mode_y = 159,
        .status_x = 130, .status_y = 25,
        .status_max = 22,
        .bluetooth_x = 190, .bluetooth_y = 159,
        .gps_x = 227, .gps_y = 159,
        .memtemp_x = 100, .memtemp_y = 159,
        .memtemp_data_x = 100, .memtemp_data_y = 159,
        .battery_x = 230, .battery_y = 0
    },
    /* Waveshare 1.54" - 200x200 */
    {
        .name = "waveshare1in54",
        .width = 200, .height = 200,
        .face_x = 0, .face_y = 60,
        .name_x = 5, .name_y = 20,
        .channel_x = 0, .channel_y = 0,
        .aps_x = 35, .aps_y = 0,
        .uptime_x = 140, .uptime_y = 0,
        .line1_x1 = 0, .line1_y1 = 14, .line1_x2 = 200, .line1_y2 = 14,
        .line2_x1 = 0, .line2_y1 = 180, .line2_x2 = 200, .line2_y2 = 180,
        .friend_x = 40, .friend_y = 160,
        .shakes_x = 0, .shakes_y = 183,
        .mode_x = 110, .mode_y = 183,
        .status_x = 100, .status_y = 20,
        .status_max = 16,
        .bluetooth_x = 140, .bluetooth_y = 183,
        .gps_x = 170, .gps_y = 183,
        .memtemp_x = 60, .memtemp_y = 183,
        .memtemp_data_x = 60, .memtemp_data_y = 183,
        .battery_x = 170, .battery_y = 0
    },
    /* Inky pHAT - 212x104 */
    {
        .name = "inky",
        .width = 212, .height = 104,
        .face_x = 0, .face_y = 32,
        .name_x = 5, .name_y = 16,
        .channel_x = 0, .channel_y = 0,
        .aps_x = 25, .aps_y = 0,
        .uptime_x = 155, .uptime_y = 0,
        .line1_x1 = 0, .line1_y1 = 12, .line1_x2 = 212, .line1_y2 = 12,
        .line2_x1 = 0, .line2_y1 = 90, .line2_x2 = 212, .line2_y2 = 90,
        .friend_x = 35, .friend_y = 76,
        .shakes_x = 0, .shakes_y = 92,
        .mode_x = 120, .mode_y = 92,
        .status_x = 110, .status_y = 16,
        .status_max = 17,
        .bluetooth_x = 150, .bluetooth_y = 92,
        .gps_x = 180, .gps_y = 92,
        .memtemp_x = 70, .memtemp_y = 92,
        .memtemp_data_x = 70, .memtemp_data_y = 92,
        .battery_x = 185, .battery_y = 0
    },
    /* Framebuffer/Dummy - 250x122 (default) */
    {
        .name = "default",
        .width = 250, .height = 122,
        .face_x = 0, .face_y = 40,
        .name_x = 5, .name_y = 20,
        .channel_x = 0, .channel_y = 0,
        .aps_x = 28, .aps_y = 0,
        .uptime_x = 185, .uptime_y = 0,
        .line1_x1 = 0, .line1_y1 = 14, .line1_x2 = 250, .line1_y2 = 14,
        .line2_x1 = 0, .line2_y1 = 108, .line2_x2 = 250, .line2_y2 = 108,
        .friend_x = 40, .friend_y = 94,
        .shakes_x = 0, .shakes_y = 109,
        .mode_x = 145, .mode_y = 109,
        .status_x = 125, .status_y = 20,
        .status_max = 20,
        .bluetooth_x = 178, .bluetooth_y = 109,
        .gps_x = 213, .gps_y = 109,
        .memtemp_x = 80, .memtemp_y = 109,
        .memtemp_data_x = 80, .memtemp_data_y = 109,
        .battery_x = 220, .battery_y = 0
    },
};

#define NUM_LAYOUTS (sizeof(g_layouts) / sizeof(g_layouts[0]))

static const layout_t *g_current_layout = &g_layouts[0];

/*
 * Initialize the renderer
 */
int renderer_init(void) {
    /* Set default layout */
    g_current_layout = &g_layouts[NUM_LAYOUTS - 1];  /* Default */
    g_display_width = g_current_layout->width;
    g_display_height = g_current_layout->height;
    return 0;
}

/*
 * Cleanup renderer resources
 */
void renderer_cleanup(void) {
    /* Nothing to cleanup with static allocation */
}

/*
 * Set display layout by name
 */
void renderer_set_layout(const char *layout_name) {
    for (size_t i = 0; i < NUM_LAYOUTS; i++) {
        if (strncmp(g_layouts[i].name, layout_name, strlen(g_layouts[i].name)) == 0) {
            g_current_layout = &g_layouts[i];
            g_display_width = g_current_layout->width;
            g_display_height = g_current_layout->height;
            return;
        }
    }
    /* Layout not found, keep current */
}

/*
 * Get display dimensions
 */
int renderer_get_width(void) {
    return g_display_width;
}

int renderer_get_height(void) {
    return g_display_height;
}

/*
 * Set a single pixel in the framebuffer
 * Framebuffer is 1-bit packed, MSB first
 */
void renderer_set_pixel(uint8_t *framebuffer, int width, int x, int y, int color) {
    if (x < 0 || x >= width || y < 0 || y >= g_display_height) {
        return;  /* Out of bounds */
    }
    
    int byte_idx = (y * width + x) / 8;
    int bit_idx = 7 - (x % 8);  /* MSB first */
    
    if (color) {
        framebuffer[byte_idx] |= (1 << bit_idx);   /* Set bit (white/off) */
    } else {
        framebuffer[byte_idx] &= ~(1 << bit_idx);  /* Clear bit (black/on) */
    }
}

/*
 * Get a pixel from framebuffer
 */
static int get_pixel(uint8_t *framebuffer, int width, int x, int y) {
    if (x < 0 || x >= width || y < 0 || y >= g_display_height) {
        return 1;  /* White for out of bounds */
    }
    
    int byte_idx = (y * width + x) / 8;
    int bit_idx = 7 - (x % 8);
    
    return (framebuffer[byte_idx] >> bit_idx) & 1;
}

/*
 * Clear the framebuffer
 */
void renderer_clear(ui_state_t *state, uint8_t *framebuffer) {
    int fb_size = (g_display_width * g_display_height + 7) / 8;
    
    if (state->invert) {
        memset(framebuffer, 0x00, fb_size);  /* Black background */
    } else {
        memset(framebuffer, 0xFF, fb_size);  /* White background */
    }
}

/*
 * Draw a horizontal line (optimized)
 */
static void draw_hline(uint8_t *framebuffer, int x1, int x2, int y, int color) {
    if (y < 0 || y >= g_display_height) return;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (x1 < 0) x1 = 0;
    if (x2 >= g_display_width) x2 = g_display_width - 1;
    
    for (int x = x1; x <= x2; x++) {
        renderer_set_pixel(framebuffer, g_display_width, x, y, color);
    }
}

/*
 * Draw a vertical line (optimized)
 */
static void draw_vline(uint8_t *framebuffer, int x, int y1, int y2, int color) {
    if (x < 0 || x >= g_display_width) return;
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    if (y1 < 0) y1 = 0;
    if (y2 >= g_display_height) y2 = g_display_height - 1;
    
    for (int y = y1; y <= y2; y++) {
        renderer_set_pixel(framebuffer, g_display_width, x, y, color);
    }
}

/*
 * Draw a line using Bresenham's algorithm
 */
void renderer_draw_line(ui_state_t *state, uint8_t *framebuffer,
                        int x1, int y1, int x2, int y2) {
    int color = state->invert ? 1 : 0;  /* Black line */
    
    /* Optimize for horizontal/vertical lines */
    if (y1 == y2) {
        draw_hline(framebuffer, x1, x2, y1, color);
        return;
    }
    if (x1 == x2) {
        draw_vline(framebuffer, x1, y1, y2, color);
        return;
    }
    
    /* Bresenham's line algorithm */
    int dx = abs(x2 - x1);
    int dy = abs(y2 - y1);
    int sx = (x1 < x2) ? 1 : -1;
    int sy = (y1 < y2) ? 1 : -1;
    int err = dx - dy;
    
    while (1) {
        renderer_set_pixel(framebuffer, g_display_width, x1, y1, color);
        
        if (x1 == x2 && y1 == y2) break;
        
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x1 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y1 += sy;
        }
    }
}

/*
 * Draw a rectangle
 */
void renderer_draw_rect(ui_state_t *state, uint8_t *framebuffer,
                        int x, int y, int w, int h, int filled) {
    int color = state->invert ? 1 : 0;
    
    if (filled) {
        for (int row = y; row < y + h; row++) {
            draw_hline(framebuffer, x, x + w - 1, row, color);
        }
    } else {
        draw_hline(framebuffer, x, x + w - 1, y, color);
        draw_hline(framebuffer, x, x + w - 1, y + h - 1, color);
        draw_vline(framebuffer, x, y, y + h - 1, color);
        draw_vline(framebuffer, x + w - 1, y, y + h - 1, color);
    }
}

/*
 * Draw text at position using specified font
 */
void renderer_draw_text(ui_state_t *state, uint8_t *framebuffer,
                        int x, int y, const char *text, int font_id) {
    if (!text || !text[0]) return;
    
    int color = state->invert ? 1 : 0;  /* Black text */
    int bg_color = state->invert ? 0 : 1;  /* White background */
    
    /* Scale factor for FONT_HUGE (faces) - use 1.5x (3 pixels for every 2) */
    int use_15x_scale = (font_id == FONT_HUGE) ? 1 : 0;
    int scale = use_15x_scale ? 1 : 1;  /* Base scale, 1.5x handled specially */
    
    const font_t *font = font_get(font_id);
    if (!font) {
        font = font_get(FONT_MEDIUM);  /* Fallback */
    }
    
    int cursor_x = x;
    int cursor_y = y;
    
    /* Handle UTF-8 text (for face emoticons) */
    const uint8_t *p = (const uint8_t *)text;
    while (*p) {
        uint32_t codepoint;
        int bytes;
        
        /* Decode UTF-8 */
        if ((*p & 0x80) == 0) {
            /* ASCII */
            codepoint = *p;
            bytes = 1;
        } else if ((*p & 0xE0) == 0xC0) {
            /* 2-byte UTF-8 */
            codepoint = (*p & 0x1F) << 6;
            if (p[1]) {
                codepoint |= (p[1] & 0x3F);
                bytes = 2;
            } else {
                bytes = 1;
                codepoint = '?';
            }
        } else if ((*p & 0xF0) == 0xE0) {
            /* 3-byte UTF-8 */
            codepoint = (*p & 0x0F) << 12;
            if (p[1] && p[2]) {
                codepoint |= (p[1] & 0x3F) << 6;
                codepoint |= (p[2] & 0x3F);
                bytes = 3;
            } else {
                bytes = 1;
                codepoint = '?';
            }
        } else if ((*p & 0xF8) == 0xF0) {
            /* 4-byte UTF-8 */
            codepoint = (*p & 0x07) << 18;
            if (p[1] && p[2] && p[3]) {
                codepoint |= (p[1] & 0x3F) << 12;
                codepoint |= (p[2] & 0x3F) << 6;
                codepoint |= (p[3] & 0x3F);
                bytes = 4;
            } else {
                bytes = 1;
                codepoint = '?';
            }
        } else {
            /* Invalid UTF-8, skip */
            codepoint = '?';
            bytes = 1;
        }
        
        p += bytes;
        
        /* Handle newline */
        if (codepoint == '\n') {
            cursor_x = x;
            cursor_y += font->height + 2;
            continue;
        }
        
        /* Get glyph */
        const glyph_t *glyph = font_get_glyph_from_font(font, codepoint);
        if (!glyph) {
            /* Try fallback character */
            glyph = font_get_glyph_from_font(font, '?');
            if (!glyph) {
                cursor_x += font->width;
                continue;
            }
        }
        
        /* Draw glyph with scaling and y_offset */
        int glyph_y = cursor_y + (glyph->y_offset * (use_15x_scale ? 2 : scale));
        
        if (use_15x_scale) {
            /* 1.5x scaling: 3 output pixels for every 2 source pixels */
            for (int gy = 0; gy < glyph->height; gy++) {
                int out_y = glyph_y + (gy * 3) / 2;
                int y_extra = (gy % 2 == 1);  /* Draw extra row on odd rows */
                
                for (int gx = 0; gx < glyph->width; gx++) {
                    int byte_idx = gy * ((glyph->width + 7) / 8) + gx / 8;
                    int bit_idx = 7 - (gx % 8);
                    int pixel = (glyph->bitmap[byte_idx] >> bit_idx) & 1;
                    
                    if (pixel) {
                        int out_x = cursor_x + (gx * 3) / 2;
                        int x_extra = (gx % 2 == 1);
                        
                        /* Draw base pixel */
                        renderer_set_pixel(framebuffer, g_display_width, out_x, out_y, color);
                        /* Draw extra column on odd columns */
                        if (x_extra) {
                            renderer_set_pixel(framebuffer, g_display_width, out_x + 1, out_y, color);
                        }
                        /* Draw extra row on odd rows */
                        if (y_extra) {
                            renderer_set_pixel(framebuffer, g_display_width, out_x, out_y + 1, color);
                            if (x_extra) {
                                renderer_set_pixel(framebuffer, g_display_width, out_x + 1, out_y + 1, color);
                            }
                        }
                    }
                }
            }
            cursor_x += (glyph->advance * 3) / 2;
        } else {
            /* Normal 1x scaling */
            for (int gy = 0; gy < glyph->height; gy++) {
                for (int gx = 0; gx < glyph->width; gx++) {
                    int byte_idx = gy * ((glyph->width + 7) / 8) + gx / 8;
                    int bit_idx = 7 - (gx % 8);
                    
                    int pixel = (glyph->bitmap[byte_idx] >> bit_idx) & 1;
                    if (pixel) {
                        renderer_set_pixel(framebuffer, g_display_width,
                                          cursor_x + gx, glyph_y + gy, color);
                    }
                }
            }
            cursor_x += glyph->advance;
        }
    }
}

/*
 * Draw labeled value (e.g., "CH: 6")
 */
static void draw_labeled_value(ui_state_t *state, uint8_t *framebuffer,
                               int x, int y, const char *label, const char *value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s:%s", label, value);
    renderer_draw_text(state, framebuffer, x, y, buf, FONT_BOLD);
}

/*
 * Render the full Pwnagotchi UI to the framebuffer
 */
void renderer_render_ui(ui_state_t *state, uint8_t *framebuffer) {
    const layout_t *L = g_current_layout;
    
    /* Clear first */
    renderer_clear(state, framebuffer);
    
    /* Draw separator lines */
    renderer_draw_line(state, framebuffer, 
                       L->line1_x1, L->line1_y1, L->line1_x2, L->line1_y2);
    renderer_draw_line(state, framebuffer,
                       L->line2_x1, L->line2_y1, L->line2_x2, L->line2_y2);
    
    /* Top row: CH | APS | UPTIME */
    draw_labeled_value(state, framebuffer, L->channel_x, L->channel_y, "CH", state->channel);
    draw_labeled_value(state, framebuffer, L->aps_x, L->aps_y, "APS", state->aps);
    /* Uptime rendered directly - value already formatted as DD:HH:MM:SS */
    renderer_draw_text(state, framebuffer, L->uptime_x, L->uptime_y, state->uptime, FONT_BOLD);
    
    /* Main area: Name and Face */
    renderer_draw_text(state, framebuffer, L->name_x, L->name_y, state->name, FONT_BOLD);
    
    /* Face - ALWAYS use PNG theme (no ASCII fallback) */
    theme_render_face_animated(framebuffer, g_display_width, g_display_height,
                                L->face_x, L->face_y, state->face, state->invert);
    
    /* Status text (right side of face) - with word wrapping */
    {
        int status_max_width = 250 - L->status_x - 5;  /* Right margin */
        int line_height = 10;
        int y = L->status_y;
        char status_copy[256];
        strncpy(status_copy, state->status, sizeof(status_copy) - 1);
        status_copy[sizeof(status_copy) - 1] = '\0';
        
        char *line = status_copy;
        char *space;
        int max_chars = status_max_width / 6;  /* ~6 pixels per char */
        
        while (*line && y < L->line2_y1 - 10) {
            /* Find wrap point */
            if (strlen(line) > (size_t)max_chars) {
                space = line + max_chars;
                while (space > line && *space != ' ') space--;
                if (space == line) space = line + max_chars;
                char saved = *space;
                *space = '\0';
                renderer_draw_text(state, framebuffer, L->status_x, y, line, FONT_MEDIUM);
                *space = saved;
                line = (*space == ' ') ? space + 1 : space;
            } else {
                renderer_draw_text(state, framebuffer, L->status_x, y, line, FONT_MEDIUM);
                break;
            }
            y += line_height;
        }
    }
    
    /* Friend area (if set) */
    if (state->friend_name[0]) {
        renderer_draw_text(state, framebuffer, L->friend_x, L->friend_y, 
                          state->friend_name, FONT_BOLD_SMALL);
    }
    
    /* Bottom row: PWDS:0 FHS:0 PHS:0 TAPS:0 Auto/Manual */
    char bottom_stats[64];
    snprintf(bottom_stats, sizeof(bottom_stats), "PWDS:%d FHS:%d PHS:%d TCAPS:%d",
             state->pwds, state->fhs, state->phs, state->tcaps);
    renderer_draw_text(state, framebuffer, L->shakes_x, L->shakes_y, bottom_stats, FONT_BOLD);
    
    /* Mode: "Auto Mode" or "Manual Mode" - RIGHT ALIGNED with 2px buffer from edge */
    int mode_width = font_text_width(state->mode, FONT_BOLD);
    int mode_x = g_display_width - mode_width - 2;  /* 2px buffer from right edge */
    renderer_draw_text(state, framebuffer, mode_x, L->mode_y, state->mode, FONT_BOLD);
    
    /* BT-Tether status - "BT ✓" or "BT ✗" */
    if (state->bluetooth[0]) {
        renderer_draw_text(state, framebuffer, L->bluetooth_x, L->bluetooth_y, state->bluetooth, FONT_BOLD);
    }
    
    /* GPS status - "GPS ✓" or "GPS ✗" or "GPS S" */
    if (state->gps[0]) {
        renderer_draw_text(state, framebuffer, L->gps_x, L->gps_y, state->gps, FONT_BOLD);
    }
    
    /* Memtemp - CPU/mem/temp from native plugin
     * Header: "mem cpu tmp" at memtemp_x, memtemp_y
     * Data:   " 42%  78%  48C" at memtemp_data_x, memtemp_data_y
     */
    if (state->memtemp_header[0]) {
        renderer_draw_text(state, framebuffer, L->memtemp_x, L->memtemp_y, state->memtemp_header, FONT_SMALL);
    }
    if (state->memtemp_data[0]) {
        renderer_draw_text(state, framebuffer, L->memtemp_data_x, L->memtemp_data_y, state->memtemp_data, FONT_SMALL);
    }
    
    /* Battery status - "85%" or "85%+" (charging) in top bar */
    if (state->battery[0]) {
        renderer_draw_text(state, framebuffer, L->battery_x, L->battery_y, state->battery, FONT_BOLD);
    }
    
    /* PwnHub Stats display (pet system) - renders above memtemp if enabled */
    if (state->pwnhub_enabled) {
        /* Layout:
         * RIGHT SIDE (above memtemp):
         *   Row 1: "XP:91% [====]"
         *   Row 2: "Lvl:8      W:0/0"
         *   Row 3: "mem cpu tmp" (existing)
         * LEFT SIDE (next to memtemp):
         *   Macro icons (1-3 based on fill %)
         */
        char buf[64];
        
        /* === RIGHT SIDE: XP and Level info === */
        int right_x = 130;  /* Tunable: right side position (moved 5px left) */
        int right_y = L->memtemp_y - 20;  /* 20px above memtemp header */
        
        /* Row 1: XP percentage + graphical bar */
        snprintf(buf, sizeof(buf), "XP:%d%%", state->pwnhub_xp_percent);
        renderer_draw_text(state, framebuffer, right_x, right_y, buf, FONT_SMALL);
        
        /* Graphical XP bar: |████░░| style */
        /* Position bar with right edge 2px from display edge */
        int bar_width = 80;   /* Total bar width including border */
        int bar_height = 7;   /* Bar height */
        int bar_x = g_display_width - bar_width - 2;  /* 2px from right edge */
        int bar_y = right_y;  /* Vertically align with text */
        
        /* Draw border (unfilled rectangle) - black pixels on white background */
        /* Color 0 = black (visible), 1 = white (background) */
        int draw_color = state->invert ? 1 : 0;  /* Black pixels */
        renderer_draw_rect_simple(framebuffer, g_display_width, g_display_height,
                                  bar_x, bar_y, bar_width, bar_height, draw_color, 0);
        
        /* Fill based on percentage (inside the border) */
        int inner_width = bar_width - 2;  /* Leave 1px border on each side */
        int fill_width = (inner_width * state->pwnhub_xp_percent) / 100;
        if (fill_width > 0) {
            renderer_draw_rect_simple(framebuffer, g_display_width, g_display_height,
                                      bar_x + 1, bar_y + 1, fill_width, bar_height - 2, 
                                      draw_color, 1);
        }
        
        /* Row 2: Level, Title, and Wins */
        right_y += 10;
        snprintf(buf, sizeof(buf), "Lvl:%d %s W:%d/%d",
                 state->pwnhub_level, state->pwnhub_title, state->pwnhub_wins, state->pwnhub_battles);
        renderer_draw_text(state, framebuffer, right_x, right_y, buf, FONT_SMALL);
        
        /* === MACRO ICONS: Just left of memtemp === */
        /* Calculate combined macro percentage (0-100) as hunger indicator */
        /* 3 icons = well-fed (>=67%), 2 icons = okay (>=34%), 1 icon = hungry (>=10%) */
        /* Each macro is 0-50, total max is 150, so percentage = (total / 150) * 100 */
        int total_macros = state->pwnhub_protein + state->pwnhub_fat + state->pwnhub_carbs;
        int macro_percent = (total_macros * 100) / 150;  /* Max 50+50+50 = 150 */
        
        /* Flash state: toggle every ~500ms based on frame counter */
        /* We use a simple static counter since we don't have actual timing here */
        static int flash_counter = 0;
        static int flash_state = 1;
        flash_counter++;
        if (flash_counter >= 8) {  /* ~8 frames = ~500ms at 60fps, adjust as needed */
            flash_counter = 0;
            flash_state = !flash_state;
        }
        
        /* Position macro icons to align with memtemp data row */
        /* memtemp_data is at Y=95, line2 is at Y=108, so ~5px gap */
        /* Icons with baseline_height 15 should start at Y=90 to have bottom at ~105 */
        int macro_x = L->memtemp_x - 85;  /* 178 - 85 = 93 */
        int macro_y = L->memtemp_data_y - 5;  /* Align with memtemp data row */
        
        /* Draw macro indicator (1-3 icons based on fill level) */
        icons_draw_macro_indicator(framebuffer, g_display_width, g_display_height,
                                   macro_x, macro_y, macro_percent, flash_state, state->invert);
    }
}

/*
 * Simplified APIs for testing
 */
void renderer_clear_fb(uint8_t *framebuffer, int width, int height, int color) {
    int fb_size = (width * height + 7) / 8;
    memset(framebuffer, color ? 0x00 : 0xFF, fb_size);
}

void renderer_draw_line_simple(uint8_t *framebuffer, int width, int height,
                               int x1, int y1, int x2, int y2, int color) {
    /* Temporarily set dimensions */
    int old_height = g_display_height;
    g_display_height = height;
    
    /* Simple line implementation */
    if (y1 == y2) {
        /* Horizontal line */
        if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
        for (int x = x1; x <= x2; x++) {
            renderer_set_pixel(framebuffer, width, x, y1, color);
        }
    } else if (x1 == x2) {
        /* Vertical line */
        if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
        for (int y = y1; y <= y2; y++) {
            renderer_set_pixel(framebuffer, width, x1, y, color);
        }
    } else {
        /* Bresenham's algorithm */
        int dx = abs(x2 - x1);
        int dy = abs(y2 - y1);
        int sx = (x1 < x2) ? 1 : -1;
        int sy = (y1 < y2) ? 1 : -1;
        int err = dx - dy;
        
        while (1) {
            renderer_set_pixel(framebuffer, width, x1, y1, color);
            if (x1 == x2 && y1 == y2) break;
            int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x1 += sx; }
            if (e2 < dx) { err += dx; y1 += sy; }
        }
    }
    
    g_display_height = old_height;
}

void renderer_draw_rect_simple(uint8_t *framebuffer, int width, int height,
                               int x, int y, int w, int h, int color, int filled) {
    if (filled) {
        for (int row = y; row < y + h && row < height; row++) {
            for (int col = x; col < x + w && col < width; col++) {
                renderer_set_pixel(framebuffer, width, col, row, color);
            }
        }
    } else {
        /* Top and bottom */
        renderer_draw_line_simple(framebuffer, width, height, x, y, x + w - 1, y, color);
        renderer_draw_line_simple(framebuffer, width, height, x, y + h - 1, x + w - 1, y + h - 1, color);
        /* Left and right */
        renderer_draw_line_simple(framebuffer, width, height, x, y, x, y + h - 1, color);
        renderer_draw_line_simple(framebuffer, width, height, x + w - 1, y, x + w - 1, y + h - 1, color);
    }
}

void renderer_draw_text_simple(uint8_t *framebuffer, int width, int height,
                               int x, int y, const char *text, int font_id, int color) {
    (void)height;  /* Unused, but kept for API consistency */
    if (!text || !text[0]) return;
    
    const font_t *font = font_get(font_id);
    if (!font) font = font_get(FONT_MEDIUM);
    
    int cursor_x = x;
    const uint8_t *p = (const uint8_t *)text;
    
    while (*p) {
        uint32_t codepoint;
        int bytes;
        
        /* Simple ASCII handling */
        if ((*p & 0x80) == 0) {
            codepoint = *p;
            bytes = 1;
        } else {
            codepoint = '?';
            bytes = 1;
            while (bytes < 4 && p[bytes] && (p[bytes] & 0xC0) == 0x80) bytes++;
        }
        
        p += bytes;
        
        if (codepoint == '\n') {
            cursor_x = x;
            y += font->height + 2;
            continue;
        }
        
        const glyph_t *glyph = font_get_glyph_from_font(font, codepoint);
        if (!glyph) {
            cursor_x += font->width;
            continue;
        }
        
        /* Draw glyph */
        for (int gy = 0; gy < glyph->height; gy++) {
            for (int gx = 0; gx < glyph->width; gx++) {
                int byte_idx = gy * ((glyph->width + 7) / 8) + gx / 8;
                int bit_idx = 7 - (gx % 8);
                if ((glyph->bitmap[byte_idx] >> bit_idx) & 1) {
                    renderer_set_pixel(framebuffer, width, cursor_x + gx, y + gy, color);
                }
            }
        }
        
        cursor_x += glyph->advance;
    }
}
