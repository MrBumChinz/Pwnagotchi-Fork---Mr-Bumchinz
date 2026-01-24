/*
 * PwnaUI Icons Module Tests
 * Tests for static icon bitmaps and icon retrieval
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "test_framework.h"
#include "../src/icons.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Icon Initialization Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(icons_init_returns_success) {
    int result = icons_init();
    ASSERT_EQUAL(0, result);
}

TEST(icons_cleanup_does_not_crash) {
    icons_init();
    icons_cleanup();
    ASSERT_TRUE(1);
}

TEST(icons_can_reinitialize) {
    int r1 = icons_init();
    icons_cleanup();
    int r2 = icons_init();
    ASSERT_EQUAL(0, r1);
    ASSERT_EQUAL(0, r2);
    icons_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Signal Strength Icon Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(icons_get_signal_0) {
    icons_init();
    const icon_t *icon = icons_get("signal_0");
    ASSERT_NOT_NULL(icon);
    ASSERT_STR_EQUAL("signal_0", icon->name);
    ASSERT_TRUE(icon->width > 0);
    ASSERT_TRUE(icon->height > 0);
    ASSERT_NOT_NULL(icon->bitmap);
    icons_cleanup();
}

TEST(icons_get_signal_1) {
    icons_init();
    const icon_t *icon = icons_get("signal_1");
    ASSERT_NOT_NULL(icon);
    ASSERT_STR_EQUAL("signal_1", icon->name);
    icons_cleanup();
}

TEST(icons_get_signal_2) {
    icons_init();
    const icon_t *icon = icons_get("signal_2");
    ASSERT_NOT_NULL(icon);
    ASSERT_STR_EQUAL("signal_2", icon->name);
    icons_cleanup();
}

TEST(icons_get_signal_3) {
    icons_init();
    const icon_t *icon = icons_get("signal_3");
    ASSERT_NOT_NULL(icon);
    ASSERT_STR_EQUAL("signal_3", icon->name);
    icons_cleanup();
}

TEST(icons_get_signal_4) {
    icons_init();
    const icon_t *icon = icons_get("signal_4");
    ASSERT_NOT_NULL(icon);
    ASSERT_STR_EQUAL("signal_4", icon->name);
    icons_cleanup();
}

TEST(icons_signal_dimensions_consistent) {
    icons_init();
    const icon_t *s0 = icons_get("signal_0");
    const icon_t *s1 = icons_get("signal_1");
    const icon_t *s2 = icons_get("signal_2");
    const icon_t *s3 = icons_get("signal_3");
    const icon_t *s4 = icons_get("signal_4");
    
    /* All signal icons should have same dimensions */
    ASSERT_EQUAL(s0->width, s1->width);
    ASSERT_EQUAL(s0->width, s2->width);
    ASSERT_EQUAL(s0->width, s3->width);
    ASSERT_EQUAL(s0->width, s4->width);
    
    ASSERT_EQUAL(s0->height, s1->height);
    ASSERT_EQUAL(s0->height, s2->height);
    ASSERT_EQUAL(s0->height, s3->height);
    ASSERT_EQUAL(s0->height, s4->height);
    icons_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * WiFi Icon Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(icons_get_wifi) {
    icons_init();
    const icon_t *icon = icons_get("wifi");
    ASSERT_NOT_NULL(icon);
    ASSERT_STR_EQUAL("wifi", icon->name);
    ASSERT_TRUE(icon->width > 0);
    ASSERT_TRUE(icon->height > 0);
    ASSERT_NOT_NULL(icon->bitmap);
    icons_cleanup();
}

TEST(icons_wifi_has_content) {
    icons_init();
    const icon_t *icon = icons_get("wifi");
    ASSERT_NOT_NULL(icon);
    
    /* WiFi icon should have some set bits */
    size_t bytes = (icon->width + 7) / 8 * icon->height;
    int has_bits = 0;
    for (size_t i = 0; i < bytes && !has_bits; i++) {
        if (icon->bitmap[i] != 0) has_bits = 1;
    }
    ASSERT_TRUE(has_bits);
    icons_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Battery Icon Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(icons_get_battery_empty) {
    icons_init();
    const icon_t *icon = icons_get("battery_empty");
    ASSERT_NOT_NULL(icon);
    ASSERT_STR_EQUAL("battery_empty", icon->name);
    icons_cleanup();
}

TEST(icons_get_battery_low) {
    icons_init();
    const icon_t *icon = icons_get("battery_low");
    ASSERT_NOT_NULL(icon);
    ASSERT_STR_EQUAL("battery_low", icon->name);
    icons_cleanup();
}

TEST(icons_get_battery_med) {
    icons_init();
    const icon_t *icon = icons_get("battery_med");
    ASSERT_NOT_NULL(icon);
    ASSERT_STR_EQUAL("battery_med", icon->name);
    icons_cleanup();
}

TEST(icons_get_battery_high) {
    icons_init();
    const icon_t *icon = icons_get("battery_high");
    ASSERT_NOT_NULL(icon);
    ASSERT_STR_EQUAL("battery_high", icon->name);
    icons_cleanup();
}

TEST(icons_get_battery_full) {
    icons_init();
    const icon_t *icon = icons_get("battery_full");
    ASSERT_NOT_NULL(icon);
    ASSERT_STR_EQUAL("battery_full", icon->name);
    icons_cleanup();
}

TEST(icons_get_battery_charging) {
    icons_init();
    const icon_t *icon = icons_get("battery_charging");
    ASSERT_NOT_NULL(icon);
    ASSERT_STR_EQUAL("battery_charging", icon->name);
    icons_cleanup();
}

TEST(icons_battery_dimensions_consistent) {
    icons_init();
    const icon_t *empty = icons_get("battery_empty");
    const icon_t *low = icons_get("battery_low");
    const icon_t *med = icons_get("battery_med");
    const icon_t *high = icons_get("battery_high");
    const icon_t *full = icons_get("battery_full");
    const icon_t *charging = icons_get("battery_charging");
    
    /* All battery icons should have same dimensions */
    ASSERT_EQUAL(empty->width, low->width);
    ASSERT_EQUAL(empty->width, med->width);
    ASSERT_EQUAL(empty->width, high->width);
    ASSERT_EQUAL(empty->width, full->width);
    ASSERT_EQUAL(empty->width, charging->width);
    
    ASSERT_EQUAL(empty->height, low->height);
    ASSERT_EQUAL(empty->height, med->height);
    ASSERT_EQUAL(empty->height, high->height);
    ASSERT_EQUAL(empty->height, full->height);
    ASSERT_EQUAL(empty->height, charging->height);
    icons_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Lock Icon Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(icons_get_locked) {
    icons_init();
    const icon_t *icon = icons_get("locked");
    ASSERT_NOT_NULL(icon);
    ASSERT_STR_EQUAL("locked", icon->name);
    icons_cleanup();
}

TEST(icons_get_unlocked) {
    icons_init();
    const icon_t *icon = icons_get("unlocked");
    ASSERT_NOT_NULL(icon);
    ASSERT_STR_EQUAL("unlocked", icon->name);
    icons_cleanup();
}

TEST(icons_lock_dimensions_consistent) {
    icons_init();
    const icon_t *locked = icons_get("locked");
    const icon_t *unlocked = icons_get("unlocked");
    
    ASSERT_EQUAL(locked->width, unlocked->width);
    ASSERT_EQUAL(locked->height, unlocked->height);
    icons_cleanup();
}

TEST(icons_locked_differs_from_unlocked) {
    icons_init();
    const icon_t *locked = icons_get("locked");
    const icon_t *unlocked = icons_get("unlocked");
    
    size_t bytes = (locked->width + 7) / 8 * locked->height;
    
    /* The bitmaps should be different */
    int differs = memcmp(locked->bitmap, unlocked->bitmap, bytes) != 0;
    ASSERT_TRUE(differs);
    icons_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Bluetooth Icon Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(icons_get_bluetooth) {
    icons_init();
    const icon_t *icon = icons_get("bluetooth");
    ASSERT_NOT_NULL(icon);
    ASSERT_STR_EQUAL("bluetooth", icon->name);
    ASSERT_TRUE(icon->width > 0);
    ASSERT_TRUE(icon->height > 0);
    icons_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Plugin Icon Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(icons_get_plugin) {
    icons_init();
    const icon_t *icon = icons_get("plugin");
    ASSERT_NOT_NULL(icon);
    ASSERT_STR_EQUAL("plugin", icon->name);
    icons_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * AI Icon Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(icons_get_ai) {
    icons_init();
    const icon_t *icon = icons_get("ai");
    ASSERT_NOT_NULL(icon);
    ASSERT_STR_EQUAL("ai", icon->name);
    icons_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Error Handling Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(icons_get_null_returns_null) {
    icons_init();
    const icon_t *icon = icons_get(NULL);
    ASSERT_NULL(icon);
    icons_cleanup();
}

TEST(icons_get_empty_string_returns_null) {
    icons_init();
    const icon_t *icon = icons_get("");
    ASSERT_NULL(icon);
    icons_cleanup();
}

TEST(icons_get_unknown_returns_null) {
    icons_init();
    const icon_t *icon = icons_get("nonexistent_icon");
    ASSERT_NULL(icon);
    icons_cleanup();
}

TEST(icons_get_similar_name_not_found) {
    icons_init();
    /* "wifi " with space should not match "wifi" */
    const icon_t *icon = icons_get("wifi ");
    ASSERT_NULL(icon);
    icons_cleanup();
}

TEST(icons_get_case_sensitive) {
    icons_init();
    /* "WIFI" should not match "wifi" */
    const icon_t *icon = icons_get("WIFI");
    ASSERT_NULL(icon);
    icons_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Icon Bitmap Validity Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(icons_all_have_valid_bitmaps) {
    icons_init();
    const char *icon_names[] = {
        "signal_0", "signal_1", "signal_2", "signal_3", "signal_4",
        "wifi",
        "battery_empty", "battery_low", "battery_med", "battery_high",
        "battery_full", "battery_charging",
        "locked", "unlocked",
        "bluetooth", "plugin", "ai"
    };
    
    for (size_t i = 0; i < sizeof(icon_names)/sizeof(icon_names[0]); i++) {
        const icon_t *icon = icons_get(icon_names[i]);
        ASSERT_NOT_NULL(icon);
        ASSERT_NOT_NULL(icon->bitmap);
        ASSERT_TRUE(icon->width > 0);
        ASSERT_TRUE(icon->height > 0);
    }
    icons_cleanup();
}

TEST(icons_reasonable_dimensions) {
    icons_init();
    const char *icon_names[] = {
        "signal_0", "wifi", "battery_empty", "locked", "bluetooth", "plugin", "ai"
    };
    
    for (size_t i = 0; i < sizeof(icon_names)/sizeof(icon_names[0]); i++) {
        const icon_t *icon = icons_get(icon_names[i]);
        ASSERT_NOT_NULL(icon);
        /* Icons should be reasonably sized (4-32 pixels) */
        ASSERT_RANGE(icon->width, 4, 32);
        ASSERT_RANGE(icon->height, 4, 32);
    }
    icons_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Icon Count Test
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(icons_count_is_17) {
    icons_init();
    int count = icons_count();
    /* We defined 17 icons in icons.c */
    ASSERT_EQUAL(17, count);
    icons_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Icon Iteration Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(icons_get_by_index) {
    icons_init();
    for (int i = 0; i < icons_count(); i++) {
        const icon_t *icon = icons_get_by_index(i);
        ASSERT_NOT_NULL(icon);
        ASSERT_NOT_NULL(icon->name);
        ASSERT_NOT_NULL(icon->bitmap);
    }
    icons_cleanup();
}

TEST(icons_get_by_index_out_of_bounds) {
    icons_init();
    const icon_t *icon = icons_get_by_index(100);
    ASSERT_NULL(icon);
    
    icon = icons_get_by_index(-1);
    ASSERT_NULL(icon);
    icons_cleanup();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test Suite Runner
 * ═══════════════════════════════════════════════════════════════════════════ */

void run_suite_icons(void) {
    printf("\n");
    printf("Icons Module Tests\n");
    printf("================\n");

    /* Initialization tests */
    RUN_TEST(icons_init_returns_success);
    RUN_TEST(icons_cleanup_does_not_crash);
    RUN_TEST(icons_can_reinitialize);
    
    /* Signal icon tests */
    RUN_TEST(icons_get_signal_0);
    RUN_TEST(icons_get_signal_1);
    RUN_TEST(icons_get_signal_2);
    RUN_TEST(icons_get_signal_3);
    RUN_TEST(icons_get_signal_4);
    RUN_TEST(icons_signal_dimensions_consistent);
    
    /* WiFi icon tests */
    RUN_TEST(icons_get_wifi);
    RUN_TEST(icons_wifi_has_content);
    
    /* Battery icon tests */
    RUN_TEST(icons_get_battery_empty);
    RUN_TEST(icons_get_battery_low);
    RUN_TEST(icons_get_battery_med);
    RUN_TEST(icons_get_battery_high);
    RUN_TEST(icons_get_battery_full);
    RUN_TEST(icons_get_battery_charging);
    RUN_TEST(icons_battery_dimensions_consistent);
    
    /* Lock icon tests */
    RUN_TEST(icons_get_locked);
    RUN_TEST(icons_get_unlocked);
    RUN_TEST(icons_lock_dimensions_consistent);
    RUN_TEST(icons_locked_differs_from_unlocked);
    
    /* Other icon tests */
    RUN_TEST(icons_get_bluetooth);
    RUN_TEST(icons_get_plugin);
    RUN_TEST(icons_get_ai);
    
    /* Error handling tests */
    RUN_TEST(icons_get_null_returns_null);
    RUN_TEST(icons_get_empty_string_returns_null);
    RUN_TEST(icons_get_unknown_returns_null);
    RUN_TEST(icons_get_similar_name_not_found);
    RUN_TEST(icons_get_case_sensitive);
    
    /* Bitmap validity tests */
    RUN_TEST(icons_all_have_valid_bitmaps);
    RUN_TEST(icons_reasonable_dimensions);
    
    /* Count and iteration tests */
    RUN_TEST(icons_count_is_17);
    RUN_TEST(icons_get_by_index);
    RUN_TEST(icons_get_by_index_out_of_bounds);
}

#ifndef TEST_ALL
int main(void) {
    printf("PwnaUI Icons Module Tests\n");
    printf("=========================\n");
    
    run_suite_icons();
    
    test_print_summary();
    return test_exit_code();
}
#endif
