/*
 * faces.c - Sprite-based face rendering for PwnaUI
 * 36 faces in 4x9 grid, 64x40 pixels each
 */

#include <string.h>
#include <stdint.h>
#include "faces.h"

/* Face string to sprite ID mapping */
typedef struct {
    const char *face_str;
    int sprite_id;
} face_map_t;

/* Map face strings to sprite IDs (0-35) */
static const face_map_t g_face_map[] = {
    /* Row 0: Boot/Startup faces */
    {"(•‿•)",           0},   /* Boot / Startup */
    {"(⌐■_■)",          1},   /* Boot / Scanning, sunglasses */
    {"(◕‿◕)",           2},   /* Ready / Idle - DEFAULT */
    {"(=^‿^=)",         3},   /* Ready / Idle (Catlike) */
    
    /* Row 1: More idle/ready */
    {"(ᵔ◡ᵔ)",           5},   /* Ready / Idle */
    {"(◕‿◕✿)",          6},   /* Learning / AI Training */
    {"(◔_◔)",           7},   /* Learning / Curious */
    {"(ಠ_ಠ)",           8},   /* Learning / Annoyed */
    
    /* Row 2: Searching/Scanning */
    {"( ⚆_⚆ )",         9},   /* Searching / Scanning */
    {"( •_•)>⌐■-■",    10},   /* Scanning / Sunglasses transition */
    
    /* Row 3: Associating */
    {"(•̀ᴗ•́)و",        12},   /* Associating / Confident */
    {"(ง •̀_•́)ง",       13},   /* Associating / Ready to handshake */
    
    /* Row 4: Deauthing */
    {"(ง'̀-'́)ง",        15},   /* Deauthing / Aggressive */
    {"(ಠ‿ಠ)",          16},   /* Deauthing / Sly */
    {"(¬‿¬)",          17},   /* Deauthing / Smirking */
    
    /* Row 4-5: Sad faces */
    {"(╥﹏╥)",          18},   /* Sad / No Networks */
    {"(;﹏;)",          19},   /* Sad / Crying */
    {"(╥☁╥)",          20},   /* Sad / Cloudy tears */
    
    /* Row 5: Bored faces */
    {"(¬_¬)",          21},   /* Bored / Unamused */
    {"(－‸ლ)",          23},   /* Bored / Facepalm */
    
    /* Row 6: Low battery / Error */
    {"(×_×)",          24},   /* Low Battery */
    {"(x_x)",          25},   /* Low Battery / Faint */
    {"(☉_☉)",          26},   /* Error / Crash */
    {"(✖╭╮✖)",         27},   /* Error / Sad crash */
    {"(ಥ﹏ಥ)",          28},   /* Error / Crying crash */
    
    /* Row 7: Sleeping */
    {"(－_－) zzZ",     29},   /* Sleeping */
    {"(￣o￣) zzZ",     30},   /* Sleeping */
    
    /* Row 8: Variants */
    {"(˳ᴗ˳)و",         31},   /* Variant confident */
    {"(•_•)",          32},   /* Pre-sunglasses */
    {"(•_•)>⌐■-■",     33},   /* Sunglasses transition */
    
    /* Common Pwnagotchi faces - map to closest */
    {"(◕‿‿◕)",         2},   /* Happy -> Ready/Idle */
    {"(≖‿‿≖)",         1},   /* Cool -> Sunglasses */
    {"(◕ᴗ◕)",          5},   /* Excited -> Ready */
    {"(-_-)",         21},   /* Bored -> Unamused */
    {"(≖_≖)",          8},   /* Suspicious -> Annoyed */
    {"(>_<)",         15},   /* Angry -> Aggressive */
    {"(;_;)",         19},   /* Sad -> Crying */
    {"(T_T)",         19},   /* Crying -> Crying */
    {"(°_°)",         26},   /* Surprised -> Error */
    {"(~_~)",         21},   /* Tired -> Bored */
    {"(^_^)",          2},   /* Happy -> Ready */
    {"( ᵕ◡ᵕ)",         5},   /* Content -> Ready */
    
    {NULL, -1}  /* Terminator */
};

/* Default face ID if no match found */
#define DEFAULT_FACE_ID 2  /* (◕‿◕) Ready/Idle */

/*
 * Look up face string and return sprite ID
 */
int face_get_sprite_id(const char *face_str) {
    if (!face_str || !face_str[0]) {
        return DEFAULT_FACE_ID;
    }
    
    /* Search for exact match */
    for (int i = 0; g_face_map[i].face_str != NULL; i++) {
        if (strcmp(face_str, g_face_map[i].face_str) == 0) {
            return g_face_map[i].sprite_id;
        }
    }
    
    /* No match - return default */
    return DEFAULT_FACE_ID;
}

/*
 * Render face sprite to framebuffer
 * Blits from sprite sheet to destination
 */
void face_render(uint8_t *framebuffer, int fb_width, int fb_height,
                 int dest_x, int dest_y, int face_id, int invert) {
    if (face_id < 0 || face_id >= FACE_COUNT) {
        face_id = DEFAULT_FACE_ID;
    }
    
    /* Calculate source position in sprite sheet */
    int col = face_id % FACE_COLS;
    int row = face_id / FACE_COLS;
    int src_x = col * FACE_WIDTH;
    int src_y = row * FACE_HEIGHT;
    
    /* Bytes per row in sprite sheet */
    int sheet_row_bytes = FACE_SHEET_WIDTH / 8;
    
    /* Blit face to framebuffer */
    for (int y = 0; y < FACE_HEIGHT; y++) {
        int screen_y = dest_y + y;
        if (screen_y < 0 || screen_y >= fb_height) continue;
        
        for (int x = 0; x < FACE_WIDTH; x++) {
            int screen_x = dest_x + x;
            if (screen_x < 0 || screen_x >= fb_width) continue;
            
            /* Get pixel from sprite sheet */
            int sheet_x = src_x + x;
            int sheet_y = src_y + y;
            int byte_idx = sheet_y * sheet_row_bytes + sheet_x / 8;
            int bit_idx = 7 - (sheet_x % 8);
            int pixel = (g_face_spritesheet[byte_idx] >> bit_idx) & 1;
            
            /* Invert sprite (sprite has black bg, we want white bg) */
            pixel = !pixel;
            
            /* Apply invert if needed (for inverted display mode) */
            if (invert) {
                pixel = !pixel;
            }
            
            /* Set pixel in framebuffer - use same formula as renderer_set_pixel */
            int fb_byte = (screen_y * fb_width + screen_x) / 8;
            int fb_bit = 7 - ((screen_y * fb_width + screen_x) % 8);
            
            if (pixel) {
                /* Set bit (black pixel) */
                framebuffer[fb_byte] |= (1 << fb_bit);
            } else {
                /* Clear bit (white pixel) */
                framebuffer[fb_byte] &= ~(1 << fb_bit);
            }
        }
    }
}

/*
 * Render face by string - convenience function
 */
void face_render_by_string(uint8_t *framebuffer, int fb_width, int fb_height,
                           int dest_x, int dest_y, const char *face_str, int invert) {
    int face_id = face_get_sprite_id(face_str);
    face_render(framebuffer, fb_width, fb_height, dest_x, dest_y, face_id, invert);
}
