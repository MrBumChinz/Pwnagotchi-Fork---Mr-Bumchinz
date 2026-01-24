/*
 * PwnaUI Font Module Tests
 * Tests for bitmap font rendering and UTF-8 support
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "test_framework.h"
#include "../src/font.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Font Initialization Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(font_init_returns_success) {
    int result = font_init();
    ASSERT_EQUAL(0, result);
}

TEST(font_cleanup_does_not_crash) {
    font_init();
    font_cleanup();
    /* If we get here, cleanup succeeded */
    ASSERT_TRUE(1);
}

TEST(font_can_reinitialize) {
    int result1 = font_init();
    font_cleanup();
    int result2 = font_init();
    ASSERT_EQUAL(0, result1);
    ASSERT_EQUAL(0, result2);
    font_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Glyph Retrieval Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(font_get_glyph_ascii_space) {
    font_init();
    const glyph_t *glyph = font_get_glyph(' ');
    ASSERT_NOT_NULL(glyph);
    ASSERT_EQUAL(' ', glyph->codepoint);
    ASSERT_TRUE(glyph->width > 0);
    ASSERT_TRUE(glyph->height > 0);
    ASSERT_NOT_NULL(glyph->bitmap);
    font_cleanup();
}

TEST(font_get_glyph_ascii_letter_A) {
    font_init();
    const glyph_t *glyph = font_get_glyph('A');
    ASSERT_NOT_NULL(glyph);
    ASSERT_EQUAL('A', glyph->codepoint);
    ASSERT_TRUE(glyph->width > 0);
    font_cleanup();
}

TEST(font_get_glyph_ascii_letter_lowercase_z) {
    font_init();
    const glyph_t *glyph = font_get_glyph('z');
    ASSERT_NOT_NULL(glyph);
    ASSERT_EQUAL('z', glyph->codepoint);
    font_cleanup();
}

TEST(font_get_glyph_ascii_digit_0) {
    font_init();
    const glyph_t *glyph = font_get_glyph('0');
    ASSERT_NOT_NULL(glyph);
    ASSERT_EQUAL('0', glyph->codepoint);
    font_cleanup();
}

TEST(font_get_glyph_ascii_digit_9) {
    font_init();
    const glyph_t *glyph = font_get_glyph('9');
    ASSERT_NOT_NULL(glyph);
    ASSERT_EQUAL('9', glyph->codepoint);
    font_cleanup();
}

TEST(font_get_glyph_ascii_punctuation) {
    font_init();
    
    const glyph_t *exclaim = font_get_glyph('!');
    ASSERT_NOT_NULL(exclaim);
    
    const glyph_t *question = font_get_glyph('?');
    ASSERT_NOT_NULL(question);
    
    const glyph_t *at = font_get_glyph('@');
    ASSERT_NOT_NULL(at);
    
    font_cleanup();
}

TEST(font_get_glyph_all_printable_ascii) {
    font_init();
    int count = 0;
    
    /* Test all printable ASCII characters (32-126) */
    for (int c = 32; c <= 126; c++) {
        const glyph_t *glyph = font_get_glyph(c);
        if (glyph != NULL) {
            count++;
        }
    }
    
    /* Should have glyphs for all 95 printable ASCII chars */
    ASSERT_EQUAL(95, count);
    font_cleanup();
}

TEST(font_get_glyph_nonprintable_returns_fallback) {
    font_init();
    /* Non-printable character should return fallback or NULL */
    const glyph_t *glyph = font_get_glyph(1);  /* SOH - non-printable */
    /* Either NULL or a valid fallback glyph is acceptable */
    ASSERT_TRUE(glyph == NULL || glyph->width > 0);
    font_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * UTF-8 Decoding Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(font_utf8_decode_ascii) {
    font_init();
    const char *text = "A";
    const char *ptr = text;
    uint32_t codepoint = font_utf8_decode(&ptr);
    ASSERT_EQUAL('A', codepoint);
    ASSERT_EQUAL(text + 1, ptr);  /* Should advance by 1 */
    font_cleanup();
}

TEST(font_utf8_decode_two_byte) {
    font_init();
    /* UTF-8 for © (U+00A9): C2 A9 */
    const char text[] = { 0xC2, 0xA9, 0x00 };
    const char *ptr = text;
    uint32_t codepoint = font_utf8_decode(&ptr);
    ASSERT_EQUAL(0x00A9, codepoint);
    ASSERT_EQUAL(text + 2, ptr);  /* Should advance by 2 */
    font_cleanup();
}

TEST(font_utf8_decode_three_byte) {
    font_init();
    /* UTF-8 for ◕ (U+25D5): E2 97 95 */
    const char text[] = { 0xE2, 0x97, 0x95, 0x00 };
    const char *ptr = text;
    uint32_t codepoint = font_utf8_decode(&ptr);
    ASSERT_EQUAL(0x25D5, codepoint);
    ASSERT_EQUAL(text + 3, ptr);  /* Should advance by 3 */
    font_cleanup();
}

TEST(font_utf8_decode_face_character_smile) {
    font_init();
    /* UTF-8 for ‿ (U+203F - undertie/smile): E2 80 BF */
    const char text[] = { 0xE2, 0x80, 0xBF, 0x00 };
    const char *ptr = text;
    uint32_t codepoint = font_utf8_decode(&ptr);
    ASSERT_EQUAL(0x203F, codepoint);
    font_cleanup();
}

TEST(font_utf8_decode_multiple_chars) {
    font_init();
    const char *text = "ABC";
    const char *ptr = text;
    
    ASSERT_EQUAL('A', font_utf8_decode(&ptr));
    ASSERT_EQUAL('B', font_utf8_decode(&ptr));
    ASSERT_EQUAL('C', font_utf8_decode(&ptr));
    ASSERT_EQUAL(text + 3, ptr);
    font_cleanup();
}

TEST(font_utf8_decode_mixed_ascii_unicode) {
    font_init();
    /* "A◕B" - mixed ASCII and Unicode */
    const char text[] = { 'A', 0xE2, 0x97, 0x95, 'B', 0x00 };
    const char *ptr = text;
    
    ASSERT_EQUAL('A', font_utf8_decode(&ptr));
    ASSERT_EQUAL(0x25D5, font_utf8_decode(&ptr));  /* ◕ */
    ASSERT_EQUAL('B', font_utf8_decode(&ptr));
    font_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Unicode Face Glyph Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(font_has_glyph_for_filled_eye) {
    font_init();
    /* U+25D5 - Circle with upper right quadrant black (◕) */
    const glyph_t *glyph = font_get_glyph(0x25D5);
    ASSERT_NOT_NULL(glyph);
    ASSERT_EQUAL(0x25D5, glyph->codepoint);
    font_cleanup();
}

TEST(font_has_glyph_for_undertie) {
    font_init();
    /* U+203F - Undertie / smile curve (‿) */
    const glyph_t *glyph = font_get_glyph(0x203F);
    ASSERT_NOT_NULL(glyph);
    ASSERT_EQUAL(0x203F, glyph->codepoint);
    font_cleanup();
}

TEST(font_has_glyph_for_dotted_eye) {
    font_init();
    /* U+2686 - White circle with dot right (⚆) */
    const glyph_t *glyph = font_get_glyph(0x2686);
    ASSERT_NOT_NULL(glyph);
    font_cleanup();
}

TEST(font_has_glyph_for_sun) {
    font_init();
    /* U+2609 - Sun / dotted circle (☉) */
    const glyph_t *glyph = font_get_glyph(0x2609);
    ASSERT_NOT_NULL(glyph);
    font_cleanup();
}

TEST(font_has_glyph_for_degree) {
    font_init();
    /* U+00B0 - Degree sign (°) */
    const glyph_t *glyph = font_get_glyph(0x00B0);
    ASSERT_NOT_NULL(glyph);
    font_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Text Width Calculation Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(font_text_width_empty_string) {
    font_init();
    int width = font_text_width("", FONT_SMALL);
    ASSERT_EQUAL(0, width);
    font_cleanup();
}

TEST(font_text_width_single_char) {
    font_init();
    int width = font_text_width("A", FONT_SMALL);
    ASSERT_TRUE(width > 0);
    ASSERT_TRUE(width <= 10);  /* Reasonable max width for a character */
    font_cleanup();
}

TEST(font_text_width_multiple_chars) {
    font_init();
    int width1 = font_text_width("A", FONT_SMALL);
    int width3 = font_text_width("ABC", FONT_SMALL);
    ASSERT_TRUE(width3 >= width1 * 3);  /* Width should scale with chars */
    font_cleanup();
}

TEST(font_text_width_different_sizes) {
    font_init();
    int small = font_text_width("Test", FONT_SMALL);
    int medium = font_text_width("Test", FONT_MEDIUM);
    int large = font_text_width("Test", FONT_LARGE);
    
    /* Larger fonts should generally have larger widths */
    ASSERT_TRUE(medium >= small);
    ASSERT_TRUE(large >= medium);
    font_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Text Height Calculation Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(font_get_height_small) {
    font_init();
    int height = font_get_height(FONT_SMALL);
    ASSERT_RANGE(height, 5, 10);  /* Small font should be 5-10 pixels */
    font_cleanup();
}

TEST(font_get_height_medium) {
    font_init();
    int height = font_get_height(FONT_MEDIUM);
    ASSERT_RANGE(height, 8, 16);  /* Medium font should be 8-16 pixels */
    font_cleanup();
}

TEST(font_get_height_large) {
    font_init();
    int height = font_get_height(FONT_LARGE);
    ASSERT_RANGE(height, 12, 24);  /* Large font should be 12-24 pixels */
    font_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Glyph Bitmap Validity Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(font_glyph_bitmap_not_all_zeros_for_A) {
    font_init();
    const glyph_t *glyph = font_get_glyph('A');
    ASSERT_NOT_NULL(glyph);
    
    /* At least some bits should be set */
    int has_bits = 0;
    size_t bytes = (glyph->width + 7) / 8 * glyph->height;
    for (size_t i = 0; i < bytes && !has_bits; i++) {
        if (glyph->bitmap[i] != 0) has_bits = 1;
    }
    ASSERT_TRUE(has_bits);
    font_cleanup();
}

TEST(font_glyph_bitmap_space_is_mostly_empty) {
    font_init();
    const glyph_t *glyph = font_get_glyph(' ');
    ASSERT_NOT_NULL(glyph);
    
    /* Space should be all zeros */
    int is_empty = 1;
    size_t bytes = (glyph->width + 7) / 8 * glyph->height;
    for (size_t i = 0; i < bytes && is_empty; i++) {
        if (glyph->bitmap[i] != 0) is_empty = 0;
    }
    ASSERT_TRUE(is_empty);
    font_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test Suite Runner
 * ═══════════════════════════════════════════════════════════════════════════ */

void run_suite_font(void) {
    printf("\n");
    printf("Font Module Tests\n");
    printf("=================\n");
    /* Initialization tests */
    RUN_TEST(font_init_returns_success);
    RUN_TEST(font_cleanup_does_not_crash);
    RUN_TEST(font_can_reinitialize);
    
    /* Glyph retrieval tests */
    RUN_TEST(font_get_glyph_ascii_space);
    RUN_TEST(font_get_glyph_ascii_letter_A);
    RUN_TEST(font_get_glyph_ascii_letter_lowercase_z);
    RUN_TEST(font_get_glyph_ascii_digit_0);
    RUN_TEST(font_get_glyph_ascii_digit_9);
    RUN_TEST(font_get_glyph_ascii_punctuation);
    RUN_TEST(font_get_glyph_all_printable_ascii);
    RUN_TEST(font_get_glyph_nonprintable_returns_fallback);
    
    /* UTF-8 decoding tests */
    RUN_TEST(font_utf8_decode_ascii);
    RUN_TEST(font_utf8_decode_two_byte);
    RUN_TEST(font_utf8_decode_three_byte);
    RUN_TEST(font_utf8_decode_face_character_smile);
    RUN_TEST(font_utf8_decode_multiple_chars);
    RUN_TEST(font_utf8_decode_mixed_ascii_unicode);
    
    /* Unicode face glyph tests */
    RUN_TEST(font_has_glyph_for_filled_eye);
    RUN_TEST(font_has_glyph_for_undertie);
    RUN_TEST(font_has_glyph_for_dotted_eye);
    RUN_TEST(font_has_glyph_for_sun);
    RUN_TEST(font_has_glyph_for_degree);
    
    /* Text width tests */
    RUN_TEST(font_text_width_empty_string);
    RUN_TEST(font_text_width_single_char);
    RUN_TEST(font_text_width_multiple_chars);
    RUN_TEST(font_text_width_different_sizes);
    
    /* Text height tests */
    RUN_TEST(font_get_height_small);
    RUN_TEST(font_get_height_medium);
    RUN_TEST(font_get_height_large);
    
    /* Glyph bitmap validity tests */
    RUN_TEST(font_glyph_bitmap_not_all_zeros_for_A);
    RUN_TEST(font_glyph_bitmap_space_is_mostly_empty);
}

#ifndef TEST_ALL
int main(void) {
    printf("PwnaUI Font Module Tests\n");
    printf("========================\n");
    
    run_suite_font();
    
    test_print_summary();
    return test_exit_code();
}
#endif
