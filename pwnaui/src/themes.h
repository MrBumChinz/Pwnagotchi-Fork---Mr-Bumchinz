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
 * Face states - matches available PNG files
 * Static expressions + animation frames
 */
typedef enum {
    /* === EXPRESSIONS (static) === */
    FACE_HAPPY = 0,
    FACE_SAD,
    FACE_ANGRY,
    FACE_EXCITED,
    FACE_GRATEFUL,
    FACE_LONELY,
    FACE_COOL,
    FACE_INTENSE,
    FACE_SMART,
    FACE_FRIEND,
    FACE_BROKEN,
    FACE_DEBUG,
    FACE_DEMOTIVATED,
    
    /* === LOOKING ANIMATIONS === */
    FACE_LOOK_L,
    FACE_LOOK_R,
    FACE_LOOK_L_HAPPY,
    FACE_LOOK_R_HAPPY,
    
    /* === SLEEP ANIMATIONS (cycle 1->2->3->4->3->2->1) === */
    FACE_SLEEP1,
    FACE_SLEEP2,
    FACE_SLEEP3,
    FACE_SLEEP4,
    
    /* === UPLOAD/DOWNLOAD ANIMATIONS (binary eyes) === */
    FACE_UPLOAD_00,   /* Both eyes 0 */
    FACE_UPLOAD_01,   /* Left 0, Right 1 */
    FACE_UPLOAD_10,   /* Left 1, Right 0 */
    FACE_UPLOAD_11,   /* Both eyes 1 */
    
    FACE_STATE_COUNT
} face_state_t;

/* Animation types */
typedef enum {
    ANIM_NONE = 0,
    ANIM_LOOK,        /* Alternate LOOK_L <-> LOOK_R */
    ANIM_LOOK_HAPPY,  /* Alternate LOOK_L_HAPPY <-> LOOK_R_HAPPY */
    ANIM_SLEEP,       /* Cycle SLEEP1->2->3->4->3->2->1 */
    ANIM_UPLOAD,      /* Cycle 00->01->11->10 (binary counter) */
    ANIM_DOWNLOAD     /* Cycle 11->10->00->01 (reverse binary) */
} animation_type_t;

/* Animation state */
typedef struct {
    animation_type_t type;
    int frame;           /* Current frame index */
    int direction;       /* 1 = forward, -1 = backward (for ping-pong) */
    uint32_t last_tick;  /* Last update time (ms) */
    int interval_ms;     /* Ms between frames */
} animation_state_t;

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

/* Global animation state */
extern animation_state_t g_anim_state;

/*
 * Initialize theme system
 */
int themes_init(const char *base_dir);
void themes_cleanup(void);

/*
 * Theme loading/management
 */
theme_t *theme_load(const char *name);
void theme_unload(theme_t *theme);
int theme_set_active(const char *name);
const char *theme_get_active(void);
char **theme_list_available(int *count);
void theme_list_free(char **list);

/*
 * Face access
 */
face_bitmap_t *theme_get_face(face_state_t state);
face_state_t theme_face_string_to_state(const char *face_str);

/*
 * Rendering
 */
/*
 * Rendering
 */
void theme_render_face(uint8_t *framebuffer, int fb_width, int fb_height,
                       int dest_x, int dest_y, face_state_t state, int invert);
void theme_render_face_by_string(uint8_t *framebuffer, int fb_width, int fb_height,
                                 int dest_x, int dest_y, const char *face_str, int invert);
void theme_render_face_animated(uint8_t *framebuffer, int fb_width, int fb_height,
                                int dest_x, int dest_y, const char *face_str, int invert);
face_state_t theme_name_to_state(const char *name);

/*
 * Animation support
 */
void animation_start(animation_type_t type, int interval_ms);
void animation_stop(void);
face_state_t animation_get_frame(void);
int animation_is_active(void);
void animation_tick(uint32_t now_ms);

#endif /* PWNAUI_THEMES_H */
