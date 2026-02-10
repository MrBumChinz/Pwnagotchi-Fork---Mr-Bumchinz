/*
 * PwnaUI - Renderer Header
 * Text, icons, and layout rendering engine
 */

#ifndef PWNAUI_RENDERER_H
#define PWNAUI_RENDERER_H

#include <stdint.h>
#include <stddef.h>
#include "themes.h"

/* Maximum display dimensions */
#define DISPLAY_MAX_WIDTH   400
#define DISPLAY_MAX_HEIGHT  300

/* UI State structure - Static allocation */
typedef struct {
    /* Widget values - fixed size buffers */
    char face[64];           /* Legacy face string - DEPRECATED */
    face_state_t face_enum;  /* Direct PNG face state - USE THIS */
    char status[256];        /* Status text (multi-line) */
    char channel[16];        /* Channel number */
    char aps[32];            /* APS count string (current visible) */
    char uptime[32];         /* Uptime string */
    char shakes[32];         /* Handshakes string (legacy, unused) */
    char mode[16];           /* Mode (AUTO/MANU/AI) */

    /* Bottom bar stats - totals from persisted data */
    int pwds;                /* Passwords cracked */
    int fhs;                 /* Full handshakes captured */
    int phs;                 /* Partial handshakes captured */
    int tcaps;                /* Total pcap captures */
    char name[64];           /* Pwnagotchi name */
    char friend_name[128];   /* Friend name/info */
    char friend_face[64];    /* Friend face */

    /* Plugin widgets */
    char bluetooth[16];      /* BT status */
    char memtemp_header[32]; /* Memtemp header */
    char memtemp_data[32];   /* Memtemp data */
    char gps[24];            /* GPS status */
    char battery[16];        /* Battery status */

    /* PwnHub Stats widgets (pet system) */
    int pwnhub_enabled;      /* 0 = hide, 1 = show PwnHub stats */
    int pwnhub_protein;      /* Protein macro (0-50) */
    int pwnhub_fat;          /* Fat macro (0-50) */
    int pwnhub_carbs;        /* Carbs macro (0-50) */
    int pwnhub_xp_percent;   /* XP progress to next level (0-100) */
    int pwnhub_level;        /* Current level */
    int pwnhub_wins;         /* Battle wins */
    int pwnhub_battles;      /* Total battles */
    char pwnhub_title[24];   /* Stage title */

    /* Display settings */
    int invert;              /* Invert colors */
    int width;               /* Display width */
    int height;              /* Display height */

    /* Layout positions - set by layout config */
    int face_x, face_y;
    int status_x, status_y;
    int channel_x, channel_y;
    int aps_x, aps_y;
    int uptime_x, uptime_y;
    int shakes_x, shakes_y;
    int mode_x, mode_y;
    int name_x, name_y;
    int friend_x, friend_y;
    int line1_x1, line1_y1, line1_x2, line1_y2;
    int line2_x1, line2_y1, line2_x2, line2_y2;

    /* Plugin widget positions */
    int bluetooth_x, bluetooth_y;
    int memtemp_x, memtemp_y;
    int memtemp_data_x, memtemp_data_y;
    int gps_x, gps_y;
    int battery_x, battery_y;

    /* PwnHub Stats positions */
    int pwnhub_x, pwnhub_y;

} ui_state_t;

/* Font IDs */
#define FONT_SMALL      0
#define FONT_MEDIUM     1
#define FONT_BOLD       2
#define FONT_BOLD_SMALL 3
#define FONT_HUGE       4

/* Frame buffer utilities */
void fb_set_pixel(uint8_t *fb, int width, int x, int y, int color);
int fb_get_pixel(uint8_t *fb, int width, int x, int y);
void fb_fill_rect(uint8_t *fb, int width, int height, int x, int y, int w, int h, int color);
void fb_clear(uint8_t *fb, int width, int height);
void fb_invert(uint8_t *fb, int width, int height);

/* Text rendering */
int renderer_draw_char(uint8_t *fb, int fb_width, int fb_height, int x, int y, char c, int font_id, int color);
int renderer_draw_string(uint8_t *fb, int fb_width, int fb_height, int x, int y, const char *text, int font_id, int color);
int renderer_get_string_width(const char *text, int font_id);
int renderer_get_font_height(int font_id);

/* Icon rendering */
void renderer_draw_icon(uint8_t *fb, int fb_width, int fb_height, int x, int y, const char *icon_name, int color);

/* Line rendering */
void renderer_draw_line_fb(uint8_t *fb, int fb_width, int fb_height, int x1, int y1, int x2, int y2);

/* Initialization */
int renderer_init(void);
void renderer_cleanup(void);

/* Font loading from custom path */
int renderer_load_fonts(const char *font_dir);

/* Glyph cache info */
int renderer_get_cache_stats(int *hits, int *misses, int *loaded);

/* High-level UI rendering */
void renderer_clear(ui_state_t *state, uint8_t *framebuffer);
void renderer_render_ui(ui_state_t *state, uint8_t *framebuffer);
void renderer_draw_line(ui_state_t *state, uint8_t *framebuffer,
                        int x1, int y1, int x2, int y2);
void renderer_draw_rect(ui_state_t *state, uint8_t *framebuffer,
                        int x, int y, int w, int h, int filled);
void renderer_draw_text(ui_state_t *state, uint8_t *framebuffer,
                        int x, int y, const char *text, int font);

#endif /* PWNAUI_RENDERER_H */
