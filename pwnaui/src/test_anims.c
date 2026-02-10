/*
 * Animation Test - UPLOAD, DOWNLOAD, SLEEP only
 * Default theme, on the actual e-ink display.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include "display.h"
#include "themes.h"
#include "font.h"

static volatile int g_running = 1;
static void sig_handler(int s) { (void)s; g_running = 0; }

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void render_face(uint8_t *fb, int w, int h, face_state_t face) {
    memset(fb, 0xFF, (w * h) / 8);
    theme_render_face(fb, w, h, 2, 30, face, 0);
    display_partial_update(fb, 0, 0, w, h);
}

static void run_anim(uint8_t *fb, int w, int h, animation_type_t type, int interval_ms, int duration_ms, const char *name) {
    printf("  %s (%dms/frame, %ds)\n", name, interval_ms, duration_ms / 1000);
    animation_start(type, interval_ms);
    uint32_t start = now_ms();
    while (g_running && (now_ms() - start) < (uint32_t)duration_ms) {
        animation_tick(now_ms());
        face_state_t face = animation_get_frame();
        render_face(fb, w, h, face);
        usleep(150000);
    }
    animation_stop();
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int w = 250, h = 122;

    if (display_init(DISPLAY_WAVESHARE_2IN13_V4, w, h) != 0) {
        fprintf(stderr, "Display init failed\n");
        return 1;
    }
    if (themes_init(NULL) != 0) {
        fprintf(stderr, "Theme init failed\n");
        return 1;
    }
    if (theme_set_active("default") != 0) {
        fprintf(stderr, "default theme not found!\n");
        return 1;
    }

    uint8_t *fb = calloc(1, (w * h) / 8);
    if (!fb) return 1;

    printf("Active theme: default\n");

    /* UPLOAD animation - 1000ms/frame, 12 seconds */
    if (g_running) {
        printf("\n=== UPLOAD ANIMATION ===\n");
        run_anim(fb, w, h, ANIM_UPLOAD, 1000, 12000, "UPLOAD");
    }

    /* DOWNLOAD animation - 500ms/frame, 8 seconds */
    if (g_running) {
        printf("\n=== DOWNLOAD ANIMATION ===\n");
        run_anim(fb, w, h, ANIM_DOWNLOAD, 500, 8000, "DOWNLOAD");
    }

    /* SLEEP animation - 2000ms/frame, 14 seconds */
    if (g_running) {
        printf("\n=== SLEEP ANIMATION ===\n");
        run_anim(fb, w, h, ANIM_SLEEP, 2000, 14000, "SLEEP");
    }

    free(fb);
    display_cleanup();
    themes_cleanup();
    printf("\nDone.\n");
    return 0;
}
