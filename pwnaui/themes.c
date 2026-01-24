/*
 * PwnaUI Theme System Implementation
 * Runtime PNG-based face themes with hot-swapping
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "themes.h"
#include "lodepng.h"

/* Global theme manager */
theme_manager_t g_theme_mgr = {0};

/* Whether themes are enabled (vs text fallback) */
static int g_themes_enabled = 0;

/* Face state names - must match face_state_t enum order */
const char *g_face_state_names[FACE_STATE_COUNT] = {
    "LOOK_R",
    "LOOK_L", 
    "LOOK_R_HAPPY",
    "LOOK_L_HAPPY",
    "SLEEP",
    "SLEEP2",
    "AWAKE",
    "BORED",
    "INTENSE",
    "COOL",
    "HAPPY",
    "EXCITED",
    "GRATEFUL",
    "MOTIVATED",
    "DEMOTIVATED",
    "SMART",
    "LONELY",
    "SAD",
    "ANGRY",
    "FRIEND",
    "BROKEN",
    "DEBUG",
    "UPLOAD",
    "UPLOAD1",
    "UPLOAD2"
};

/* Face string to state mapping */
typedef struct {
    const char *face_str;
    face_state_t state;
} face_str_map_t;

/* Map common pwnagotchi face strings to states */
static const face_str_map_t g_face_str_map[] = {
    /* Happy/Positive */
    {"(◕‿‿◕)",    FACE_HAPPY},
    {"(◕‿◕)",     FACE_HAPPY},
    {"(^_^)",     FACE_HAPPY},
    {"(◕ᴗ◕)",     FACE_EXCITED},
    {"(ᵔ◡ᵔ)",     FACE_EXCITED},
    
    /* Cool */
    {"(⌐■_■)",    FACE_COOL},
    {"(≖‿‿≖)",    FACE_COOL},
    
    /* Looking */
    {"( ⚆_⚆)",    FACE_LOOK_R},
    {"( ⚆_⚆ )",   FACE_LOOK_R},
    {"(⚆_⚆ )",    FACE_LOOK_L},
    {"( ◕‿◕)",    FACE_LOOK_R_HAPPY},
    {"(◕‿◕ )",    FACE_LOOK_L_HAPPY},
    
    /* Sleeping */
    {"(⇀‿‿↼)",    FACE_SLEEP},
    {"(-_-) zzZ", FACE_SLEEP},
    {"(－_－) zzZ", FACE_SLEEP},
    {"(￣o￣) zzZ", FACE_SLEEP2},
    
    /* Sad/Negative */
    {"(;_;)",     FACE_SAD},
    {"(T_T)",     FACE_SAD},
    {"(╥☁╥)",     FACE_SAD},
    {"(╥﹏╥)",     FACE_SAD},
    {"(;﹏;)",     FACE_SAD},
    
    /* Angry */
    {"(>_<)",     FACE_ANGRY},
    {"(-_-')",    FACE_ANGRY},
    {"(ಠ_ಠ)",     FACE_ANGRY},
    
    /* Bored */
    {"(-_-)",     FACE_BORED},
    {"(¬_¬)",     FACE_BORED},
    {"(－‸ლ)",     FACE_BORED},
    
    /* Intense */
    {"(ง'̀-'́)ง",   FACE_INTENSE},
    {"(ง •̀_•́)ง",  FACE_INTENSE},
    
    /* Friend */
    {"(♥‿‿♥)",    FACE_FRIEND},
    
    /* Broken/Error */
    {"(☓‿‿☓)",    FACE_BROKEN},
    {"(×_×)",     FACE_BROKEN},
    {"(x_x)",     FACE_BROKEN},
    
    /* Lonely */
    {"(ب__ب)",    FACE_LONELY},
    
    /* Motivated */
    {"(☼‿‿☼)",    FACE_MOTIVATED},
    {"(•̀ᴗ•́)و",   FACE_MOTIVATED},
    
    /* Demotivated */
    {"(≖__≖)",    FACE_DEMOTIVATED},
    
    /* Smart */
    {"(✜‿‿✜)",    FACE_SMART},
    
    /* Grateful */
    {"(^‿‿^)",    FACE_GRATEFUL},
    
    /* Debug */
    {"(#__#)",    FACE_DEBUG},
    
    /* Upload */
    {"(1__0)",    FACE_UPLOAD},
    {"(1__1)",    FACE_UPLOAD1},
    {"(0__1)",    FACE_UPLOAD2},
    
    /* Awake */
    {"(◕◡◕)",     FACE_AWAKE},
    {"(•‿•)",     FACE_AWAKE},
    
    {NULL, FACE_HAPPY}  /* Terminator + default */
};

/*
 * Initialize theme system
 */
int themes_init(const char *base_dir) {
    memset(&g_theme_mgr, 0, sizeof(g_theme_mgr));
    
    if (base_dir) {
        strncpy(g_theme_mgr.base_dir, base_dir, sizeof(g_theme_mgr.base_dir) - 1);
    } else {
        strncpy(g_theme_mgr.base_dir, THEME_BASE_DIR, sizeof(g_theme_mgr.base_dir) - 1);
    }
    
    /* Create themes directory if it doesn't exist */
    mkdir(g_theme_mgr.base_dir, 0755);
    
    /* Allocate initial theme array */
    g_theme_mgr.theme_capacity = 8;
    g_theme_mgr.themes = calloc(g_theme_mgr.theme_capacity, sizeof(theme_t));
    if (!g_theme_mgr.themes) {
        return -1;
    }
    
    g_themes_enabled = 0;  /* Start with text rendering */
    
    return 0;
}

/*
 * Cleanup theme system
 */
void themes_cleanup(void) {
    if (g_theme_mgr.themes) {
        for (int i = 0; i < g_theme_mgr.theme_count; i++) {
            theme_unload(&g_theme_mgr.themes[i]);
        }
        free(g_theme_mgr.themes);
        g_theme_mgr.themes = NULL;
    }
    g_theme_mgr.theme_count = 0;
    g_theme_mgr.current = NULL;
}

/*
 * Load a PNG file and convert to 1-bit bitmap
 * Returns 0 on success, -1 on error
 */
static int load_face_png(const char *path, face_bitmap_t *face) {
    unsigned char *rgba = NULL;
    unsigned width, height;
    unsigned error;
    
    /* Decode PNG to RGBA */
    error = lodepng_decode32_file(&rgba, &width, &height, path);
    if (error) {
        fprintf(stderr, "PNG decode error %u: %s\n", error, lodepng_error_text(error));
        return -1;
    }
    
    /* Allocate 1-bit bitmap */
    int stride = (width + 7) / 8;  /* bytes per row */
    size_t bitmap_size = stride * height;
    face->bitmap = calloc(1, bitmap_size);
    if (!face->bitmap) {
        free(rgba);
        return -1;
    }
    
    face->width = width;
    face->height = height;
    face->stride = stride;
    
    /* Convert RGBA to 1-bit using luminance threshold
     * For e-ink: 1 = black, 0 = white
     * Consider alpha channel for transparency
     */
    for (unsigned y = 0; y < height; y++) {
        for (unsigned x = 0; x < width; x++) {
            unsigned idx = (y * width + x) * 4;  /* RGBA = 4 bytes per pixel */
            unsigned char r = rgba[idx + 0];
            unsigned char g = rgba[idx + 1];
            unsigned char b = rgba[idx + 2];
            unsigned char a = rgba[idx + 3];
            
            /* Calculate luminance */
            /* Using standard luminance formula: 0.299*R + 0.587*G + 0.114*B */
            unsigned lum = (299 * r + 587 * g + 114 * b) / 1000;
            
            /* If alpha < 128, treat as white (transparent -> white on e-ink) */
            /* If luminance < 128, treat as black */
            int is_black = (a >= 128) && (lum < 128);
            
            if (is_black) {
                /* Set bit (black pixel) */
                int byte_idx = y * stride + x / 8;
                int bit_idx = 7 - (x % 8);
                face->bitmap[byte_idx] |= (1 << bit_idx);
            }
        }
    }
    
    free(rgba);
    face->loaded = 1;
    return 0;
}

/*
 * Find the directory containing face PNGs within a theme
 * Themes can have various structures:
 * - faces directly in theme root
 * - custom-faces/ subdirectory
 * - faces_*/ subdirectory (e.g., faces_flipper_dolphin)
 * - _faces/ subdirectory
 * Returns 1 if found (and fills faces_dir), 0 if not found
 */
static int find_faces_dir(const char *theme_path, char *faces_dir, size_t faces_dir_size) {
    char test_path[512];
    struct stat st;
    
    /* Check if HAPPY.png exists directly in theme root */
    snprintf(test_path, sizeof(test_path), "%s/HAPPY.png", theme_path);
    if (stat(test_path, &st) == 0 && S_ISREG(st.st_mode)) {
        strncpy(faces_dir, theme_path, faces_dir_size - 1);
        return 1;
    }
    
    /* Search subdirectories */
    DIR *dir = opendir(theme_path);
    if (!dir) return 0;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        
        char subdir[512];
        snprintf(subdir, sizeof(subdir), "%s/%s", theme_path, entry->d_name);
        
        if (stat(subdir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        
        /* Check if HAPPY.png exists in this subdirectory */
        snprintf(test_path, sizeof(test_path), "%s/HAPPY.png", subdir);
        if (stat(test_path, &st) == 0 && S_ISREG(st.st_mode)) {
            strncpy(faces_dir, subdir, faces_dir_size - 1);
            closedir(dir);
            return 1;
        }
    }
    
    closedir(dir);
    return 0;
}

/*
 * Load a theme by name
 */
theme_t *theme_load(const char *name) {
    if (!name || !g_theme_mgr.themes) {
        return NULL;
    }
    
    /* Check if already loaded */
    for (int i = 0; i < g_theme_mgr.theme_count; i++) {
        if (strcmp(g_theme_mgr.themes[i].name, name) == 0) {
            return &g_theme_mgr.themes[i];
        }
    }
    
    /* Expand array if needed */
    if (g_theme_mgr.theme_count >= g_theme_mgr.theme_capacity) {
        int new_cap = g_theme_mgr.theme_capacity * 2;
        theme_t *new_themes = realloc(g_theme_mgr.themes, new_cap * sizeof(theme_t));
        if (!new_themes) {
            return NULL;
        }
        g_theme_mgr.themes = new_themes;
        g_theme_mgr.theme_capacity = new_cap;
    }
    
    /* Create new theme entry */
    theme_t *theme = &g_theme_mgr.themes[g_theme_mgr.theme_count];
    memset(theme, 0, sizeof(theme_t));
    
    strncpy(theme->name, name, sizeof(theme->name) - 1);
    snprintf(theme->path, sizeof(theme->path), "%s/%s", g_theme_mgr.base_dir, name);
    
    /* Check if theme directory exists */
    struct stat st;
    if (stat(theme->path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Theme directory not found: %s\n", theme->path);
        return NULL;
    }
    
    /* Find the faces directory */
    char faces_dir[512];
    if (!find_faces_dir(theme->path, faces_dir, sizeof(faces_dir))) {
        fprintf(stderr, "No faces found in theme '%s'\n", name);
        return NULL;
    }
    
    printf("Loading theme '%s' from %s\n", name, faces_dir);
    
    /* Load each face PNG */
    int loaded_count = 0;
    for (int i = 0; i < FACE_STATE_COUNT; i++) {
        char png_path[512];
        snprintf(png_path, sizeof(png_path), "%s/%s.png", faces_dir, g_face_state_names[i]);
        
        if (load_face_png(png_path, &theme->faces[i]) == 0) {
            loaded_count++;
            
            /* Track common face dimensions */
            if (theme->face_width == 0) {
                theme->face_width = theme->faces[i].width;
                theme->face_height = theme->faces[i].height;
            }
        }
    }
    
    if (loaded_count > 0) {
        theme->loaded = 1;
        g_theme_mgr.theme_count++;
        printf("Loaded theme '%s' with %d faces (%dx%d)\n", 
               name, loaded_count, theme->face_width, theme->face_height);
        return theme;
    }
    
    fprintf(stderr, "Failed to load any faces for theme '%s'\n", name);
    return NULL;
}

/*
 * Unload a theme and free resources
 */
void theme_unload(theme_t *theme) {
    if (!theme) return;
    
    for (int i = 0; i < FACE_STATE_COUNT; i++) {
        if (theme->faces[i].bitmap) {
            free(theme->faces[i].bitmap);
            theme->faces[i].bitmap = NULL;
        }
        theme->faces[i].loaded = 0;
    }
    theme->loaded = 0;
}

/*
 * Set the active theme
 */
int theme_set_active(const char *name) {
    if (!name) {
        g_theme_mgr.current = NULL;
        g_themes_enabled = 0;
        return 0;
    }
    
    /* Try to find or load the theme */
    theme_t *theme = NULL;
    
    /* Check if already loaded */
    for (int i = 0; i < g_theme_mgr.theme_count; i++) {
        if (strcmp(g_theme_mgr.themes[i].name, name) == 0) {
            theme = &g_theme_mgr.themes[i];
            break;
        }
    }
    
    /* Try to load if not found */
    if (!theme) {
        theme = theme_load(name);
    }
    
    if (theme && theme->loaded) {
        g_theme_mgr.current = theme;
        g_themes_enabled = 1;
        printf("Active theme set to '%s'\n", name);
        return 0;
    }
    
    return -1;
}

/*
 * Get list of available themes
 */
char **theme_list_available(int *count) {
    DIR *dir = opendir(g_theme_mgr.base_dir);
    if (!dir) {
        if (count) *count = 0;
        return NULL;
    }
    
    /* Count directories */
    int num_themes = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", g_theme_mgr.base_dir, entry->d_name);
        
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            num_themes++;
        }
    }
    
    if (num_themes == 0) {
        closedir(dir);
        if (count) *count = 0;
        return NULL;
    }
    
    /* Allocate array (NULL terminated) */
    char **list = calloc(num_themes + 1, sizeof(char *));
    if (!list) {
        closedir(dir);
        if (count) *count = 0;
        return NULL;
    }
    
    /* Fill array */
    rewinddir(dir);
    int idx = 0;
    while ((entry = readdir(dir)) != NULL && idx < num_themes) {
        if (entry->d_name[0] == '.') continue;
        
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", g_theme_mgr.base_dir, entry->d_name);
        
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            list[idx] = strdup(entry->d_name);
            idx++;
        }
    }
    
    closedir(dir);
    
    if (count) *count = idx;
    return list;
}

/*
 * Free theme list
 */
void theme_list_free(char **list) {
    if (!list) return;
    
    for (int i = 0; list[i] != NULL; i++) {
        free(list[i]);
    }
    free(list);
}

/*
 * Get face bitmap for current theme
 */
face_bitmap_t *theme_get_face(face_state_t state) {
    if (!g_theme_mgr.current || state < 0 || state >= FACE_STATE_COUNT) {
        return NULL;
    }
    
    face_bitmap_t *face = &g_theme_mgr.current->faces[state];
    if (face->loaded) {
        return face;
    }
    
    /* Fallback to HAPPY face */
    face = &g_theme_mgr.current->faces[FACE_HAPPY];
    if (face->loaded) {
        return face;
    }
    
    return NULL;
}

/*
 * Map face string to face state
 */
face_state_t theme_face_string_to_state(const char *face_str) {
    if (!face_str || !face_str[0]) {
        return FACE_HAPPY;
    }
    
    /* Search for match */
    for (int i = 0; g_face_str_map[i].face_str != NULL; i++) {
        if (strcmp(face_str, g_face_str_map[i].face_str) == 0) {
            return g_face_str_map[i].state;
        }
    }
    
    return FACE_HAPPY;  /* Default */
}

/*
 * Render face from current theme to framebuffer
 */
void theme_render_face(uint8_t *framebuffer, int fb_width, int fb_height,
                       int dest_x, int dest_y, face_state_t state, int invert) {
    face_bitmap_t *face = theme_get_face(state);
    if (!face || !face->bitmap) {
        return;  /* No face to render */
    }
    
    /* Blit bitmap to framebuffer */
    for (int y = 0; y < face->height; y++) {
        int screen_y = dest_y + y;
        if (screen_y < 0 || screen_y >= fb_height) continue;
        
        for (int x = 0; x < face->width; x++) {
            int screen_x = dest_x + x;
            if (screen_x < 0 || screen_x >= fb_width) continue;
            
            /* Get pixel from face bitmap */
            int src_byte = y * face->stride + x / 8;
            int src_bit = 7 - (x % 8);
            int pixel = (face->bitmap[src_byte] >> src_bit) & 1;
            
            /* Apply invert if needed */
            if (invert) {
                pixel = !pixel;
            }
            
            /* Set pixel in framebuffer */
            int fb_byte = (screen_y * fb_width + screen_x) / 8;
            int fb_bit = 7 - ((screen_y * fb_width + screen_x) % 8);
            
            if (pixel) {
                framebuffer[fb_byte] |= (1 << fb_bit);
            } else {
                framebuffer[fb_byte] &= ~(1 << fb_bit);
            }
        }
    }
}

/*
 * Render face by string (convenience wrapper)
 */
void theme_render_face_by_string(uint8_t *framebuffer, int fb_width, int fb_height,
                                 int dest_x, int dest_y, const char *face_str, int invert) {
    face_state_t state = theme_face_string_to_state(face_str);
    theme_render_face(framebuffer, fb_width, fb_height, dest_x, dest_y, state, invert);
}

/*
 * Check if themes are enabled
 */
int themes_enabled(void) {
    return g_themes_enabled && g_theme_mgr.current != NULL;
}

/*
 * Enable/disable theme rendering
 */
void themes_set_enabled(int enabled) {
    g_themes_enabled = enabled;
}
