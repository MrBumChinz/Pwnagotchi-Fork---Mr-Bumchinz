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
    /* Expressions */
    "HAPPY",
    "SAD",
    "ANGRY",
    "EXCITED",
    "GRATEFUL",
    "LONELY",
    "COOL",
    "INTENSE",
    "SMART",
    "FRIEND",
    "BROKEN",
    "DEBUG",
    "DEMOTIVATED",
    
    /* Looking animations */
    "LOOK_L",
    "LOOK_R",
    "LOOK_L_HAPPY",
    "LOOK_R_HAPPY",
    
    /* Sleep animations */
    "SLEEP1",
    "SLEEP2",
    "SLEEP3",
    "SLEEP4",
    
    /* Upload/Download animations (binary eyes) */
    "00",
    "01",
    "10",
    "11"
};

/* Global animation state */
animation_state_t g_anim_state = {0};

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
    {"(._. )",     FACE_LOOK_L},
    {"(o_o)",      FACE_LOOK_L},
    {"( ._. )",    FACE_LOOK_R},
    {"(._.)",      FACE_SAD},
    
    /* Sleeping */
    {"(⇀‿‿↼)",    FACE_SLEEP1},
    {"(-_-) zzZ", FACE_SLEEP1},
    {"(－_－) zzZ", FACE_SLEEP1},
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
    {"(-__-)",    FACE_DEMOTIVATED},
    {"(-_-)",     FACE_DEMOTIVATED},
    {"(¬_¬)",     FACE_DEMOTIVATED},
    {"(－‸ლ)",     FACE_DEMOTIVATED},
    
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
    {"(☼‿‿☼)",    FACE_EXCITED},
    {"(•̀ᴗ•́)و",   FACE_EXCITED},
    
    /* Demotivated */
    {"(≖__≖)",    FACE_DEMOTIVATED},
    
    /* Smart */
    {"(✜‿‿✜)",    FACE_SMART},
    
    /* Grateful */
    {"(^‿‿^)",    FACE_GRATEFUL},
    
    /* Debug */
    {"(#__#)",    FACE_DEBUG},
    
    /* Upload */
    {"(1__0)",    FACE_UPLOAD_11},
    {"(1__1)",    FACE_UPLOAD_01},
    {"(0__1)",    FACE_UPLOAD_10},
    
    /* Awake */
    {"(◕◡◕)",     FACE_HAPPY},
    {"(•‿•)",     FACE_HAPPY},
    
    /* Plain state names (for SET_FACE STATENAME commands) */
    {"LOOK_R",       FACE_LOOK_R},
    {"LOOK_L",       FACE_LOOK_L},
    {"LOOK_R_HAPPY", FACE_LOOK_R_HAPPY},
    {"LOOK_L_HAPPY", FACE_LOOK_L_HAPPY},
    {"SLEEP",        FACE_SLEEP1},
    {"SLEEP2",       FACE_SLEEP2},
    {"AWAKE",        FACE_HAPPY},
    {"BORED",        FACE_DEMOTIVATED},
    {"INTENSE",      FACE_INTENSE},
    {"COOL",         FACE_COOL},
    {"HAPPY",        FACE_HAPPY},
    {"EXCITED",      FACE_EXCITED},
    {"GRATEFUL",     FACE_GRATEFUL},
    {"MOTIVATED",    FACE_EXCITED},
    {"DEMOTIVATED",  FACE_DEMOTIVATED},
    {"SMART",        FACE_SMART},
    {"LONELY",       FACE_LONELY},
    {"SAD",          FACE_SAD},
    {"ANGRY",        FACE_ANGRY},
    {"FRIEND",       FACE_FRIEND},
    {"BROKEN",       FACE_BROKEN},
    {"DEBUG",        FACE_DEBUG},
    {"UPLOAD",       FACE_UPLOAD_11},
    {"UPLOAD1",      FACE_UPLOAD_01},
    {"UPLOAD2",      FACE_UPLOAD_10},
    
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
    g_theme_mgr.base_dir[sizeof(g_theme_mgr.base_dir) - 1] = '\0';
    
    /* Create themes directory if it doesn't exist */
    mkdir(g_theme_mgr.base_dir, 0755);
    
    /* Allocate initial theme array */
    g_theme_mgr.theme_capacity = 32;
    g_theme_mgr.themes = calloc(g_theme_mgr.theme_capacity, sizeof(theme_t));
    if (!g_theme_mgr.themes) {
        return -1;
    }
    
    g_themes_enabled = 0;  /* Start with text rendering */
    
    /* Scan themes directory for available themes */
    DIR *dir = opendir(g_theme_mgr.base_dir);
    if (!dir) {
        fprintf(stderr, "Cannot open themes directory: %s\n", g_theme_mgr.base_dir);
        return 0;  /* Not fatal, just no themes */
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (entry->d_name[0] == '.') continue;
        
        /* Check if it's a directory */
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", g_theme_mgr.base_dir, entry->d_name);
        
        struct stat st;
        if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            /* Found a potential theme directory, try to load it */
            theme_t *theme = theme_load(entry->d_name);
            if (theme && theme->loaded) {
                fprintf(stderr, "Discovered theme: %s (%d faces, %dx%d)\n", 
                        entry->d_name, 
                        theme->face_width > 0 ? FACE_STATE_COUNT : 0,
                        theme->face_width, theme->face_height);
            }
        }
    }
    closedir(dir);
    
    fprintf(stderr, "Theme system initialized: %d themes found\n", g_theme_mgr.theme_count);
    
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
    
    /* Debug log to file */
    FILE *dbg = fopen("/tmp/theme_debug.log", "a");
    
    /* Decode PNG to RGBA */
    error = lodepng_decode32_file(&rgba, &width, &height, path);
    if (error) {
        if (dbg) { fprintf(dbg, "PNG decode error %u: %s - %s\n", error, lodepng_error_text(error), path); fclose(dbg); }
        fprintf(stderr, "PNG decode error %u: %s\n", error, lodepng_error_text(error));
        return -1;
    }
    
    if (dbg) fprintf(dbg, "DEBUG: Loaded %s: %ux%u\n", path, width, height);
    
    /* Debug: check first few pixels */
    if (dbg) {
        fprintf(dbg, "DEBUG: First 8 pixels RGBA: ");
        for (int i = 0; i < 8 && i < (int)(width * height); i++) {
            fprintf(dbg, "[%02x%02x%02x%02x] ", rgba[i*4], rgba[i*4+1], rgba[i*4+2], rgba[i*4+3]);
        }
        fprintf(dbg, "\n");
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
    int black_count = 0, white_count = 0;
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
                black_count++;
                /* Set bit (black pixel) */
                int byte_idx = y * stride + x / 8;
                int bit_idx = 7 - (x % 8);
                face->bitmap[byte_idx] |= (1 << bit_idx);
            } else {
                white_count++;
            }
        }
    }
    
    if (dbg) {
        fprintf(dbg, "DEBUG: black=%d, white=%d (total %d)\n", black_count, white_count, black_count + white_count);
        fprintf(dbg, "DEBUG: First 8 bitmap bytes: ");
        for (int i = 0; i < 8 && i < (int)bitmap_size; i++) {
            fprintf(dbg, "%02x ", face->bitmap[i]);
        }
        fprintf(dbg, "\n");
        fclose(dbg);
    }
    
    free(rgba);
    face->loaded = 1;
    return 0;
}

/*
 * Find the directory containing face PNGs within a theme
 * Themes can have various structures:
 * - faces directly in theme root
 * - custom-faces subdirectory
 * - faces_* subdirectory like faces_flipper_dolphin
 * - _faces subdirectory
 * Returns 1 if found (and fills faces_dir), 0 if not found
 * Also sets *use_lowercase flag if lowercase filenames detected
 */
static int check_for_face_png(const char *dir_path, int *use_lowercase) {
    /* Check for both HAPPY.png and happy.png */
    char test_path[512];
    struct stat st;
    
    snprintf(test_path, sizeof(test_path), "%s/HAPPY.png", dir_path);
    if (stat(test_path, &st) == 0 && S_ISREG(st.st_mode)) {
        if (use_lowercase) *use_lowercase = 0;
        return 1;
    }
    
    snprintf(test_path, sizeof(test_path), "%s/happy.png", dir_path);
    if (stat(test_path, &st) == 0 && S_ISREG(st.st_mode)) {
        if (use_lowercase) *use_lowercase = 1;
        return 1;
    }
    
    return 0;
}

static int find_faces_dir(const char *theme_path, char *faces_dir, size_t faces_dir_size, int *use_lowercase) {
    struct stat st;
    
    /* Check if face exists directly in theme root */
    if (check_for_face_png(theme_path, use_lowercase)) {
        strncpy(faces_dir, theme_path, faces_dir_size - 1);
        faces_dir[faces_dir_size - 1] = '\0';
        return 1;
    }
    
    /* Search subdirectories */
    DIR *dir = opendir(theme_path);
    if (!dir) {
        fprintf(stderr, "Cannot open theme directory: %s\n", theme_path);
        return 0;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (entry->d_name[0] == '_' && entry->d_name[1] == '_') continue;  /* Skip __pycache__ etc */
        
        char subdir[512];
        snprintf(subdir, sizeof(subdir), "%s/%s", theme_path, entry->d_name);
        
        if (stat(subdir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        
        /* Check if face PNG exists in this subdirectory */
        if (check_for_face_png(subdir, use_lowercase)) {
            strncpy(faces_dir, subdir, faces_dir_size - 1);
            faces_dir[faces_dir_size - 1] = '\0';
            closedir(dir);
            return 1;
        }
    }
    
    closedir(dir);
    fprintf(stderr, "No faces directory found in: %s\n", theme_path);
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
    int use_lowercase = 0;
    if (!find_faces_dir(theme->path, faces_dir, sizeof(faces_dir), &use_lowercase)) {
        fprintf(stderr, "No faces found in theme '%s'\n", name);
        return NULL;
    }
    
    /* Store lowercase preference in theme */
    theme->use_lowercase = use_lowercase;
    
    printf("Loading theme '%s' from %s (lowercase=%d)\n", name, faces_dir, use_lowercase);
    
    /* Load each face PNG */
    int loaded_count = 0;
    for (int i = 0; i < FACE_STATE_COUNT; i++) {
        char png_path[512];
        char face_name[64];
        
        /* Convert face name to lowercase if needed */
        if (use_lowercase) {
            strncpy(face_name, g_face_state_names[i], sizeof(face_name) - 1);
            face_name[sizeof(face_name) - 1] = '\0';
            for (char *p = face_name; *p; p++) {
                if (*p >= 'A' && *p <= 'Z') *p = *p - 'A' + 'a';
            }
        } else {
            strncpy(face_name, g_face_state_names[i], sizeof(face_name) - 1);
            face_name[sizeof(face_name) - 1] = '\0';
        }
        
        snprintf(png_path, sizeof(png_path), "%s/%s.png", faces_dir, face_name);
        
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

/* Get face PNG filename from face state (returns lowercase name like "awake") */

/*
 * Animation functions
 */

/* Sleep animation frames (ping-pong: 1->2->3->4->3->2->1) */
static const face_state_t sleep_frames[] = {
    FACE_SLEEP1, FACE_SLEEP2, FACE_SLEEP3, FACE_SLEEP4,
    FACE_SLEEP3, FACE_SLEEP2
};
#define SLEEP_FRAME_COUNT 6

/* Look animation frames */
static const face_state_t look_frames[] = { FACE_LOOK_L, FACE_LOOK_R };
static const face_state_t look_happy_frames[] = { FACE_LOOK_L_HAPPY, FACE_LOOK_R_HAPPY };
#define LOOK_FRAME_COUNT 2

/* Upload animation (binary counter: 00->01->10->11) */
static const face_state_t upload_frames[] = {
    FACE_UPLOAD_00, FACE_UPLOAD_01, FACE_UPLOAD_10, FACE_UPLOAD_11
};
#define UPLOAD_FRAME_COUNT 4

/* Download animation (reverse: 11->10->01->00) */
static const face_state_t download_frames[] = {
    FACE_UPLOAD_11, FACE_UPLOAD_10, FACE_UPLOAD_01, FACE_UPLOAD_00
};
#define DOWNLOAD_FRAME_COUNT 4

void animation_start(animation_type_t type, int interval_ms) {
    g_anim_state.type = type;
    g_anim_state.frame = 0;
    g_anim_state.direction = 1;
    g_anim_state.interval_ms = interval_ms > 0 ? interval_ms : 500;
    g_anim_state.last_tick = 0;
}

void animation_stop(void) {
    g_anim_state.type = ANIM_NONE;
}

int animation_is_active(void) {
    return g_anim_state.type != ANIM_NONE;
}

void animation_tick(uint32_t now_ms) {
    if (g_anim_state.type == ANIM_NONE) return;
    
    if (now_ms - g_anim_state.last_tick < (uint32_t)g_anim_state.interval_ms) return;
    
    g_anim_state.last_tick = now_ms;
    
    int max_frame = 0;
    switch (g_anim_state.type) {
        case ANIM_LOOK:
        case ANIM_LOOK_HAPPY:
            max_frame = LOOK_FRAME_COUNT;
            break;
        case ANIM_SLEEP:
            max_frame = SLEEP_FRAME_COUNT;
            break;
        case ANIM_UPLOAD:
        case ANIM_DOWNLOAD:
            max_frame = UPLOAD_FRAME_COUNT;
            break;
        default:
            return;
    }
    
    g_anim_state.frame = (g_anim_state.frame + 1) % max_frame;
}

face_state_t animation_get_frame(void) {
    switch (g_anim_state.type) {
        case ANIM_LOOK:
            return look_frames[g_anim_state.frame % LOOK_FRAME_COUNT];
        case ANIM_LOOK_HAPPY:
            return look_happy_frames[g_anim_state.frame % LOOK_FRAME_COUNT];
        case ANIM_SLEEP:
            return sleep_frames[g_anim_state.frame % SLEEP_FRAME_COUNT];
        case ANIM_UPLOAD:
            return upload_frames[g_anim_state.frame % UPLOAD_FRAME_COUNT];
        case ANIM_DOWNLOAD:
            return download_frames[g_anim_state.frame % DOWNLOAD_FRAME_COUNT];
        default:
            return FACE_HAPPY;
    }
}


const char *theme_get_face_name(face_state_t state) {
    if (state < 0 || state >= FACE_STATE_COUNT) {
        return "awake";  /* Default */
    }
    /* g_face_state_names has uppercase like "AWAKE", but PNG files are lowercase */
    static char lowercase_name[32];
    const char *name = g_face_state_names[state];
    int i;
    for (i = 0; name[i] && i < 31; i++) {
        lowercase_name[i] = name[i];  /* Keep uppercase - files are AWAKE.png etc */
    }
    lowercase_name[i] = '\0';
    return lowercase_name;
}

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
 * Handles both ASCII emoticons and PNG paths
 */
face_state_t theme_face_string_to_state(const char *face_str) {
    if (!face_str || !face_str[0]) {
        return FACE_HAPPY;
    }
    
    /* Check if this is a PNG path (contains .png) */
    const char *png_ext = strstr(face_str, ".png");
    if (!png_ext) png_ext = strstr(face_str, ".PNG");
    
    if (png_ext) {
        /* Extract face name from path: /path/to/HAPPY.png -> HAPPY */
        const char *slash = strrchr(face_str, '/');
        const char *name_start = slash ? slash + 1 : face_str;
        
        /* Calculate name length (without .png) */
        size_t name_len = png_ext - name_start;
        if (name_len > 0 && name_len < 32) {
            char face_name[32];
            strncpy(face_name, name_start, name_len);
            face_name[name_len] = '\0';
            
            /* Convert to uppercase for matching */
            for (char *p = face_name; *p; p++) {
                if (*p >= 'a' && *p <= 'z') *p -= 32;
            }
            
            /* Match against state names */
            for (int i = 0; i < FACE_STATE_COUNT; i++) {
                if (strcmp(face_name, g_face_state_names[i]) == 0) {
                    return (face_state_t)i;
                }
            }
        }
        return FACE_HAPPY;  /* PNG path but unknown face name */
    }
    
    /* Search for ASCII emoticon match */
    for (int i = 0; g_face_str_map[i].face_str != NULL; i++) {
        if (strcmp(face_str, g_face_str_map[i].face_str) == 0) {
            return g_face_str_map[i].state;
        }
    }
    
    /* Check for plain state name (e.g., "HAPPY", "SAD", "BORED") */
    char upper[32];
    strncpy(upper, face_str, sizeof(upper) - 1);
    upper[sizeof(upper) - 1] = '\0';
    for (char *p = upper; *p; p++) {
        if (*p >= 'a' && *p <= 'z') *p -= 32;
    }
    for (int i = 0; i < FACE_STATE_COUNT; i++) {
        if (strcmp(upper, g_face_state_names[i]) == 0) {
            return (face_state_t)i;
        }
    }

    return FACE_HAPPY;  /* Default */
}

/* Scale factor for theme faces (percentage, 100 = no scale) */
static int g_theme_scale = 100;  /* 100% = native size, no scaling */

/*
 * Set theme scale factor (percentage)
 */
void theme_set_scale(int scale_percent) {
    if (scale_percent < 20) scale_percent = 20;
    if (scale_percent > 200) scale_percent = 200;
    g_theme_scale = scale_percent;
}

/*
 * Get current theme scale factor
 */
int theme_get_scale(void) {
    return g_theme_scale;
}

/*
 * Render face from current theme to framebuffer
 * Renders at NATIVE size - no forced scaling
 * Each theme displays at whatever size its face PNGs are
 */
void theme_render_face(uint8_t *framebuffer, int fb_width, int fb_height,
                       int dest_x, int dest_y, face_state_t state, int invert) {
    face_bitmap_t *face = theme_get_face(state);
    if (!face || !face->bitmap) {
        return;  /* No face to render */
    }
    
    /* Render at native size - no scaling */
    int face_w = face->width;
    int face_h = face->height;
    
    /* Blit face bitmap to framebuffer at native size */
    for (int y = 0; y < face_h; y++) {
        int screen_y = dest_y + y;
        if (screen_y < 0 || screen_y >= fb_height) continue;
        
        for (int x = 0; x < face_w; x++) {
            int screen_x = dest_x + x;
            if (screen_x < 0 || screen_x >= fb_width) continue;
            
            /* Get pixel from face bitmap */
            int src_byte = y * face->stride + x / 8;
            int src_bit = 7 - (x % 8);
            int pixel = (face->bitmap[src_byte] >> src_bit) & 1;
            
            /* Invert by default - PNG has 1=black, e-ink framebuffer needs 0=black */
            pixel = !pixel;
            
            /* Apply display invert if needed (double invert) */
            if (invert) {
                pixel = !pixel;
            }
            
            /* Set pixel in framebuffer - linear bit packing (same as renderer.c) */
            int fb_byte = (screen_y * fb_width + screen_x) / 8;
            int fb_bit = 7 - (screen_x % 8);
            
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

/*
 * Disable themes (convenience wrapper)
 */
void themes_disable(void) {
    g_themes_enabled = 0;
}

/*
 * Get count of available themes
 */
int themes_count(void) {
    return g_theme_mgr.theme_count;
}

/*
 * Static storage for theme name list
 */
static const char *g_theme_name_list[64];

/*
 * Get list of theme names
 */
const char **themes_list(void) {
    int count = g_theme_mgr.theme_count;
    if (count > 63) count = 63;  /* Cap at array size - 1 */
    
    for (int i = 0; i < count; i++) {
        g_theme_name_list[i] = g_theme_mgr.themes[i].name;
    }
    g_theme_name_list[count] = NULL;  /* NULL terminate */
    
    return g_theme_name_list;
}

/*
 * Get current active theme name
 */
const char *theme_get_active(void) {
    if (g_theme_mgr.current && g_theme_mgr.current->loaded) {
        return g_theme_mgr.current->name;
    }
    return "";
}

/*
 * Find face state by PNG name (e.g., " HAPPY\, \LOOK_L\, \SLEEP1\)
 */
face_state_t theme_name_to_state(const char *name) {
 if (!name) return FACE_DEMOTIVATED;
 
 for (int i = 0; i < FACE_STATE_COUNT; i++) {
 if (strcasecmp(g_face_state_names[i], name) == 0) {
 return (face_state_t)i;
 }
 }
 return FACE_DEMOTIVATED;
}

/*
 * Render face with animation override
 * If animation is active, uses animation frame instead of face string lookup
 */
void theme_render_face_animated(uint8_t *framebuffer, int fb_width, int fb_height,
 int dest_x, int dest_y, const char *face_str, int invert) {
 face_state_t state;
 
 /* Check if animation is active - if so, use animation frame */
 if (animation_is_active()) {
 state = animation_get_frame();
 } else {
 /* Fall back to string lookup */
 state = theme_face_string_to_state(face_str);
 }
 
 theme_render_face(framebuffer, fb_width, fb_height, dest_x, dest_y, state, invert);
}
