/*
 * PwnaUI - Renderer Implementation
 * Text, icons, and layout rendering engine
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include " renderer.h\
#include \font.h\
#include \icons.h\
#include \display.h\

/* Current display dimensions */
static int g_display_width = 250;
static int g_display_height = 122;

/* Layout presets matching Pwnagotchi display configurations */
typedef struct {
 const char *name;
 int width;
 int height;
 /* Widget positions */
 int face_x, face_y;
 int name_x, name_y;
 int channel_x, channel_y;
 int aps_x, aps_y;
 int uptime_x, uptime_y;
 int line1_x1, line1_y1, line1_x2, line1_y2;
 int line2_x1, line2_y1, line2_x2, line2_y2;
 int friend_x, friend_y;
 int shakes_x, shakes_y;
 int mode_x, mode_y;
 int status_x, status_y;
 int status_max; /* Max chars per status line */
 /* Plugin widget positions */
 int bluetooth_x, bluetooth_y;
 int memtemp_x, memtemp_y;
} layout_t;

/* Predefined layouts matching Pwnagotchi's hardware drivers */
static const layout_t g_layouts[] = {
 /* Waveshare 2.13" V2 - 250x122 */
