/*
 * PwnaUI - Font Implementation
 * Bitmap font rendering with UTF-8 support for Pwnagotchi faces
 * 
 * Includes built-in bitmap fonts optimized for e-ink displays.
 * Supports ASCII + common Unicode symbols used in Pwnagotchi faces.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "font.h"

/*
 * 5x7 Small Font Bitmaps (ASCII 32-126)
 * Each character is 5 pixels wide, 7 pixels tall
 * Packed as 1 byte per row (MSB first, 3 unused bits)
 */
static const uint8_t font_small_32[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };  /* space */
static const uint8_t font_small_33[] = { 0x20, 0x20, 0x20, 0x20, 0x00, 0x20, 0x00 };  /* ! */
static const uint8_t font_small_34[] = { 0x50, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00 };  /* " */
static const uint8_t font_small_35[] = { 0x50, 0xF8, 0x50, 0x50, 0xF8, 0x50, 0x00 };  /* # */
static const uint8_t font_small_36[] = { 0x20, 0x78, 0xA0, 0x70, 0x28, 0xF0, 0x20 };  /* $ */
static const uint8_t font_small_37[] = { 0xC0, 0xC8, 0x10, 0x20, 0x40, 0x98, 0x18 };  /* % */
static const uint8_t font_small_38[] = { 0x40, 0xA0, 0x40, 0xA8, 0x90, 0x68, 0x00 };  /* & */
static const uint8_t font_small_39[] = { 0x20, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00 };  /* ' */
static const uint8_t font_small_40[] = { 0x10, 0x20, 0x40, 0x40, 0x40, 0x20, 0x10 };  /* ( */
static const uint8_t font_small_41[] = { 0x40, 0x20, 0x10, 0x10, 0x10, 0x20, 0x40 };  /* ) */
static const uint8_t font_small_42[] = { 0x00, 0x20, 0xA8, 0x70, 0xA8, 0x20, 0x00 };  /* * */
static const uint8_t font_small_43[] = { 0x00, 0x20, 0x20, 0xF8, 0x20, 0x20, 0x00 };  /* + */
static const uint8_t font_small_44[] = { 0x00, 0x00, 0x00, 0x00, 0x20, 0x20, 0x40 };  /* , */
static const uint8_t font_small_45[] = { 0x00, 0x00, 0x00, 0xF8, 0x00, 0x00, 0x00 };  /* - */
static const uint8_t font_small_46[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00 };  /* . */
static const uint8_t font_small_47[] = { 0x00, 0x08, 0x10, 0x20, 0x40, 0x80, 0x00 };  /* / */
static const uint8_t font_small_48[] = { 0x70, 0x88, 0x98, 0xA8, 0xC8, 0x70, 0x00 };  /* 0 */
static const uint8_t font_small_49[] = { 0x20, 0x60, 0x20, 0x20, 0x20, 0x70, 0x00 };  /* 1 */
static const uint8_t font_small_50[] = { 0x70, 0x88, 0x08, 0x30, 0x40, 0xF8, 0x00 };  /* 2 */
static const uint8_t font_small_51[] = { 0xF8, 0x10, 0x30, 0x08, 0x88, 0x70, 0x00 };  /* 3 */
static const uint8_t font_small_52[] = { 0x10, 0x30, 0x50, 0x90, 0xF8, 0x10, 0x00 };  /* 4 */
static const uint8_t font_small_53[] = { 0xF8, 0x80, 0xF0, 0x08, 0x88, 0x70, 0x00 };  /* 5 */
static const uint8_t font_small_54[] = { 0x30, 0x40, 0xF0, 0x88, 0x88, 0x70, 0x00 };  /* 6 */
static const uint8_t font_small_55[] = { 0xF8, 0x08, 0x10, 0x20, 0x40, 0x40, 0x00 };  /* 7 */
static const uint8_t font_small_56[] = { 0x70, 0x88, 0x70, 0x88, 0x88, 0x70, 0x00 };  /* 8 */
static const uint8_t font_small_57[] = { 0x70, 0x88, 0x88, 0x78, 0x10, 0x60, 0x00 };  /* 9 */
static const uint8_t font_small_58[] = { 0x00, 0x20, 0x00, 0x00, 0x20, 0x00, 0x00 };  /* : */
static const uint8_t font_small_59[] = { 0x00, 0x20, 0x00, 0x00, 0x20, 0x20, 0x40 };  /* ; */
static const uint8_t font_small_60[] = { 0x08, 0x10, 0x20, 0x40, 0x20, 0x10, 0x08 };  /* < */
static const uint8_t font_small_61[] = { 0x00, 0x00, 0xF8, 0x00, 0xF8, 0x00, 0x00 };  /* = */
static const uint8_t font_small_62[] = { 0x80, 0x40, 0x20, 0x10, 0x20, 0x40, 0x80 };  /* > */
static const uint8_t font_small_63[] = { 0x70, 0x88, 0x10, 0x20, 0x00, 0x20, 0x00 };  /* ? */
static const uint8_t font_small_64[] = { 0x70, 0x88, 0xB8, 0xB8, 0x80, 0x78, 0x00 };  /* @ */
/* A-Z */
static const uint8_t font_small_65[] = { 0x70, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x00 };  /* A */
static const uint8_t font_small_66[] = { 0xF0, 0x88, 0xF0, 0x88, 0x88, 0xF0, 0x00 };  /* B */
static const uint8_t font_small_67[] = { 0x70, 0x88, 0x80, 0x80, 0x88, 0x70, 0x00 };  /* C */
static const uint8_t font_small_68[] = { 0xE0, 0x90, 0x88, 0x88, 0x90, 0xE0, 0x00 };  /* D */
static const uint8_t font_small_69[] = { 0xF8, 0x80, 0xF0, 0x80, 0x80, 0xF8, 0x00 };  /* E */
static const uint8_t font_small_70[] = { 0xF8, 0x80, 0xF0, 0x80, 0x80, 0x80, 0x00 };  /* F */
static const uint8_t font_small_71[] = { 0x70, 0x88, 0x80, 0xB8, 0x88, 0x70, 0x00 };  /* G */
static const uint8_t font_small_72[] = { 0x88, 0x88, 0xF8, 0x88, 0x88, 0x88, 0x00 };  /* H */
static const uint8_t font_small_73[] = { 0x70, 0x20, 0x20, 0x20, 0x20, 0x70, 0x00 };  /* I */
static const uint8_t font_small_74[] = { 0x38, 0x10, 0x10, 0x10, 0x90, 0x60, 0x00 };  /* J */
static const uint8_t font_small_75[] = { 0x88, 0x90, 0xE0, 0x90, 0x88, 0x88, 0x00 };  /* K */
static const uint8_t font_small_76[] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0xF8, 0x00 };  /* L */
static const uint8_t font_small_77[] = { 0x88, 0xD8, 0xA8, 0x88, 0x88, 0x88, 0x00 };  /* M */
static const uint8_t font_small_78[] = { 0x88, 0xC8, 0xA8, 0x98, 0x88, 0x88, 0x00 };  /* N */
static const uint8_t font_small_79[] = { 0x70, 0x88, 0x88, 0x88, 0x88, 0x70, 0x00 };  /* O */
static const uint8_t font_small_80[] = { 0xF0, 0x88, 0x88, 0xF0, 0x80, 0x80, 0x00 };  /* P */
static const uint8_t font_small_81[] = { 0x70, 0x88, 0x88, 0xA8, 0x90, 0x68, 0x00 };  /* Q */
static const uint8_t font_small_82[] = { 0xF0, 0x88, 0x88, 0xF0, 0x88, 0x88, 0x00 };  /* R */
static const uint8_t font_small_83[] = { 0x70, 0x88, 0x40, 0x20, 0x88, 0x70, 0x00 };  /* S */
static const uint8_t font_small_84[] = { 0xF8, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00 };  /* T */
static const uint8_t font_small_85[] = { 0x88, 0x88, 0x88, 0x88, 0x88, 0x70, 0x00 };  /* U */
static const uint8_t font_small_86[] = { 0x88, 0x88, 0x88, 0x50, 0x50, 0x20, 0x00 };  /* V */
static const uint8_t font_small_87[] = { 0x88, 0x88, 0x88, 0xA8, 0xA8, 0x50, 0x00 };  /* W */
static const uint8_t font_small_88[] = { 0x88, 0x50, 0x20, 0x20, 0x50, 0x88, 0x00 };  /* X */
static const uint8_t font_small_89[] = { 0x88, 0x50, 0x20, 0x20, 0x20, 0x20, 0x00 };  /* Y */
static const uint8_t font_small_90[] = { 0xF8, 0x08, 0x10, 0x20, 0x40, 0xF8, 0x00 };  /* Z */
/* [ \ ] ^ _ ` */
static const uint8_t font_small_91[] = { 0x70, 0x40, 0x40, 0x40, 0x40, 0x70, 0x00 };  /* [ */
static const uint8_t font_small_92[] = { 0x00, 0x80, 0x40, 0x20, 0x10, 0x08, 0x00 };  /* \ */
static const uint8_t font_small_93[] = { 0x70, 0x10, 0x10, 0x10, 0x10, 0x70, 0x00 };  /* ] */
static const uint8_t font_small_94[] = { 0x20, 0x50, 0x88, 0x00, 0x00, 0x00, 0x00 };  /* ^ */
static const uint8_t font_small_95[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x00 };  /* _ */
static const uint8_t font_small_96[] = { 0x40, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00 };  /* ` */
/* a-z */
static const uint8_t font_small_97[] = { 0x00, 0x00, 0x70, 0x08, 0x78, 0x78, 0x00 };   /* a */
static const uint8_t font_small_98[] = { 0x80, 0x80, 0xB0, 0xC8, 0x88, 0xF0, 0x00 };   /* b */
static const uint8_t font_small_99[] = { 0x00, 0x00, 0x70, 0x80, 0x80, 0x70, 0x00 };   /* c */
static const uint8_t font_small_100[] = { 0x08, 0x08, 0x68, 0x98, 0x88, 0x78, 0x00 };  /* d */
static const uint8_t font_small_101[] = { 0x00, 0x00, 0x70, 0xF8, 0x80, 0x70, 0x00 };  /* e */
static const uint8_t font_small_102[] = { 0x30, 0x48, 0x40, 0xE0, 0x40, 0x40, 0x00 };  /* f */
static const uint8_t font_small_103[] = { 0x00, 0x00, 0x78, 0x88, 0x78, 0x08, 0x70 };  /* g */
static const uint8_t font_small_104[] = { 0x80, 0x80, 0xB0, 0xC8, 0x88, 0x88, 0x00 };  /* h */
static const uint8_t font_small_105[] = { 0x20, 0x00, 0x60, 0x20, 0x20, 0x70, 0x00 };  /* i */
static const uint8_t font_small_106[] = { 0x10, 0x00, 0x30, 0x10, 0x10, 0x90, 0x60 };  /* j */
static const uint8_t font_small_107[] = { 0x80, 0x80, 0x90, 0xE0, 0x90, 0x88, 0x00 };  /* k */
static const uint8_t font_small_108[] = { 0x60, 0x20, 0x20, 0x20, 0x20, 0x70, 0x00 };  /* l */
static const uint8_t font_small_109[] = { 0x00, 0x00, 0xD0, 0xA8, 0xA8, 0x88, 0x00 };  /* m */
static const uint8_t font_small_110[] = { 0x00, 0x00, 0xB0, 0xC8, 0x88, 0x88, 0x00 };  /* n */
static const uint8_t font_small_111[] = { 0x00, 0x00, 0x70, 0x88, 0x88, 0x70, 0x00 };  /* o */
static const uint8_t font_small_112[] = { 0x00, 0x00, 0xF0, 0x88, 0xF0, 0x80, 0x80 };  /* p */
static const uint8_t font_small_113[] = { 0x00, 0x00, 0x78, 0x88, 0x78, 0x08, 0x08 };  /* q */
static const uint8_t font_small_114[] = { 0x00, 0x00, 0xB0, 0xC8, 0x80, 0x80, 0x00 };  /* r */
static const uint8_t font_small_115[] = { 0x00, 0x00, 0x78, 0xC0, 0x18, 0xF0, 0x00 };  /* s */
static const uint8_t font_small_116[] = { 0x40, 0x40, 0xE0, 0x40, 0x48, 0x30, 0x00 };  /* t */
static const uint8_t font_small_117[] = { 0x00, 0x00, 0x88, 0x88, 0x98, 0x68, 0x00 };  /* u */
static const uint8_t font_small_118[] = { 0x00, 0x00, 0x88, 0x88, 0x50, 0x20, 0x00 };  /* v */
static const uint8_t font_small_119[] = { 0x00, 0x00, 0x88, 0xA8, 0xA8, 0x50, 0x00 };  /* w */
static const uint8_t font_small_120[] = { 0x00, 0x00, 0x88, 0x50, 0x50, 0x88, 0x00 };  /* x */
static const uint8_t font_small_121[] = { 0x00, 0x00, 0x88, 0x88, 0x78, 0x08, 0x70 };  /* y */
static const uint8_t font_small_122[] = { 0x00, 0x00, 0xF8, 0x10, 0x40, 0xF8, 0x00 };  /* z */
/* { | } ~ */
static const uint8_t font_small_123[] = { 0x18, 0x20, 0x60, 0x20, 0x20, 0x18, 0x00 };  /* { */
static const uint8_t font_small_124[] = { 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00 };  /* | */
static const uint8_t font_small_125[] = { 0xC0, 0x20, 0x30, 0x20, 0x20, 0xC0, 0x00 };  /* } */
static const uint8_t font_small_126[] = { 0x00, 0x00, 0x48, 0xA8, 0x90, 0x00, 0x00 };  /* ~ */

/*
 * Extended Unicode glyphs for Pwnagotchi faces
 * These are commonly used symbols in the face expressions
 */

/* ◕ U+25D5 - Circle with upper right quadrant black (8x8) */
static const uint8_t glyph_25D5[] = { 
    0x3C, 0x42, 0x9D, 0xBD, 0xBD, 0x81, 0x42, 0x3C 
};

/* ‿ U+203F - Undertie / smile curve (8x4) */
static const uint8_t glyph_203F[] = { 
    0x00, 0x81, 0x42, 0x3C 
};

/* ⚆ U+2686 - White circle with dot right (8x8) */
static const uint8_t glyph_2686[] = { 
    0x3C, 0x42, 0x81, 0x83, 0x83, 0x81, 0x42, 0x3C 
};

/* ☉ U+2609 - Sun / dotted circle (8x8) */
static const uint8_t glyph_2609[] = { 
    0x3C, 0x42, 0x81, 0x99, 0x99, 0x81, 0x42, 0x3C 
};

/* ⇀ U+21C0 - Rightwards harpoon (8x5) */
static const uint8_t glyph_21C0[] = { 
    0x00, 0x08, 0x04, 0xFE, 0x04 
};

/* ↼ U+21BC - Leftwards harpoon (8x5) */
static const uint8_t glyph_21BC[] = { 
    0x00, 0x20, 0x40, 0xFE, 0x40 
};

/* ≖ U+2256 - Ring in equal to (8x5) */
static const uint8_t glyph_2256[] = { 
    0x7E, 0x00, 0x18, 0x00, 0x7E 
};

/* ° U+00B0 - Degree sign (5x5) */
static const uint8_t glyph_00B0[] = { 
    0x60, 0x90, 0x90, 0x60, 0x00 
};

/* ▃ U+2583 - Lower three eighths block (8x8) */
static const uint8_t glyph_2583[] = { 
    0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF 
};

/* ⌐ U+2310 - Reversed not sign (6x6) */
static const uint8_t glyph_2310[] = { 
    0xFC, 0x04, 0x04, 0x04, 0x04, 0x04 
};

/* ■ U+25A0 - Black square (8x8) */
static const uint8_t glyph_25A0[] = { 
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF 
};

/* • U+2022 - Bullet / filled circle (7x7) - better for scaling */
static const uint8_t glyph_2022[] = { 
    0x38, 0x7C, 0xFE, 0xFE, 0xFE, 0x7C, 0x38
};

/* ^ U+005E - Circumflex (handled in ASCII) */

/* ╥ U+2565 - Box drawings down double (8x8) */
static const uint8_t glyph_2565[] = { 
    0x14, 0x14, 0xF4, 0x04, 0xF4, 0x14, 0x14, 0x14 
};

/* ☁ U+2601 - Cloud (10x7) */
static const uint8_t glyph_2601[] = { 
    0x0E, 0x00, 0x1F, 0x00, 0x3F, 0x80, 0x7F, 0xC0, 
    0xFF, 0xE0, 0xFF, 0xE0, 0x7F, 0xC0 
};

/* ☼ U+263C - Sun with rays (10x10) */
static const uint8_t glyph_263C[] = { 
    0x08, 0x00, 0x49, 0x00, 0x2A, 0x00, 0x1C, 0x00, 
    0xF7, 0xC0, 0x1C, 0x00, 0x2A, 0x00, 0x49, 0x00, 
    0x08, 0x00, 0x00, 0x00 
};

/* ✜ U+271C - Heavy Greek cross (8x8) */
static const uint8_t glyph_271C[] = { 
    0x18, 0x18, 0x18, 0xFF, 0xFF, 0x18, 0x18, 0x18 
};

/* ب U+0628 - Arabic Ba (8x10) */
static const uint8_t glyph_0628[] = { 
    0x00, 0x00, 0x7E, 0x42, 0x42, 0x7E, 0x00, 0x18, 
    0x18, 0x00 
};

/* ♥ U+2665 - Heart (8x7) */
static const uint8_t glyph_2665[] = { 
    0x66, 0xFF, 0xFF, 0xFF, 0x7E, 0x3C, 0x18 
};

/* ☓ U+2613 - Saltire / X mark (8x8) */
static const uint8_t glyph_2613[] = { 
    0xC3, 0x66, 0x3C, 0x18, 0x18, 0x3C, 0x66, 0xC3 
};

/* ᵔ U+1D54 - Modifier letter small o (6x4) */
static const uint8_t glyph_1D54[] = { 
    0x30, 0x48, 0x48, 0x30 
};

/* ◡ U+25E1 - Lower half circle (8x4) */
static const uint8_t glyph_25E1[] = { 
    0x81, 0x42, 0x24, 0x18 
};

/* █ U+2588 - Full block (8x10) */
static const uint8_t glyph_2588[] = { 
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 
    0xFF, 0xFF 
};

/* ▌ U+258C - Left half block (4x10) */
static const uint8_t glyph_258C[] = { 
    0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 
    0xF0, 0xF0 
};

/* │ U+2502 - Box light vertical (2x10) */
static const uint8_t glyph_2502[] = { 
    0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 
    0xC0, 0xC0 
};

/*
 * Glyph arrays for each font
 */
static const glyph_t font_small_glyphs[] = {
    { 32, 5, 7, 0, 0, 6, font_small_32 },
    { 33, 5, 7, 0, 0, 6, font_small_33 },
    { 34, 5, 7, 0, 0, 6, font_small_34 },
    { 35, 5, 7, 0, 0, 6, font_small_35 },
    { 36, 5, 7, 0, 0, 6, font_small_36 },
    { 37, 5, 7, 0, 0, 6, font_small_37 },
    { 38, 5, 7, 0, 0, 6, font_small_38 },
    { 39, 5, 7, 0, 0, 6, font_small_39 },
    { 40, 5, 7, 0, 0, 6, font_small_40 },
    { 41, 5, 7, 0, 0, 6, font_small_41 },
    { 42, 5, 7, 0, 0, 6, font_small_42 },
    { 43, 5, 7, 0, 0, 6, font_small_43 },
    { 44, 5, 7, 0, 0, 6, font_small_44 },
    { 45, 5, 7, 0, 0, 6, font_small_45 },
    { 46, 5, 7, 0, 0, 6, font_small_46 },
    { 47, 5, 7, 0, 0, 6, font_small_47 },
    { 48, 5, 7, 0, 0, 6, font_small_48 },
    { 49, 5, 7, 0, 0, 6, font_small_49 },
    { 50, 5, 7, 0, 0, 6, font_small_50 },
    { 51, 5, 7, 0, 0, 6, font_small_51 },
    { 52, 5, 7, 0, 0, 6, font_small_52 },
    { 53, 5, 7, 0, 0, 6, font_small_53 },
    { 54, 5, 7, 0, 0, 6, font_small_54 },
    { 55, 5, 7, 0, 0, 6, font_small_55 },
    { 56, 5, 7, 0, 0, 6, font_small_56 },
    { 57, 5, 7, 0, 0, 6, font_small_57 },
    { 58, 5, 7, 0, 0, 6, font_small_58 },
    { 59, 5, 7, 0, 0, 6, font_small_59 },
    { 60, 5, 7, 0, 0, 6, font_small_60 },
    { 61, 5, 7, 0, 0, 6, font_small_61 },
    { 62, 5, 7, 0, 0, 6, font_small_62 },
    { 63, 5, 7, 0, 0, 6, font_small_63 },
    { 64, 5, 7, 0, 0, 6, font_small_64 },
    { 65, 5, 7, 0, 0, 6, font_small_65 },
    { 66, 5, 7, 0, 0, 6, font_small_66 },
    { 67, 5, 7, 0, 0, 6, font_small_67 },
    { 68, 5, 7, 0, 0, 6, font_small_68 },
    { 69, 5, 7, 0, 0, 6, font_small_69 },
    { 70, 5, 7, 0, 0, 6, font_small_70 },
    { 71, 5, 7, 0, 0, 6, font_small_71 },
    { 72, 5, 7, 0, 0, 6, font_small_72 },
    { 73, 5, 7, 0, 0, 6, font_small_73 },
    { 74, 5, 7, 0, 0, 6, font_small_74 },
    { 75, 5, 7, 0, 0, 6, font_small_75 },
    { 76, 5, 7, 0, 0, 6, font_small_76 },
    { 77, 5, 7, 0, 0, 6, font_small_77 },
    { 78, 5, 7, 0, 0, 6, font_small_78 },
    { 79, 5, 7, 0, 0, 6, font_small_79 },
    { 80, 5, 7, 0, 0, 6, font_small_80 },
    { 81, 5, 7, 0, 0, 6, font_small_81 },
    { 82, 5, 7, 0, 0, 6, font_small_82 },
    { 83, 5, 7, 0, 0, 6, font_small_83 },
    { 84, 5, 7, 0, 0, 6, font_small_84 },
    { 85, 5, 7, 0, 0, 6, font_small_85 },
    { 86, 5, 7, 0, 0, 6, font_small_86 },
    { 87, 5, 7, 0, 0, 6, font_small_87 },
    { 88, 5, 7, 0, 0, 6, font_small_88 },
    { 89, 5, 7, 0, 0, 6, font_small_89 },
    { 90, 5, 7, 0, 0, 6, font_small_90 },
    { 91, 5, 7, 0, 0, 6, font_small_91 },
    { 92, 5, 7, 0, 0, 6, font_small_92 },
    { 93, 5, 7, 0, 0, 6, font_small_93 },
    { 94, 5, 7, 0, 0, 6, font_small_94 },
    { 95, 5, 7, 0, 0, 6, font_small_95 },
    { 96, 5, 7, 0, 0, 6, font_small_96 },
    { 97, 5, 7, 0, 0, 6, font_small_97 },
    { 98, 5, 7, 0, 0, 6, font_small_98 },
    { 99, 5, 7, 0, 0, 6, font_small_99 },
    { 100, 5, 7, 0, 0, 6, font_small_100 },
    { 101, 5, 7, 0, 0, 6, font_small_101 },
    { 102, 5, 7, 0, 0, 6, font_small_102 },
    { 103, 5, 7, 0, 0, 6, font_small_103 },
    { 104, 5, 7, 0, 0, 6, font_small_104 },
    { 105, 5, 7, 0, 0, 6, font_small_105 },
    { 106, 5, 7, 0, 0, 6, font_small_106 },
    { 107, 5, 7, 0, 0, 6, font_small_107 },
    { 108, 5, 7, 0, 0, 6, font_small_108 },
    { 109, 5, 7, 0, 0, 6, font_small_109 },
    { 110, 5, 7, 0, 0, 6, font_small_110 },
    { 111, 5, 7, 0, 0, 6, font_small_111 },
    { 112, 5, 7, 0, 0, 6, font_small_112 },
    { 113, 5, 7, 0, 0, 6, font_small_113 },
    { 114, 5, 7, 0, 0, 6, font_small_114 },
    { 115, 5, 7, 0, 0, 6, font_small_115 },
    { 116, 5, 7, 0, 0, 6, font_small_116 },
    { 117, 5, 7, 0, 0, 6, font_small_117 },
    { 118, 5, 7, 0, 0, 6, font_small_118 },
    { 119, 5, 7, 0, 0, 6, font_small_119 },
    { 120, 5, 7, 0, 0, 6, font_small_120 },
    { 121, 5, 7, 0, 0, 6, font_small_121 },
    { 122, 5, 7, 0, 0, 6, font_small_122 },
    { 123, 5, 7, 0, 0, 6, font_small_123 },
    { 124, 5, 7, 0, 0, 6, font_small_124 },
    { 125, 5, 7, 0, 0, 6, font_small_125 },
    { 126, 5, 7, 0, 0, 6, font_small_126 },
    /* Unicode glyphs for faces */
    { 0x25D5, 8, 8, 0, 0, 9, glyph_25D5 },  /* ◕ */
    { 0x203F, 8, 4, 0, 5, 9, glyph_203F },  /* ‿ - y_offset=5 for proper mouth position */
    { 0x2686, 8, 8, 0, 0, 9, glyph_2686 },  /* ⚆ */
    { 0x2609, 8, 8, 0, 0, 9, glyph_2609 },  /* ☉ */
    { 0x21C0, 8, 5, 0, 1, 9, glyph_21C0 },  /* ⇀ */
    { 0x21BC, 8, 5, 0, 1, 9, glyph_21BC },  /* ↼ */
    { 0x2256, 8, 5, 0, 1, 9, glyph_2256 },  /* ≖ */
    { 0x00B0, 5, 5, 0, 0, 6, glyph_00B0 },  /* ° */
    { 0x2583, 8, 8, 0, 0, 9, glyph_2583 },  /* ▃ */
    { 0x2310, 6, 6, 0, 1, 7, glyph_2310 },  /* ⌐ */
    { 0x25A0, 8, 8, 0, 0, 9, glyph_25A0 },  /* ■ */
    { 0x2022, 7, 7, 0, 0, 8, glyph_2022 },  /* • */
    { 0x2565, 8, 8, 0, 0, 9, glyph_2565 },  /* ╥ */
    { 0x2601, 10, 7, 0, 0, 11, glyph_2601 }, /* ☁ */
    { 0x263C, 10, 10, 0, 0, 11, glyph_263C }, /* ☼ */
    { 0x271C, 8, 8, 0, 0, 9, glyph_271C },  /* ✜ */
    { 0x0628, 8, 10, 0, 0, 9, glyph_0628 }, /* ب */
    { 0x2665, 8, 7, 0, 1, 9, glyph_2665 },  /* ♥ */
    { 0x2613, 8, 8, 0, 0, 9, glyph_2613 },  /* ☓ */
    { 0x1D54, 6, 4, 0, 0, 7, glyph_1D54 },  /* ᵔ */
    { 0x25E1, 8, 4, 0, 2, 9, glyph_25E1 },  /* ◡ */
    { 0x2588, 8, 10, 0, 0, 9, glyph_2588 }, /* █ */
    { 0x258C, 4, 10, 0, 0, 5, glyph_258C }, /* ▌ */
    { 0x2502, 2, 10, 0, 0, 3, glyph_2502 }, /* │ */
};

#define FONT_SMALL_NUM_GLYPHS (sizeof(font_small_glyphs) / sizeof(font_small_glyphs[0]))

/*
 * Font definitions
 */
static const font_t g_fonts[FONT_COUNT] = {
    { "small", 5, 7, 6, FONT_SMALL_NUM_GLYPHS, font_small_glyphs },      /* FONT_SMALL */
    { "medium", 6, 9, 7, FONT_SMALL_NUM_GLYPHS, font_small_glyphs },     /* FONT_MEDIUM (scaled) */
    { "bold", 6, 10, 8, FONT_SMALL_NUM_GLYPHS, font_small_glyphs },      /* FONT_BOLD */
    { "bold_small", 5, 8, 6, FONT_SMALL_NUM_GLYPHS, font_small_glyphs }, /* FONT_BOLD_SMALL */
    { "huge", 12, 20, 16, FONT_SMALL_NUM_GLYPHS, font_small_glyphs },    /* FONT_HUGE (for faces) */
};

/*
 * Initialize font system
 */
int font_init(void) {
    /* All fonts are statically allocated, nothing to do */
    return 0;
}

/*
 * Cleanup font resources
 */
void font_cleanup(void) {
    /* Nothing to cleanup with static allocation */
}

/*
 * Get font by ID
 */
const font_t *font_get(int font_id) {
    if (font_id < 0 || font_id >= FONT_COUNT) {
        return &g_fonts[FONT_MEDIUM];  /* Default */
    }
    return &g_fonts[font_id];
}

/*
 * Get glyph for a codepoint (binary search for ASCII, linear for Unicode)
 */
static const glyph_t *font_get_glyph_font(const font_t *font, uint32_t codepoint) {
    if (!font || !font->glyphs) return NULL;
    
    /* ASCII range - direct index */
    if (codepoint >= 32 && codepoint <= 126) {
        int idx = codepoint - 32;
        if (idx < font->num_glyphs && font->glyphs[idx].codepoint == codepoint) {
            return &font->glyphs[idx];
        }
    }
    
    /* Linear search for Unicode glyphs */
    for (int i = 0; i < font->num_glyphs; i++) {
        if (font->glyphs[i].codepoint == codepoint) {
            return &font->glyphs[i];
        }
    }
    
    return NULL;  /* Glyph not found */
}

/*
 * Calculate text width in pixels
 */
static int font_text_width_font(const font_t *font, const char *text) {
    if (!font || !text) return 0;
    
    int width = 0;
    int max_width = 0;
    const uint8_t *p = (const uint8_t *)text;
    
    while (*p) {
        if (*p == '\n') {
            if (width > max_width) max_width = width;
            width = 0;
            p++;
            continue;
        }
        
        uint32_t codepoint;
        int bytes;
        
        /* Decode UTF-8 */
        if ((*p & 0x80) == 0) {
            codepoint = *p;
            bytes = 1;
        } else if ((*p & 0xE0) == 0xC0) {
            codepoint = (*p & 0x1F) << 6 | (p[1] & 0x3F);
            bytes = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            codepoint = (*p & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F);
            bytes = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            codepoint = (*p & 0x07) << 18 | (p[1] & 0x3F) << 12 | (p[2] & 0x3F) << 6 | (p[3] & 0x3F);
            bytes = 4;
        } else {
            codepoint = '?';
            bytes = 1;
        }
        
        p += bytes;
        
        const glyph_t *glyph = font_get_glyph_font(font, codepoint);
        if (glyph) {
            width += glyph->advance;
        } else {
            width += font->width;
        }
    }
    
    return (width > max_width) ? width : max_width;
}

/*
 * Calculate text height in pixels
 */
int font_text_height_font(const font_t *font, const char *text) {
    if (!font || !text) return 0;
    
    int lines = 1;
    while (*text) {
        if (*text == '\n') lines++;
        text++;
    }
    
    return lines * (font->height + 2);
}

/*
 * Wrapper functions using default font
 */
const glyph_t *font_get_glyph(uint32_t codepoint) {
    const font_t *font = font_get(FONT_MEDIUM);
    return font_get_glyph_font(font, codepoint);
}

const glyph_t *font_get_glyph_from_font(const font_t *font, uint32_t codepoint) {
    return font_get_glyph_font(font, codepoint);
}

uint32_t font_utf8_decode(const char **ptr) {
    if (!ptr || !*ptr || !**ptr) return 0;
    
    const uint8_t *p = (const uint8_t *)*ptr;
    uint32_t codepoint;
    int bytes;
    
    if ((*p & 0x80) == 0) {
        codepoint = *p;
        bytes = 1;
    } else if ((*p & 0xE0) == 0xC0) {
        codepoint = (*p & 0x1F) << 6 | (p[1] & 0x3F);
        bytes = 2;
    } else if ((*p & 0xF0) == 0xE0) {
        codepoint = (*p & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F);
        bytes = 3;
    } else if ((*p & 0xF8) == 0xF0) {
        codepoint = (*p & 0x07) << 18 | (p[1] & 0x3F) << 12 | (p[2] & 0x3F) << 6 | (p[3] & 0x3F);
        bytes = 4;
    } else {
        codepoint = '?';
        bytes = 1;
    }
    
    *ptr += bytes;
    return codepoint;
}

int font_text_width(const char *text, int font_id) {
    const font_t *font = font_get(font_id);
    return font_text_width_font(font, text);
}

int font_get_height(int font_id) {
    const font_t *font = font_get(font_id);
    return font ? font->height : 0;
}

int font_text_height(const char *text, int font_id) {
    const font_t *font = font_get(font_id);
    return font_text_height_font(font, text);
}
