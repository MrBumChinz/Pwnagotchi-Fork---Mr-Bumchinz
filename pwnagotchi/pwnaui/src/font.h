/*
 * PwnaUI - Font Header
 * Bitmap font rendering with UTF-8 support
 */

#ifndef PWNAUI_FONT_H
#define PWNAUI_FONT_H

#include <stdint.h>
#include <stddef.h>

/* Glyph structure */
typedef struct {
    uint32_t codepoint;     /* Unicode codepoint */
    uint8_t width;          /* Glyph width in pixels */
    uint8_t height;         /* Glyph height in pixels */
    int8_t x_offset;        /* X offset from cursor */
    int8_t y_offset;        /* Y offset from baseline */
    uint8_t advance;        /* Cursor advance after glyph */
    const uint8_t *bitmap;  /* Packed 1-bit bitmap data */
} glyph_t;

/* Font structure */
typedef struct {
    const char *name;       /* Font name */
    uint8_t width;          /* Default glyph width */
    uint8_t height;         /* Line height */
    uint8_t baseline;       /* Baseline offset */
    int num_glyphs;         /* Number of glyphs */
    const glyph_t *glyphs;  /* Glyph array */
} font_t;

/* Font IDs (must match renderer.h) */
#define FONT_SMALL      0
#define FONT_MEDIUM     1
#define FONT_BOLD       2
#define FONT_BOLD_SMALL 3
#define FONT_LARGE      4
#define FONT_HUGE       4  /* Alias */
#define FONT_COUNT      5

/*
 * Initialize font system
 */
int font_init(void);

/*
 * Cleanup font resources
 */
void font_cleanup(void);

/*
 * Get font by ID
 */
const font_t *font_get(int font_id);

/*
 * Get glyph for a codepoint (uses default font)
 * Returns glyph or NULL if not found
 */
const glyph_t *font_get_glyph(uint32_t codepoint);

/*
 * Get glyph for a codepoint from a specific font
 * Returns glyph or NULL if not found
 */
const glyph_t *font_get_glyph_from_font(const font_t *font, uint32_t codepoint);

/*
 * Decode UTF-8 sequence and advance pointer
 * Returns codepoint
 */
uint32_t font_utf8_decode(const char **ptr);

/*
 * Calculate text width in pixels for given font size
 */
int font_text_width(const char *text, int font_id);

/*
 * Get font height for given font size
 */
int font_get_height(int font_id);

/*
 * Calculate text height in pixels (including newlines)
 */
int font_text_height(const char *text, int font_id);

#endif /* PWNAUI_FONT_H */
