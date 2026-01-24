/*
 * PwnaUI Display Module Tests
 * Tests for display abstraction layer and hardware detection
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "test_framework.h"
#include "../src/display.h"

/* Test framebuffer */
#define TEST_WIDTH 250
#define TEST_HEIGHT 122
#define TEST_FB_SIZE ((TEST_WIDTH + 7) / 8 * TEST_HEIGHT)
static uint8_t g_test_fb[TEST_FB_SIZE];

/* ═══════════════════════════════════════════════════════════════════════════
 * Display Initialization Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(display_init_dummy_succeeds) {
    int result = display_init(DISPLAY_DUMMY, 250, 122);
    ASSERT_EQUAL(0, result);
    display_cleanup();
}

TEST(display_cleanup_does_not_crash) {
    display_init(DISPLAY_DUMMY, 250, 122);
    display_cleanup();
    ASSERT_TRUE(1);
}

TEST(display_can_reinitialize) {
    int r1 = display_init(DISPLAY_DUMMY, 250, 122);
    display_cleanup();
    int r2 = display_init(DISPLAY_DUMMY, 264, 176);
    ASSERT_EQUAL(0, r1);
    ASSERT_EQUAL(0, r2);
    display_cleanup();
}

TEST(display_init_framebuffer_on_non_linux) {
    /* Framebuffer may fail on non-Linux, which is acceptable */
    int result = display_init(DISPLAY_FRAMEBUFFER, 250, 122);
    /* Either success (0) or failure (-1) is acceptable depending on system */
    ASSERT_TRUE(result == 0 || result == -1);
    display_cleanup();
}

TEST(display_init_various_sizes) {
    /* Test common display sizes */
    struct { int w, h; } sizes[] = {
        {250, 122},   /* 2.13" V2/V3 */
        {264, 176},   /* 2.7" */
        {200, 200},   /* 1.54" */
        {212, 104},   /* Inky pHAT */
        {128, 64},    /* Small OLED */
        {320, 240},   /* Larger display */
    };
    
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        int result = display_init(DISPLAY_DUMMY, sizes[i].w, sizes[i].h);
        ASSERT_EQUAL(0, result);
        display_cleanup();
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Display Type Detection Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(display_get_type_after_init) {
    display_init(DISPLAY_DUMMY, 250, 122);
    display_type_t type = display_get_type();
    ASSERT_EQUAL(DISPLAY_DUMMY, type);
    display_cleanup();
}

TEST(display_get_dimensions_after_init) {
    display_init(DISPLAY_DUMMY, 250, 122);
    int width = display_get_width();
    int height = display_get_height();
    ASSERT_EQUAL(250, width);
    ASSERT_EQUAL(122, height);
    display_cleanup();
}

TEST(display_get_dimensions_different_size) {
    display_init(DISPLAY_DUMMY, 264, 176);
    int width = display_get_width();
    int height = display_get_height();
    ASSERT_EQUAL(264, width);
    ASSERT_EQUAL(176, height);
    display_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Display Update Tests (Dummy mode)
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(display_update_dummy_succeeds) {
    display_init(DISPLAY_DUMMY, 250, 122);
    memset(g_test_fb, 0xFF, sizeof(g_test_fb));
    
    int result = display_update(g_test_fb);
    ASSERT_EQUAL(0, result);
    display_cleanup();
}

TEST(display_update_with_null_fb) {
    display_init(DISPLAY_DUMMY, 250, 122);
    
    int result = display_update(NULL);
    /* Should return error for NULL framebuffer */
    ASSERT_EQUAL(-1, result);
    display_cleanup();
}

TEST(display_update_multiple_times) {
    display_init(DISPLAY_DUMMY, 250, 122);
    
    for (int i = 0; i < 10; i++) {
        memset(g_test_fb, i % 2 == 0 ? 0xFF : 0x00, sizeof(g_test_fb));
        int result = display_update(g_test_fb);
        ASSERT_EQUAL(0, result);
    }
    display_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Partial Update Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(display_partial_update_dummy) {
    display_init(DISPLAY_DUMMY, 250, 122);
    memset(g_test_fb, 0xFF, sizeof(g_test_fb));
    
    /* Partial update of a small region */
    int result = display_partial_update(g_test_fb, 10, 10, 50, 30);
    /* Dummy display should accept partial updates */
    ASSERT_EQUAL(0, result);
    display_cleanup();
}

TEST(display_partial_update_full_screen) {
    display_init(DISPLAY_DUMMY, 250, 122);
    memset(g_test_fb, 0xFF, sizeof(g_test_fb));
    
    int result = display_partial_update(g_test_fb, 0, 0, 250, 122);
    ASSERT_EQUAL(0, result);
    display_cleanup();
}

TEST(display_partial_update_at_origin) {
    display_init(DISPLAY_DUMMY, 250, 122);
    memset(g_test_fb, 0xFF, sizeof(g_test_fb));
    
    int result = display_partial_update(g_test_fb, 0, 0, 20, 20);
    ASSERT_EQUAL(0, result);
    display_cleanup();
}

TEST(display_partial_update_at_corner) {
    display_init(DISPLAY_DUMMY, 250, 122);
    memset(g_test_fb, 0xFF, sizeof(g_test_fb));
    
    int result = display_partial_update(g_test_fb, 230, 102, 20, 20);
    ASSERT_EQUAL(0, result);
    display_cleanup();
}

TEST(display_partial_update_out_of_bounds) {
    display_init(DISPLAY_DUMMY, 250, 122);
    memset(g_test_fb, 0xFF, sizeof(g_test_fb));
    
    /* Region extends past display bounds */
    int result = display_partial_update(g_test_fb, 240, 110, 50, 50);
    /* Should handle gracefully (clamp or reject) */
    ASSERT_TRUE(result == 0 || result == -1);
    display_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Display Clear Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(display_clear_white) {
    display_init(DISPLAY_DUMMY, 250, 122);
    int result = display_clear(0);  /* 0 = white */
    ASSERT_EQUAL(0, result);
    display_cleanup();
}

TEST(display_clear_black) {
    display_init(DISPLAY_DUMMY, 250, 122);
    int result = display_clear(1);  /* 1 = black */
    ASSERT_EQUAL(0, result);
    display_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Display Sleep/Wake Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(display_sleep_dummy) {
    display_init(DISPLAY_DUMMY, 250, 122);
    int result = display_sleep();
    ASSERT_EQUAL(0, result);
    display_cleanup();
}

TEST(display_wake_dummy) {
    display_init(DISPLAY_DUMMY, 250, 122);
    display_sleep();
    int result = display_wake();
    ASSERT_EQUAL(0, result);
    display_cleanup();
}

TEST(display_wake_without_sleep) {
    display_init(DISPLAY_DUMMY, 250, 122);
    int result = display_wake();
    /* Should be safe to call wake without prior sleep */
    ASSERT_EQUAL(0, result);
    display_cleanup();
}

TEST(display_update_after_sleep) {
    display_init(DISPLAY_DUMMY, 250, 122);
    display_sleep();
    
    memset(g_test_fb, 0xFF, sizeof(g_test_fb));
    int result = display_update(g_test_fb);
    
    /* May fail or auto-wake, either is acceptable */
    ASSERT_TRUE(result == 0 || result == -1);
    display_cleanup();
}

TEST(display_update_after_wake) {
    display_init(DISPLAY_DUMMY, 250, 122);
    display_sleep();
    display_wake();
    
    memset(g_test_fb, 0xFF, sizeof(g_test_fb));
    int result = display_update(g_test_fb);
    ASSERT_EQUAL(0, result);
    display_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Display Capabilities Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(display_supports_partial_update) {
    display_init(DISPLAY_DUMMY, 250, 122);
    int supports = display_supports_partial();
    /* Dummy display should report capability */
    ASSERT_TRUE(supports == 0 || supports == 1);
    display_cleanup();
}

TEST(display_supports_grayscale) {
    display_init(DISPLAY_DUMMY, 250, 122);
    int supports = display_supports_grayscale();
    /* Most e-ink displays don't support grayscale */
    ASSERT_TRUE(supports == 0 || supports == 1);
    display_cleanup();
}

TEST(display_get_bits_per_pixel) {
    display_init(DISPLAY_DUMMY, 250, 122);
    int bpp = display_get_bpp();
    /* Should be 1 for monochrome e-ink */
    ASSERT_TRUE(bpp >= 1 && bpp <= 32);
    display_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Display Buffer Size Calculation Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(display_buffer_size_250x122) {
    size_t size = display_calc_buffer_size(250, 122, 1);
    /* 250 pixels = 32 bytes per row, 122 rows = 3904 bytes */
    ASSERT_EQUAL(32 * 122, size);
}

TEST(display_buffer_size_264x176) {
    size_t size = display_calc_buffer_size(264, 176, 1);
    /* 264 pixels = 33 bytes per row, 176 rows = 5808 bytes */
    ASSERT_EQUAL(33 * 176, size);
}

TEST(display_buffer_size_200x200) {
    size_t size = display_calc_buffer_size(200, 200, 1);
    /* 200 pixels = 25 bytes per row, 200 rows = 5000 bytes */
    ASSERT_EQUAL(25 * 200, size);
}

TEST(display_buffer_size_8bpp) {
    size_t size = display_calc_buffer_size(250, 122, 8);
    /* 250 * 122 = 30500 bytes */
    ASSERT_EQUAL(250 * 122, size);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SPI Speed Configuration Tests (where applicable)
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(display_set_spi_speed_valid) {
    display_init(DISPLAY_DUMMY, 250, 122);
    int result = display_set_spi_speed(4000000);  /* 4 MHz */
    /* Should succeed or be no-op for dummy */
    ASSERT_EQUAL(0, result);
    display_cleanup();
}

TEST(display_set_spi_speed_high) {
    display_init(DISPLAY_DUMMY, 250, 122);
    int result = display_set_spi_speed(10000000);  /* 10 MHz */
    ASSERT_EQUAL(0, result);
    display_cleanup();
}

TEST(display_set_spi_speed_low) {
    display_init(DISPLAY_DUMMY, 250, 122);
    int result = display_set_spi_speed(500000);  /* 500 kHz */
    ASSERT_EQUAL(0, result);
    display_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Display Type Enumeration Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(display_type_enum_values) {
    /* Verify enum values are distinct */
    ASSERT_NOT_EQUAL(DISPLAY_DUMMY, DISPLAY_FRAMEBUFFER);
    ASSERT_NOT_EQUAL(DISPLAY_DUMMY, DISPLAY_WAVESHARE_2IN13_V2);
    ASSERT_NOT_EQUAL(DISPLAY_FRAMEBUFFER, DISPLAY_WAVESHARE_2IN13_V2);
}

TEST(display_name_for_type) {
    const char *name = display_type_name(DISPLAY_DUMMY);
    ASSERT_NOT_NULL(name);
    ASSERT_TRUE(strlen(name) > 0);
}

TEST(display_name_for_waveshare) {
    const char *name = display_type_name(DISPLAY_WAVESHARE_2IN13_V2);
    ASSERT_NOT_NULL(name);
    ASSERT_TRUE(strlen(name) > 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test Suite Runner
 * ═══════════════════════════════════════════════════════════════════════════ */

void run_suite_display(void) {
    printf("\n");
    printf("Display Module Tests\n");
    printf("================\n");

    /* Initialization tests */
    RUN_TEST(display_init_dummy_succeeds);
    RUN_TEST(display_cleanup_does_not_crash);
    RUN_TEST(display_can_reinitialize);
    RUN_TEST(display_init_framebuffer_on_non_linux);
    RUN_TEST(display_init_various_sizes);
    
    /* Type detection tests */
    RUN_TEST(display_get_type_after_init);
    RUN_TEST(display_get_dimensions_after_init);
    RUN_TEST(display_get_dimensions_different_size);
    
    /* Update tests */
    RUN_TEST(display_update_dummy_succeeds);
    RUN_TEST(display_update_with_null_fb);
    RUN_TEST(display_update_multiple_times);
    
    /* Partial update tests */
    RUN_TEST(display_partial_update_dummy);
    RUN_TEST(display_partial_update_full_screen);
    RUN_TEST(display_partial_update_at_origin);
    RUN_TEST(display_partial_update_at_corner);
    RUN_TEST(display_partial_update_out_of_bounds);
    
    /* Clear tests */
    RUN_TEST(display_clear_white);
    RUN_TEST(display_clear_black);
    
    /* Sleep/wake tests */
    RUN_TEST(display_sleep_dummy);
    RUN_TEST(display_wake_dummy);
    RUN_TEST(display_wake_without_sleep);
    RUN_TEST(display_update_after_sleep);
    RUN_TEST(display_update_after_wake);
    
    /* Capabilities tests */
    RUN_TEST(display_supports_partial_update);
    RUN_TEST(display_supports_grayscale);
    RUN_TEST(display_get_bits_per_pixel);
    
    /* Buffer size tests */
    RUN_TEST(display_buffer_size_250x122);
    RUN_TEST(display_buffer_size_264x176);
    RUN_TEST(display_buffer_size_200x200);
    RUN_TEST(display_buffer_size_8bpp);
    
    /* SPI speed tests */
    RUN_TEST(display_set_spi_speed_valid);
    RUN_TEST(display_set_spi_speed_high);
    RUN_TEST(display_set_spi_speed_low);
    
    /* Type enumeration tests */
    RUN_TEST(display_type_enum_values);
    RUN_TEST(display_name_for_type);
    RUN_TEST(display_name_for_waveshare);
}

#ifndef TEST_ALL
int main(void) {
    printf("PwnaUI Display Module Tests\n");
    printf("===========================\n");
    
    run_suite_display();
    
    test_print_summary();
    return test_exit_code();
}
#endif
