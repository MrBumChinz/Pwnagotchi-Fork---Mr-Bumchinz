/*
 * PwnaUI Theme System
 * Runtime PNG-based face themes with hot-swapping support
 */

#ifndef PWNAUI_THEMES_H
#define PWNAUI_THEMES_H

#include <stdint.h>

/* Default theme directory */
#define THEME_BASE_DIR "/etc/pwnagotchi/custom-faces"
#define THEME_DEFAULT "default"

/* Face dimensions (can vary per theme, but this is the target) */
#define FACE_MAX_WIDTH  128
#define FACE_MAX_HEIGHT 64

/* 
 * Face states - matches Pwnagotchi's face states
 * These map to PNG filenames: HAPPY.png, SAD.png, etc.
 */
typedef enum {
    FACE_LOOK_R = 0,
    FACE_LOOK_L,
    FACE_LOOK_R_HAPPY,
    FACE_LOOK_L_HAPPY,
    FACE_SLEEP,
    FACE_SLEEP2,
    FACE_AWAKE,
    FACE_BORED,
    FACE_INTENSE,
    FACE_COOL,
    FACE_HAPPY,
    FACE_EXCITED,
    FACE_GRATEFUL,
    FACE_MOTIVATED,
    FACE_DEMOTIVATED,
    FACE_SMART,
    FACE_LONELY,
    FACE_SAD,
    FACE_ANGRY,
    FACE_FRIEND,
    FACE_BROKEN,
    FACE_DEBUG,
    FACE_UPLOAD,
    FACE_UPLOAD1,
    FACE_UPLOAD2,
    FACE_STATE_COUNT
} face_state_t;

/* Face state name lookup (for filename matching) */
extern const char *g_face_state_names[FACE_STATE_COUNT];

/*
 * Single face bitmap (loaded from PNG)
 */
typedef struct {
    uint8_t *bitmap;      /* 1-bit packed bitmap data */
    int width;            /* Original image width */
    int height;           /* Original image height */
    int stride;           /* Bytes per row (width/8 rounded up) */
    int loaded;           /* 1 if successfully loaded */
} face_bitmap_t;

/*
 * Theme - collection of face bitmaps
 */
typedef struct {
    char name[64];                          /* Theme name */
    char path[256];                         /* Theme directory path */
    face_bitmap_t faces[FACE_STATE_COUNT];  /* Face bitmaps */
    int face_width;                         /* Common face width (0 = varies) */
    int face_height;                        /* Common face height (0 = varies) */
    int loaded;                             /* 1 if theme loaded */
    int use_lowercase;                      /* 1 if theme uses lowercase filenames */
} theme_t;

/*
 * Theme manager state
 */
typedef struct {
    theme_t *current;           /* Currently active theme */
    theme_t *themes;            /* Array of loaded themes */
    int theme_count;            /* Number of loaded themes */
    int theme_capacity;         /* Allocated capacity */
    char base_dir[256];         /* Base themes directory */
} theme_manager_t;

/* Global theme manager */
extern theme_manager_t g_theme_mgr;

/*
 * Initialize theme system
 * Returns 0 on success, -1 on error
 */
int themes_init(const char *base_dir);

/*
 * Cleanup theme system
 */
void themes_cleanup(void);

/*
 * Load a theme by name
 * Returns pointer to theme or NULL on error
 */
theme_t *theme_load(const char *name);

/*
 * Unload a theme and free resources
 */
void theme_unload(theme_t *theme);

/*
 * Set the active theme
 * Returns 0 on success, -1 on error
 */
int theme_set_active(const char *name);

/*
 * Get list of available themes
 * Returns array of theme names (NULL terminated)
 * Caller must free the array (but not the strings)
 */
char **theme_list_available(int *count);

/*
 * Free theme list returned by theme_list_available
 */
void theme_list_free(char **list);

/*
 * Get face bitmap for current theme
 * Returns NULL if not loaded
 */
face_bitmap_t *theme_get_face(face_state_t state);

/*
 * Map face string to face state
 * Returns FACE_HAPPY as default if not found
 */
face_state_t theme_face_string_to_state(const char *face_str);

/*
 * Render face from current theme to framebuffer
 */
void theme_render_face(uint8_t *framebuffer, int fb_width, int fb_height,
                       int dest_x, int dest_y, face_state_t state, int invert);

/*
 * Render face by string (convenience wrapper)
 */
void theme_render_face_by_string(uint8_t *framebuffer, int fb_width, int fb_height,
                                 int dest_x, int dest_y, const char *face_str, int invert);

/*
 * Check if themes are enabled (vs fallback to text)
 */
int themes_enabled(void);

/*
 * Enable/disable theme rendering
 */
void themes_set_enabled(int enabled);

/*
 * Disable themes (convenience wrapper for themes_set_enabled(0))
 */
void themes_disable(void);

/*
 * Get count of available themes
 */
int themes_count(void);

/*
 * Get list of theme names (internal static array, do not free)
 * Returns pointer to array of theme name strings
 */
const char **themes_list(void);

/*
 * Get current active theme name
 * Returns NULL or empty string if no theme active
 */
const char *theme_get_active(void);

/*
 * Set theme scale factor (percentage, 20-200)
 * Default is 80 (80% of original size)
 */
void theme_set_scale(int scale_percent);

/*
 * Get current theme scale factor
 */
int theme_get_scale(void);

#endif /* PWNAUI_THEMES_H */
