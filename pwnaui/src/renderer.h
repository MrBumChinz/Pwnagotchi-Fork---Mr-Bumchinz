/*
 * PwnaUI - Renderer Header
 * Text, icons, and layout rendering engine
 */

#ifndef PWNAUI_RENDERER_H
#define PWNAUI_RENDERER_H

#include <stdint.h>
#include <stddef.h>

/* Maximum display dimensions */
#define DISPLAY_MAX_WIDTH   400
#define DISPLAY_MAX_HEIGHT  300

/* UI State structure - Static allocation */
typedef struct {
    /* Widget values - fixed size buffers */
    char face[64];           /* Face emoticon string */
    char status[256];        /* Status text (multi-line) */
    char channel[16];        /* Channel number */
    char aps[32];            /* APS count string */
    char uptime[32];         /* Uptime string */
    char shakes[32];         /* Handshakes string */
    char mode[16];           /* Mode (AUTO/MANU/AI) */
    char name[64];           /* Pwnagotchi name */
    char friend_name[128];   /* Friend name/info */
    char friend_face[64];    /* Friend face */
    
    /* Plugin widgets */
    char bluetooth[16];      /* BT status: "BT ✓" or "BT ✗" */
    char memtemp_header[32]; /* Memtemp header: "mem cpu tmp" */
    char memtemp_data[32];   /* Memtemp data: " 45%  12%  52C" */
    char gps[24];            /* GPS status: "GPS ✓", "GPS ✗", "GPS S", "GPS ?" */
    char battery[16];        /* Battery: "85%" or "85%+" (charging) */
    
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
    int memtemp_x, memtemp_y;      /* For memtemp header */
    int memtemp_data_x, memtemp_data_y;  /* For memtemp data */
    int gps_x, gps_y;             /* GPS status position */
    int battery_x, battery_y;     /* Battery status position */
    
} ui_state_t;

/* Font IDs */
#define FONT_SMALL      0
#define FONT_MEDIUM     1
#define FONT_BOLD       2
#define FONT_BOLD_SMALL 3
#define FONT_HUGE       4

/*
 * Initialize the renderer
 * Returns 0 on success, -1 on error
 */
int renderer_init(void);

/*
 * Cleanup renderer resources
 */
void renderer_cleanup(void);

/*
 * Set display layout by name
 */
void renderer_set_layout(const char *layout_name);

/*
 * Get display width/height based on current layout
 */
int renderer_get_width(void);
int renderer_get_height(void);

/*
 * Set a single pixel in the framebuffer
 */
void renderer_set_pixel(uint8_t *framebuffer, int width, int x, int y, int color);

/*
 * Clear the framebuffer
 */
void renderer_clear_fb(uint8_t *framebuffer, int width, int height, int color);

/*
 * Draw a line
 */
void renderer_draw_line_simple(uint8_t *framebuffer, int width, int height,
                               int x1, int y1, int x2, int y2, int color);

/*
 * Draw a rectangle (outline or filled)
 */
void renderer_draw_rect_simple(uint8_t *framebuffer, int width, int height,
                               int x, int y, int w, int h, int color, int filled);

/*
 * Draw text at position (simplified API)
 */
void renderer_draw_text_simple(uint8_t *framebuffer, int width, int height,
                               int x, int y, const char *text, int font_id, int color);

/*
 * UI state-based APIs (for full Pwnagotchi rendering)
 */
void renderer_clear(ui_state_t *state, uint8_t *framebuffer);
void renderer_render_ui(ui_state_t *state, uint8_t *framebuffer);
void renderer_draw_line(ui_state_t *state, uint8_t *framebuffer, 
                        int x1, int y1, int x2, int y2);
void renderer_draw_rect(ui_state_t *state, uint8_t *framebuffer, 
                        int x, int y, int w, int h, int filled);
void renderer_draw_text(ui_state_t *state, uint8_t *framebuffer, 
                        int x, int y, const char *text, int font_id);

#endif /* PWNAUI_RENDERER_H */
