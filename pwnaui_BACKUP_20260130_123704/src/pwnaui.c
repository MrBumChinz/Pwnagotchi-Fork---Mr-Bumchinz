/*
 * PwnaUI - High-Performance C UI Renderer for Pwnagotchi
 * 
 * Main daemon that handles all UI rendering via UNIX socket IPC.
 * Replaces Python/PIL UI with native C for 10-30× performance improvement.
 * 
 * Author: PwnaUI Project
 * License: MIT
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <stdarg.h>
#include <pthread.h>

#include "ipc.h"
#include "renderer.h"
#include "display.h"
#include "font.h"
#include "icons.h"
#include "plugins.h"
#include "themes.h"
#include "bcap_ws.h"

/* Configuration */
#define SOCKET_PATH         "/var/run/pwnaui.sock"
#define PID_FILE            "/var/run/pwnaui.pid"
#define MAX_CLIENTS         64      /* Handle burst connections - must be >= SOCKET_BACKLOG in ipc.c */
#define BUFFER_SIZE         1024
#define UPDATE_INTERVAL_MS  300     /* ~3 Hz partial refresh (no blink) */

/* Global state */
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reload_config = 0;
static int g_daemon_mode = 0;
static int g_verbose = 0;
static int g_native_plugins = 0;    /* Enable native C plugins */

/* Static allocation, no malloc in hot paths */
static ui_state_t g_ui_state;
static uint8_t g_framebuffer[DISPLAY_MAX_WIDTH * DISPLAY_MAX_HEIGHT / 8];
static volatile int g_dirty = 0;
static uint64_t g_last_update_ms = 0;

/* Display thread state */
static pthread_t g_display_thread;
static pthread_mutex_t g_ui_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_display_cond = PTHREAD_COND_INITIALIZER;
static volatile int g_display_pending = 0;  /* Signal display thread to render */
static uint8_t g_display_fb[DISPLAY_MAX_WIDTH * DISPLAY_MAX_HEIGHT / 8];  /* Copy for display thread */

/* Native C plugins state */
static plugin_state_t g_plugins;
static uint64_t g_last_full_refresh_ms = 0;

/* GPS CNCplugin enabled flag (separate from native_plugins for future flexibility) */
static int g_gps_enabled = 0;

/* Bettercap WebSocket client state */
static int g_bcap_enabled = 0;
static bcap_ws_ctx_t *g_bcap_ctx = NULL;
static int g_bcap_ap_count = 0;
static int g_bcap_handshake_count = 0;

/* Forward declarations */
static void trigger_display_update(void);
static void *display_thread_func(void *arg);

/*
 * Logging
 */
static void pwnaui_log(int priority, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    
    if (g_daemon_mode) {
        vsyslog(priority, fmt, ap);
    } else {
        FILE *out = (priority <= LOG_WARNING) ? stderr : stdout;
        vfprintf(out, fmt, ap);
        fprintf(out, "\n");
    }
    
    va_end(ap);
}

#define PWNAUI_LOG_INFO(...)    pwnaui_log(LOG_INFO, __VA_ARGS__)
#define PWNAUI_LOG_WARN(...)    pwnaui_log(LOG_WARNING, __VA_ARGS__)
#define PWNAUI_LOG_ERR(...)     pwnaui_log(LOG_ERR, __VA_ARGS__)
#define PWNAUI_LOG_DEBUG(...)   do { if (g_verbose) pwnaui_log(LOG_DEBUG, __VA_ARGS__); } while(0)

/* ==========================================================================
 * Bettercap WebSocket Event Callbacks
 * ========================================================================== */

static void bcap_on_event(const bcap_event_t *event, void *user_data) {
    (void)user_data;
    char mac_str[18];
    
    switch (event->type) {
        case BCAP_EVT_AP_NEW:
            g_bcap_ap_count++;
            bcap_format_mac(&event->data.ap.bssid, mac_str);
            PWNAUI_LOG_DEBUG("[bcap] AP NEW: %s (%s) ch=%d", 
                           mac_str, event->data.ap.ssid, event->data.ap.channel);
            /* Update UI state */
            pthread_mutex_lock(&g_ui_mutex);
            snprintf(g_ui_state.aps, sizeof(g_ui_state.aps), "%d", g_bcap_ap_count);
            g_dirty = 1;
            pthread_mutex_unlock(&g_ui_mutex);
            break;
            
        case BCAP_EVT_AP_LOST:
            if (g_bcap_ap_count > 0) g_bcap_ap_count--;
            pthread_mutex_lock(&g_ui_mutex);
            snprintf(g_ui_state.aps, sizeof(g_ui_state.aps), "%d", g_bcap_ap_count);
            g_dirty = 1;
            pthread_mutex_unlock(&g_ui_mutex);
            break;
            
        case BCAP_EVT_HANDSHAKE:
            g_bcap_handshake_count++;
            bcap_format_mac(&event->data.hs.ap_bssid, mac_str);
            PWNAUI_LOG_INFO("[bcap] *** HANDSHAKE *** AP=%s SSID=%s", mac_str, event->data.hs.ssid);
            /* Update UI state - show handshake notification */
            pthread_mutex_lock(&g_ui_mutex);
            g_ui_state.pwds = g_bcap_handshake_count;
            snprintf(g_ui_state.shakes, sizeof(g_ui_state.shakes), "%d", g_bcap_handshake_count);
            g_dirty = 1;
            pthread_mutex_unlock(&g_ui_mutex);
            break;
            
        case BCAP_EVT_CLIENT_NEW:
        case BCAP_EVT_CLIENT_LOST:
        case BCAP_EVT_CLIENT_PROBE:
            /* Track but don't spam logs */
            break;
            
        default:
            break;
    }
}

static void bcap_on_state_change(bool connected, void *user_data) {
    (void)user_data;
    PWNAUI_LOG_INFO("[bcap] Connection: %s", connected ? "CONNECTED" : "DISCONNECTED");
    
    if (!connected) {
        /* Reset counts on disconnect - will be repopulated on reconnect */
        g_bcap_ap_count = 0;
    }
}

/*
 * Signal handlers
 */
static void signal_handler(int sig) {
    switch (sig) {
        case SIGTERM:
        case SIGINT:
            g_running = 0;
            break;
        case SIGHUP:
            g_reload_config = 1;
            break;
    }
}

static void setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    
    /* Ignore SIGPIPE - handle write errors explicitly */
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
}

/*
 * Get current time in milliseconds
 */
static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * Initialize UI state with defaults
 */
static void init_ui_state(void) {
    memset(&g_ui_state, 0, sizeof(g_ui_state));
    memset(g_framebuffer, 0xFF, sizeof(g_framebuffer));  /* White background */
    
    /* Default values matching Pwnagotchi layout */
    strncpy(g_ui_state.name, "pwnagotchi>", sizeof(g_ui_state.name) - 1);
    strncpy(g_ui_state.face, "(◕‿‿◕)", sizeof(g_ui_state.face) - 1);  /* AWAKE */
    strncpy(g_ui_state.channel, "00", sizeof(g_ui_state.channel) - 1);
    strncpy(g_ui_state.aps, "0", sizeof(g_ui_state.aps) - 1);
    strncpy(g_ui_state.uptime, "00:00:00:00", sizeof(g_ui_state.uptime) - 1);
    strncpy(g_ui_state.shakes, "0", sizeof(g_ui_state.shakes) - 1);
    strncpy(g_ui_state.mode, "AUTO", sizeof(g_ui_state.mode) - 1);
    strncpy(g_ui_state.status, "Initializing...", sizeof(g_ui_state.status) - 1);
    strncpy(g_ui_state.bluetooth, "BT-", sizeof(g_ui_state.bluetooth) - 1);
    strncpy(g_ui_state.gps, "GPS-", sizeof(g_ui_state.gps) - 1);
    
    g_ui_state.invert = 0;
    g_dirty = 1;
}

/*
 * Command handlers - Parse and execute IPC commands
 */
static int handle_command(const char *cmd, char *response, size_t resp_size) {
    char cmd_name[32];
    int n;
    
    PWNAUI_LOG_DEBUG("Received command: %s", cmd);
    
    /* Parse command name */
    n = sscanf(cmd, "%31s", cmd_name);
    if (n != 1) {
        snprintf(response, resp_size, "ERR Invalid command\n");
        return -1;
    }
    
    /* CLEAR - Clear display buffer */
    if (strcmp(cmd_name, "CLEAR") == 0) {
        renderer_clear(&g_ui_state, g_framebuffer);
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* UPDATE - Flush buffer to display using partial refresh (no blink) */
    if (strcmp(cmd_name, "UPDATE") == 0) {
        if (g_dirty) {
            uint64_t now = get_time_ms();
            /* Rate limit updates */
            if (now - g_last_update_ms >= UPDATE_INTERVAL_MS) {
                renderer_render_ui(&g_ui_state, g_framebuffer);
                trigger_display_update();  /* Non-blocking - signals display thread */
                g_last_update_ms = now;
                g_dirty = 0;
            }
        }
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* FULL_UPDATE - Force full e-ink refresh */
    if (strcmp(cmd_name, "FULL_UPDATE") == 0) {
        renderer_render_ui(&g_ui_state, g_framebuffer);
        display_update(g_framebuffer);  /* Full refresh */
        g_last_update_ms = get_time_ms();
        g_dirty = 0;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_FACE face_string */
    if (strcmp(cmd_name, "SET_FACE") == 0) {
        const char *face = cmd + 9;  /* Skip "SET_FACE " */
        while (*face == ' ') face++;
        strncpy(g_ui_state.face, face, sizeof(g_ui_state.face) - 1);
        g_ui_state.face[sizeof(g_ui_state.face) - 1] = '\0';
        /* Remove trailing newline */
        char *nl = strchr(g_ui_state.face, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_STATUS text */
    if (strcmp(cmd_name, "SET_STATUS") == 0) {
        const char *status = cmd + 11;  /* Skip "SET_STATUS " */
        while (*status == ' ') status++;
        strncpy(g_ui_state.status, status, sizeof(g_ui_state.status) - 1);
        g_ui_state.status[sizeof(g_ui_state.status) - 1] = '\0';
        /* Remove trailing newline */
        char *nl = strchr(g_ui_state.status, '\n');
        if (nl) *nl = '\0';
        /* Replace literal \n with space */
        char *p = g_ui_state.status;
        while ((p = strstr(p, "\\n")) != NULL) {
            *p = ' ';
            memmove(p + 1, p + 2, strlen(p + 2) + 1);
        }
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_CHANNEL value */
    if (strcmp(cmd_name, "SET_CHANNEL") == 0) {
        const char *val = cmd + 12;
        while (*val == ' ') val++;
        strncpy(g_ui_state.channel, val, sizeof(g_ui_state.channel) - 1);
        char *nl = strchr(g_ui_state.channel, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_APS value */
    if (strcmp(cmd_name, "SET_APS") == 0) {
        const char *val = cmd + 8;
        while (*val == ' ') val++;
        strncpy(g_ui_state.aps, val, sizeof(g_ui_state.aps) - 1);
        char *nl = strchr(g_ui_state.aps, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_UPTIME value */
    if (strcmp(cmd_name, "SET_UPTIME") == 0) {
        const char *val = cmd + 11;
        while (*val == ' ') val++;
        strncpy(g_ui_state.uptime, val, sizeof(g_ui_state.uptime) - 1);
        char *nl = strchr(g_ui_state.uptime, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_SHAKES value (legacy - kept for compatibility) */
    if (strcmp(cmd_name, "SET_SHAKES") == 0) {
        const char *val = cmd + 11;
        while (*val == ' ') val++;
        strncpy(g_ui_state.shakes, val, sizeof(g_ui_state.shakes) - 1);
        char *nl = strchr(g_ui_state.shakes, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_STATS pwds fhs phs taps - Bottom bar stats in one command */
    if (strcmp(cmd_name, "SET_STATS") == 0) {
        int pwds = 0, fhs = 0, phs = 0, taps = 0;
        if (sscanf(cmd + 10, "%d %d %d %d", &pwds, &fhs, &phs, &taps) >= 1) {
            g_ui_state.pwds = pwds;
            g_ui_state.fhs = fhs;
            g_ui_state.phs = phs;
            g_ui_state.taps = taps;
            g_dirty = 1;
            snprintf(response, resp_size, "OK\n");
        } else {
            snprintf(response, resp_size, "ERR Invalid SET_STATS format\n");
        }
        return 0;
    }
    
    /* SET_MODE mode */
    if (strcmp(cmd_name, "SET_MODE") == 0) {
        const char *val = cmd + 9;
        while (*val == ' ') val++;
        strncpy(g_ui_state.mode, val, sizeof(g_ui_state.mode) - 1);
        char *nl = strchr(g_ui_state.mode, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_NAME name */
    if (strcmp(cmd_name, "SET_NAME") == 0) {
        const char *val = cmd + 9;
        while (*val == ' ') val++;
        strncpy(g_ui_state.name, val, sizeof(g_ui_state.name) - 1);
        char *nl = strchr(g_ui_state.name, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_FRIEND name */
    if (strcmp(cmd_name, "SET_FRIEND") == 0) {
        const char *val = cmd + 11;
        while (*val == ' ') val++;
        strncpy(g_ui_state.friend_name, val, sizeof(g_ui_state.friend_name) - 1);
        char *nl = strchr(g_ui_state.friend_name, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_BLUETOOTH status - BT-Tether plugin status ('C' = connected, '-' = disconnected) */
    if (strcmp(cmd_name, "SET_BLUETOOTH") == 0) {
        const char *val = cmd + 14;
        while (*val == ' ') val++;
        strncpy(g_ui_state.bluetooth, val, sizeof(g_ui_state.bluetooth) - 1);
        char *nl = strchr(g_ui_state.bluetooth, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_GPS CNCstatus - GPS CNCplugin status ('C' = connected, '-' = disconnected, 'S' = saved) */
    if (strcmp(cmd_name, "SET_GPS") == 0) {
        const char *val = cmd + 8;
        while (*val == ' ') val++;
        strncpy(g_ui_state.gps, val, sizeof(g_ui_state.gps) - 1);
        char *nl = strchr(g_ui_state.gps, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_BATTERY status - Battery percentage (e.g. "85%" or "85%+" for charging) */
    if (strcmp(cmd_name, "SET_BATTERY") == 0) {
        const char *val = cmd + 12;
        while (*val == ' ') val++;
        strncpy(g_ui_state.battery, val, sizeof(g_ui_state.battery) - 1);
        char *nl = strchr(g_ui_state.battery, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_PWNHUB_ENABLED 0|1 - Enable/disable PwnHub stats display */
    if (strcmp(cmd_name, "SET_PWNHUB_ENABLED") == 0) {
        int enabled;
        if (sscanf(cmd, "SET_PWNHUB_ENABLED %d", &enabled) == 1) {
            g_ui_state.pwnhub_enabled = enabled ? 1 : 0;
            g_dirty = 1;
            snprintf(response, resp_size, "OK\n");
            PWNAUI_LOG_DEBUG("PwnHub display %s", enabled ? "enabled" : "disabled");
            return 0;
        }
        snprintf(response, resp_size, "ERR Invalid SET_PWNHUB_ENABLED param\n");
        return -1;
    }
    
    /* SET_PWNHUB_MACROS protein fat carbs - Set macro values (0-50 each) */
    if (strcmp(cmd_name, "SET_PWNHUB_MACROS") == 0) {
        int protein, fat, carbs;
        if (sscanf(cmd, "SET_PWNHUB_MACROS %d %d %d", &protein, &fat, &carbs) == 3) {
            g_ui_state.pwnhub_protein = (protein < 0) ? 0 : (protein > 50) ? 50 : protein;
            g_ui_state.pwnhub_fat = (fat < 0) ? 0 : (fat > 50) ? 50 : fat;
            g_ui_state.pwnhub_carbs = (carbs < 0) ? 0 : (carbs > 50) ? 50 : carbs;
            g_dirty = 1;
            snprintf(response, resp_size, "OK\n");
            return 0;
        }
        snprintf(response, resp_size, "ERR Invalid SET_PWNHUB_MACROS params (need: protein fat carbs)\n");
        return -1;
    }
    
    /* SET_PWNHUB_XP percent - Set XP progress (0-100) */
    if (strcmp(cmd_name, "SET_PWNHUB_XP") == 0) {
        int percent;
        if (sscanf(cmd, "SET_PWNHUB_XP %d", &percent) == 1) {
            g_ui_state.pwnhub_xp_percent = (percent < 0) ? 0 : (percent > 100) ? 100 : percent;
            g_dirty = 1;
            snprintf(response, resp_size, "OK\n");
            return 0;
        }
        snprintf(response, resp_size, "ERR Invalid SET_PWNHUB_XP param\n");
        return -1;
    }
    
    /* SET_PWNHUB_STAGE title level wins total - Set stage info */
    if (strcmp(cmd_name, "SET_PWNHUB_STAGE") == 0) {
        char title[24];
        int level, wins, total;
        if (sscanf(cmd, "SET_PWNHUB_STAGE %23s %d %d %d", title, &level, &wins, &total) == 4) {
            strncpy(g_ui_state.pwnhub_title, title, sizeof(g_ui_state.pwnhub_title) - 1);
            g_ui_state.pwnhub_title[sizeof(g_ui_state.pwnhub_title) - 1] = '\0';
            g_ui_state.pwnhub_level = level;
            g_ui_state.pwnhub_wins = wins;
            g_ui_state.pwnhub_battles = total;
            g_dirty = 1;
            snprintf(response, resp_size, "OK\n");
            return 0;
        }
        snprintf(response, resp_size, "ERR Invalid SET_PWNHUB_STAGE params (need: title level wins total)\n");
        return -1;
    }
    
    /* SET_MEMTEMP_HEADER header - Memtemp column headers (e.g. "mem cpu tmp") */
    if (strcmp(cmd_name, "SET_MEMTEMP_HEADER") == 0) {
        const char *val = cmd + 18;
        while (*val == ' ') val++;
        strncpy(g_ui_state.memtemp_header, val, sizeof(g_ui_state.memtemp_header) - 1);
        char *nl = strchr(g_ui_state.memtemp_header, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_MEMTEMP_DATA data - Memtemp data values (e.g. " 42%  12%  48C") */
    if (strcmp(cmd_name, "SET_MEMTEMP_DATA") == 0) {
        const char *val = cmd + 16;
        while (*val == ' ') val++;
        strncpy(g_ui_state.memtemp_data, val, sizeof(g_ui_state.memtemp_data) - 1);
        char *nl = strchr(g_ui_state.memtemp_data, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* DRAW_TEXT x y font_id text */
    if (strcmp(cmd_name, "DRAW_TEXT") == 0) {
        int x, y, font_id;
        char text[256];
        if (sscanf(cmd, "DRAW_TEXT %d %d %d %255[^\n]", &x, &y, &font_id, text) == 4) {
            renderer_draw_text(&g_ui_state, g_framebuffer, x, y, text, font_id);
            g_dirty = 1;
            snprintf(response, resp_size, "OK\n");
            return 0;
        }
        snprintf(response, resp_size, "ERR Invalid DRAW_TEXT params\n");
        return -1;
    }
    
    /* DRAW_LINE x1 y1 x2 y2 */
    if (strcmp(cmd_name, "DRAW_LINE") == 0) {
        int x1, y1, x2, y2;
        if (sscanf(cmd, "DRAW_LINE %d %d %d %d", &x1, &y1, &x2, &y2) == 4) {
            renderer_draw_line(&g_ui_state, g_framebuffer, x1, y1, x2, y2);
            g_dirty = 1;
            snprintf(response, resp_size, "OK\n");
            return 0;
        }
        snprintf(response, resp_size, "ERR Invalid DRAW_LINE params\n");
        return -1;
    }
    
    /* DRAW_ICON name x y */
    if (strcmp(cmd_name, "DRAW_ICON") == 0) {
        char icon_name[32];
        int x, y;
        if (sscanf(cmd, "DRAW_ICON %31s %d %d", icon_name, &x, &y) == 3) {
            icons_draw(g_framebuffer, icon_name, x, y);
            g_dirty = 1;
            snprintf(response, resp_size, "OK\n");
            return 0;
        }
        snprintf(response, resp_size, "ERR Invalid DRAW_ICON params\n");
        return -1;
    }
    
    /* SET_INVERT 0|1 */
    if (strcmp(cmd_name, "SET_INVERT") == 0) {
        int invert;
        if (sscanf(cmd, "SET_INVERT %d", &invert) == 1) {
            g_ui_state.invert = invert ? 1 : 0;
            g_dirty = 1;
            snprintf(response, resp_size, "OK\n");
            return 0;
        }
        snprintf(response, resp_size, "ERR Invalid SET_INVERT param\n");
        return -1;
    }
    
    /* SET_LAYOUT layout_name */
    if (strcmp(cmd_name, "SET_LAYOUT") == 0) {
        const char *layout = cmd + 11;
        while (*layout == ' ') layout++;
        renderer_set_layout(layout);
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* GET_STATE - Return current UI state (for debugging) */
    if (strcmp(cmd_name, "GET_STATE") == 0) {
        snprintf(response, resp_size, 
            "OK face=%s status=%s ch=%s aps=%s up=%s shakes=%s mode=%s name=%s bt=%s memtemp=%s\n",
            g_ui_state.face, g_ui_state.status, g_ui_state.channel,
            g_ui_state.aps, g_ui_state.uptime, g_ui_state.shakes,
            g_ui_state.mode, g_ui_state.name, g_ui_state.bluetooth,
            g_ui_state.memtemp_data);
        return 0;
    }
    
    /* PING - Connection test */
    if (strcmp(cmd_name, "PING") == 0) {
        snprintf(response, resp_size, "PONG\n");
        return 0;
    }
    
    /* SET_THEME theme_name - Switch to a different face theme */
    if (strcmp(cmd_name, "SET_THEME") == 0) {
        const char *theme_name = cmd + 10;  /* Skip "SET_THEME " */
        while (*theme_name == ' ') theme_name++;
        char name_buf[64];
        strncpy(name_buf, theme_name, sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';
        /* Remove trailing newline */
        char *nl = strchr(name_buf, '\n');
        if (nl) *nl = '\0';
        
        /* Set the PNG theme */
        if (theme_set_active(name_buf) == 0) {
            themes_set_enabled(1);  /* Always enable PNG themes */
            g_dirty = 1;
            snprintf(response, resp_size, "OK Theme set to %s\n", name_buf);
            PWNAUI_LOG_INFO("Theme switched to: %s", name_buf);
        } else {
            snprintf(response, resp_size, "ERR Theme not found: %s\n", name_buf);
        }
        return 0;
    }
    
    /* LIST_THEMES - Get list of available PNG themes */
    if (strcmp(cmd_name, "LIST_THEMES") == 0) {
        int count = themes_count();
        char *p = response;
        int remaining = (int)resp_size;
        /* PNG themes only */
        int n = snprintf(p, remaining, "OK %d themes:", count);
        p += n; remaining -= n;
        
        if (count > 0) {
            const char **names = themes_list();
            for (int i = 0; i < count && remaining > 0; i++) {
                n = snprintf(p, remaining, " %s", names[i]);
                p += n; remaining -= n;
            }
        }
        if (remaining > 0) {
            snprintf(p, remaining, "\n");
        }
        return 0;
    }
    
    /* GET_THEME - Get current active PNG theme name */
    if (strcmp(cmd_name, "GET_THEME") == 0) {
        const char *current = theme_get_active();
        if (current && current[0]) {
            snprintf(response, resp_size, "OK %s\n", current);
        } else {
            snprintf(response, resp_size, "OK pwnachu\n");  /* Default PNG theme */
        }
        return 0;
    }
    
    /* Unknown command */
    snprintf(response, resp_size, "ERR Unknown command: %s\n", cmd_name);
    return -1;
}

/*
 * Create PID file
 */
static int create_pidfile(void) {
    FILE *f = fopen(PID_FILE, "w");
    if (!f) {
        PWNAUI_LOG_ERR("Failed to create PID file: %s", strerror(errno));
        return -1;
    }
    fprintf(f, "%d\n", getpid());
    fclose(f);
    return 0;
}

/*
 * Remove PID file
 */
static void remove_pidfile(void) {
    unlink(PID_FILE);
}

/*
 * Daemonize the process
 */
static int daemonize(void) {
    pid_t pid;
    
    /* Fork off parent */
    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);  /* Parent exits */
    }
    
    /* Create new session */
    if (setsid() < 0) {
        return -1;
    }
    
    /* Fork again to prevent acquiring a controlling terminal */
    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }
    
    /* Set file permissions */
    umask(0);
    
    /* Change working directory */
    chdir("/");
    
    /* Close standard file descriptors */
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    
    /* Redirect to /dev/null */
    open("/dev/null", O_RDONLY);  /* stdin */
    open("/dev/null", O_WRONLY);  /* stdout */
    open("/dev/null", O_WRONLY);  /* stderr */
    
    return 0;
}

/*
 * Print usage
 */
/*
 * Convert display type string to enum
 */
static display_type_t parse_display_type(const char *name) {
    if (strcmp(name, "dummy") == 0) return DISPLAY_DUMMY;
    if (strcmp(name, "framebuffer") == 0) return DISPLAY_FRAMEBUFFER;
    if (strcmp(name, "waveshare2in13_v2") == 0) return DISPLAY_WAVESHARE_2IN13_V2;
    if (strcmp(name, "waveshare2in13_v3") == 0) return DISPLAY_WAVESHARE_2IN13_V3;
    if (strcmp(name, "waveshare2in13_v4") == 0) return DISPLAY_WAVESHARE_2IN13_V4;
    if (strcmp(name, "waveshare2in7") == 0) return DISPLAY_WAVESHARE_2IN7;
    if (strcmp(name, "waveshare1in54") == 0) return DISPLAY_WAVESHARE_1IN54;
    if (strcmp(name, "inky_phat") == 0) return DISPLAY_INKY_PHAT;
    return DISPLAY_WAVESHARE_2IN13_V2;  /* Default */
}

/*
 * Get display dimensions for a type
 */
static void get_display_dimensions(display_type_t type, int *width, int *height) {
    switch (type) {
        case DISPLAY_WAVESHARE_2IN13_V2:
        case DISPLAY_WAVESHARE_2IN13_V3:
        case DISPLAY_WAVESHARE_2IN13_V4:
            *width = 250; *height = 122; break;
        case DISPLAY_WAVESHARE_2IN7:
            *width = 264; *height = 176; break;
        case DISPLAY_WAVESHARE_1IN54:
            *width = 200; *height = 200; break;
        case DISPLAY_INKY_PHAT:
            *width = 212; *height = 104; break;
        default:
            *width = 250; *height = 122; break;
    }
}

/*
 * Display Thread - handles all blocking display operations
 * 
 * This thread runs independently of the main IPC loop, ensuring that
 * slow e-ink display updates (which can take 200-500ms) don't block
 * socket accept() calls and cause connection pileup.
 */
static void *display_thread_func(void *arg) {
    (void)arg;  /* Unused */
    
    PWNAUI_LOG_INFO("Display thread started");
    
    while (g_running) {
        pthread_mutex_lock(&g_ui_mutex);
        
        /* Wait for render signal or timeout (for periodic checks) */
        while (!g_display_pending && g_running) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;  /* 1 second timeout for checking g_running */
            
            int rc = pthread_cond_timedwait(&g_display_cond, &g_ui_mutex, &ts);
            if (rc == ETIMEDOUT) {
                /* Just a timeout, check g_running and loop */
                continue;
            }
        }
        
        if (!g_running) {
            pthread_mutex_unlock(&g_ui_mutex);
            break;
        }
        
        /* Copy framebuffer while holding mutex */
        memcpy(g_display_fb, g_framebuffer, sizeof(g_display_fb));
        g_display_pending = 0;
        
        pthread_mutex_unlock(&g_ui_mutex);
        
        /* Now do the slow display update WITHOUT holding the mutex */
        /* This is where epd_wait_busy() blocks, but main thread is free */
        display_partial_update(g_display_fb, 0, 0, 0, 0);
        PWNAUI_LOG_DEBUG("Display updated");
    }
    
    PWNAUI_LOG_INFO("Display thread exiting");
    return NULL;
}

/*
 * Signal the display thread to render
 * Called from main thread after updating g_framebuffer
 */
static void trigger_display_update(void) {
    pthread_mutex_lock(&g_ui_mutex);
    g_display_pending = 1;
    pthread_cond_signal(&g_display_cond);
    pthread_mutex_unlock(&g_ui_mutex);
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [OPTIONS]\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -d, --daemon     Run as daemon\n");
    fprintf(stderr, "  -v, --verbose    Verbose logging\n");
    fprintf(stderr, "  -p, --plugins    Enable native C plugins (memtemp, battery, bluetooth)\n");
    fprintf(stderr, "  -b, --bcap       Enable bettercap WebSocket (real-time AP/handshake events)\n");
    fprintf(stderr, "  -s, --socket PATH  Socket path (default: %s)\n", SOCKET_PATH);
    fprintf(stderr, "  -D, --display TYPE Display type (waveshare2in13, fb, dummy)\n");
    fprintf(stderr, "  -h, --help       Show this help\n");
}

/*
 * Main entry point
 */
int main(int argc, char *argv[]) {
    int opt;
    const char *socket_path = SOCKET_PATH;
    const char *display_type = "waveshare2in13_v4";  /* User display: Waveshare 2.13" V4 */
    int server_fd = -1;
    int client_fds[MAX_CLIENTS];
    int num_clients = 0;
    
    /* Initialize client array */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_fds[i] = -1;
    }
    
    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--daemon") == 0) {
            g_daemon_mode = 1;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--plugins") == 0) {
            g_native_plugins = 1;
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--bcap") == 0) {
            g_bcap_enabled = 1;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--socket") == 0) {
            if (i + 1 < argc) {
                socket_path = argv[++i];
            }
        } else if (strcmp(argv[i], "-D") == 0 || strcmp(argv[i], "--display") == 0) {
            if (i + 1 < argc) {
                display_type = argv[++i];
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return EXIT_SUCCESS;
        }
    }
    
    /* Setup logging */
    if (g_daemon_mode) {
        openlog("pwnaui", LOG_PID | LOG_NDELAY, LOG_DAEMON);
    }
    
    PWNAUI_LOG_INFO("PwnaUI starting...");
    
    /* Daemonize if requested */
    if (g_daemon_mode) {
        if (daemonize() < 0) {
            PWNAUI_LOG_ERR("Failed to daemonize: %s", strerror(errno));
            return EXIT_FAILURE;
        }
    }
    
    /* Create PID file */
    if (create_pidfile() < 0) {
        return EXIT_FAILURE;
    }
    
    /* Setup signal handlers */
    setup_signals();
    
    /* Initialize display */
    display_type_t dtype = parse_display_type(display_type);
    int disp_width, disp_height;
    get_display_dimensions(dtype, &disp_width, &disp_height);
    PWNAUI_LOG_INFO("Initializing display: %s (%dx%d)", display_type, disp_width, disp_height);
    if (display_init(dtype, disp_width, disp_height) < 0) {
        PWNAUI_LOG_ERR("Failed to initialize display");
        remove_pidfile();
        return EXIT_FAILURE;
    }
    
    /* Initialize font system */
    if (font_init() < 0) {
        PWNAUI_LOG_ERR("Failed to initialize fonts");
        display_cleanup();
        remove_pidfile();
        return EXIT_FAILURE;
    }
    
    /* Initialize icons */
    if (icons_init() < 0) {
        PWNAUI_LOG_ERR("Failed to initialize icons");
        font_cleanup();
        display_cleanup();
        remove_pidfile();
        return EXIT_FAILURE;
    }
    
    /* Initialize renderer */
    if (renderer_init() < 0) {
        PWNAUI_LOG_ERR("Failed to initialize renderer");
        icons_cleanup();
        font_cleanup();
        display_cleanup();
        remove_pidfile();
        return EXIT_FAILURE;
    }
    
    /* Set renderer layout based on display type */
    renderer_set_layout(display_type);
    PWNAUI_LOG_INFO("Set layout: %s", display_type);
    
    /* Initialize native plugins if enabled */
    if (g_native_plugins) {
        PWNAUI_LOG_INFO("Initializing native C plugins (memtemp, battery, bluetooth)");
        if (plugins_init(&g_plugins) < 0) {
            PWNAUI_LOG_ERR("Failed to initialize native plugins");
            /* Non-fatal - continue without native plugins */
            g_native_plugins = 0;
        } else {
            PWNAUI_LOG_INFO("Native plugins initialized successfully");
        }
    }
    
    /* Initialize theme system */
    PWNAUI_LOG_INFO("Initializing theme system");
    if (themes_init(NULL) < 0) {
        PWNAUI_LOG_WARN("Theme system not available (non-fatal)");
    } else {
        PWNAUI_LOG_INFO("Theme system ready, %d themes available", themes_count());
        
        /* Auto-load theme from pwnagotchi config - default to 'default' theme */
        char loaded_theme[64] = {0};
        FILE *cfg = fopen("/etc/pwnagotchi/config.toml", "r");
        if (cfg) {
            char line[512];
            int in_ui_faces = 0;
            while (fgets(line, sizeof(line), cfg)) {
                /* Track when we're in [ui.faces] section */
                if (strstr(line, "[ui.faces]")) {
                    in_ui_faces = 1;
                    continue;
                }
                /* Exit section when we hit another section */
                if (in_ui_faces && line[0] == '[') {
                    in_ui_faces = 0;
                }
                
                /* Look for theme = "themename" when in [ui.faces] section */
                if ((in_ui_faces && strstr(line, "theme")) || strstr(line, "ui.faces.theme")) {
                    char *theme_key = strstr(line, "theme");
                    if (theme_key) {
                        char *p = theme_key + 5;
                        while (*p == ' ' || *p == '\t') p++;
                        if (*p != '=') continue;
                    }
                    
                    char *quote1 = strchr(line, '"');
                    if (quote1) {
                        char *quote2 = strchr(quote1 + 1, '"');
                        if (quote2) {
                            *quote2 = '\0';
                            strncpy(loaded_theme, quote1 + 1, sizeof(loaded_theme) - 1);
                            break;
                        }
                    }
                }
            }
            fclose(cfg);
        }
        
        /* Default to "default" theme if no theme set */
        if (loaded_theme[0] == '\0') {
            strncpy(loaded_theme, "default", sizeof(loaded_theme) - 1);
        }
        
        /* Load the PNG theme */
        PWNAUI_LOG_INFO("Loading PNG theme: %s", loaded_theme);
        theme_t *theme = theme_load(loaded_theme);
        if (theme != NULL) {
            theme_set_active(loaded_theme);
            themes_set_enabled(1);
            PWNAUI_LOG_INFO("Theme '%s' loaded and activated (PNG mode)", loaded_theme);
        } else {
            /* Try default as fallback */
            PWNAUI_LOG_WARN("Failed to load theme '%s', trying default", loaded_theme);
            theme = theme_load("default");
            if (theme != NULL) {
                theme_set_active("default");
                themes_set_enabled(1);
                PWNAUI_LOG_INFO("Fallback theme 'default' loaded (PNG mode)");
            } else {
                PWNAUI_LOG_ERR("No PNG themes available!");
            }
        }
    }
    
    /* Initialize UI state */
    init_ui_state();
    
    /* Initialize bettercap WebSocket client if enabled */
    if (g_bcap_enabled) {
        PWNAUI_LOG_INFO("Initializing bettercap WebSocket client");
        bcap_config_t bcap_config;
        bcap_config_init(&bcap_config);
        
        /* Set callbacks for real-time events */
        bcap_config.on_event = bcap_on_event;
        bcap_config.on_state_change = bcap_on_state_change;
        bcap_config.auto_reconnect = true;
        bcap_config.max_reconnect_attempts = 0;  /* Infinite retries */
        
        g_bcap_ctx = bcap_create(&bcap_config);
        if (g_bcap_ctx) {
            if (bcap_connect_async(g_bcap_ctx) == 0) {
                bcap_subscribe(g_bcap_ctx, "wifi.*");
                PWNAUI_LOG_INFO("Bettercap WebSocket connected, subscribed to wifi events");
            } else {
                PWNAUI_LOG_WARN("Bettercap WebSocket connect failed (will retry in background)");
            }
        } else {
            PWNAUI_LOG_ERR("Failed to create bettercap WebSocket context");
            g_bcap_enabled = 0;
        }
    }
    
    /* Create IPC server */
    PWNAUI_LOG_INFO("Creating IPC server at %s", socket_path);
    server_fd = ipc_server_create(socket_path);
    if (server_fd < 0) {
        PWNAUI_LOG_ERR("Failed to create IPC server");
        renderer_cleanup();
        icons_cleanup();
        font_cleanup();
        display_cleanup();
        remove_pidfile();
        return EXIT_FAILURE;
    }
    
    /* Initial render */
    renderer_render_ui(&g_ui_state, g_framebuffer);
    display_update(g_framebuffer);  /* Full update on startup */
    g_dirty = 0;
    g_last_update_ms = get_time_ms();
    
    /* Start display thread - handles all blocking display I/O */
    PWNAUI_LOG_INFO("Starting display thread");
    if (pthread_create(&g_display_thread, NULL, display_thread_func, NULL) != 0) {
        PWNAUI_LOG_ERR("Failed to create display thread");
        ipc_server_destroy(server_fd, socket_path);
        renderer_cleanup();
        icons_cleanup();
        font_cleanup();
        display_cleanup();
        remove_pidfile();
        return EXIT_FAILURE;
    }
    
    PWNAUI_LOG_INFO("PwnaUI ready, entering main loop");
    
    /* Main event loop */
    while (g_running) {
        fd_set read_fds;
        struct timeval timeout;
        int max_fd = server_fd;
        int activity;
        
        /* Handle config reload signal */
        if (g_reload_config) {
            PWNAUI_LOG_INFO("Reloading configuration");
            /* TODO: Implement config reload */
            g_reload_config = 0;
        }
        
        /* Setup select */
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_fds[i] >= 0) {
                FD_SET(client_fds[i], &read_fds);
                if (client_fds[i] > max_fd) {
                    max_fd = client_fds[i];
                }
            }
        }
        
        /* Add GPS UDP socket to select if enabled */
        int gps_fd = -1;
        if (g_native_plugins && g_plugins.gps_enabled) {
            gps_fd = plugin_gps_get_fd(&g_plugins.gps);
            if (gps_fd >= 0) {
                FD_SET(gps_fd, &read_fds);
                if (gps_fd > max_fd) {
                    max_fd = gps_fd;
                }
            }
        }
        
        /* Timeout for periodic tasks - keep short to drain accept queue quickly */
        timeout.tv_sec = 0;
        timeout.tv_usec = 10000;  /* 10ms - fast response to prevent connection pileup */
        
        activity = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        if (activity < 0) {
            if (errno == EINTR) {
                continue;  /* Signal interrupted, check g_running */
            }
            PWNAUI_LOG_ERR("select() error: %s", strerror(errno));
            break;
        }
        
        /* Check for new connections - drain ALL pending accepts */
        if (FD_ISSET(server_fd, &read_fds)) {
            /* Accept in a loop until EAGAIN (no more pending) or max clients */
            while (num_clients < MAX_CLIENTS) {
                int client_fd = ipc_server_accept(server_fd);
                if (client_fd < 0) {
                    break;  /* No more pending connections (EAGAIN) */
                }
                
                /* Find empty slot */
                int added = 0;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (client_fds[i] < 0) {
                        client_fds[i] = client_fd;
                        num_clients++;
                        PWNAUI_LOG_DEBUG("Client connected (slot %d, fd %d)", i, client_fd);
                        added = 1;
                        break;
                    }
                }
                if (!added) {
                    PWNAUI_LOG_WARN("Max clients reached, rejecting connection");
                    close(client_fd);
                    break;
                }
            }
        }
        
        /* Handle client data */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_fds[i] >= 0 && FD_ISSET(client_fds[i], &read_fds)) {
                char buffer[BUFFER_SIZE];
                char response[BUFFER_SIZE];
                ssize_t n;
                
                n = read(client_fds[i], buffer, sizeof(buffer) - 1);
                if (n < 0) {
                    /* Check if it's just EAGAIN (no data yet) vs real error */
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        continue;  /* No data available, not a disconnect */
                    }
                    /* Real error - disconnect client */
                    PWNAUI_LOG_DEBUG("Client error (slot %d): %s", i, strerror(errno));
                    close(client_fds[i]);
                    client_fds[i] = -1;
                    num_clients--;
                } else if (n == 0) {
                    /* Client closed connection */
                    PWNAUI_LOG_DEBUG("Client disconnected (slot %d)", i);
                    close(client_fds[i]);
                    client_fds[i] = -1;
                    num_clients--;
                } else {
                    buffer[n] = '\0';
                    
                    /* Handle command */
                    handle_command(buffer, response, sizeof(response));
                    
                    /* Send response and close - one-shot IPC model */
                    write(client_fds[i], response, strlen(response));
                    close(client_fds[i]);
                    client_fds[i] = -1;
                    num_clients--;
                }
            }
        }
        
        /* Handle GPS UDP data if available */
        if (gps_fd >= 0 && FD_ISSET(gps_fd, &read_fds)) {
            if (plugin_gps_handle_data(&g_plugins.gps)) {
                /* GPS data received - update UI state */
                strncpy(g_ui_state.gps, plugin_gps_get_display(&g_plugins.gps),
                        sizeof(g_ui_state.gps) - 1);
                g_dirty = 1;
            }
        }
        
        /* Update native C plugins (if enabled) */
        if (g_native_plugins) {
            int updated = plugins_update(&g_plugins);
            if (updated) {
                /* Copy plugin data to UI state */
                if (updated & 0x01) {  /* MEMTEMP */
                    strncpy(g_ui_state.memtemp_header, g_plugins.memtemp.header,
                            sizeof(g_ui_state.memtemp_header) - 1);
                    strncpy(g_ui_state.memtemp_data, g_plugins.memtemp.data,
                            sizeof(g_ui_state.memtemp_data) - 1);
                    g_dirty = 1;
                }
                if (updated & 0x02) {  /* BATTERY */
                    /* Format battery status: percentage + charging indicator */
                    if (g_plugins.battery.available) {
                        snprintf(g_ui_state.battery, sizeof(g_ui_state.battery), "BAT%d%%%s", 
                                 g_plugins.battery.percentage,
                                 g_plugins.battery.charging ? "+" : "");
                    } else {
                        snprintf(g_ui_state.battery, sizeof(g_ui_state.battery), "");
                    }
                    PWNAUI_LOG_INFO("Battery: %s", g_ui_state.battery);
                    g_dirty = 1;
                }
                if (updated & 0x04) {  /* BLUETOOTH */
                    strncpy(g_ui_state.bluetooth, g_plugins.bluetooth.status,
                            sizeof(g_ui_state.bluetooth) - 1);
                    g_dirty = 1;
                }
                if (updated & 0x08) {  /* GPS timeout check */
                    strncpy(g_ui_state.gps, plugin_gps_get_display(&g_plugins.gps),
                            sizeof(g_ui_state.gps) - 1);
                    g_dirty = 1;
                }
            }
        }
        
        /* Auto-render when dirty (rate limited) */
        if (g_dirty) {
            uint64_t now = get_time_ms();
            if (now - g_last_update_ms >= UPDATE_INTERVAL_MS) {
                renderer_render_ui(&g_ui_state, g_framebuffer);
                trigger_display_update();  /* Non-blocking - signals display thread */
                g_last_update_ms = now;
                g_dirty = 0;
            }
        }
    }
    
    PWNAUI_LOG_INFO("PwnaUI shutting down...");
    
    /* Signal display thread to exit and wait for it */
    PWNAUI_LOG_INFO("Stopping display thread...");
    pthread_mutex_lock(&g_ui_mutex);
    g_display_pending = 1;  /* Wake up thread if waiting */
    pthread_cond_signal(&g_display_cond);
    pthread_mutex_unlock(&g_ui_mutex);
    pthread_join(g_display_thread, NULL);
    PWNAUI_LOG_INFO("Display thread stopped");
    
    /* Cleanup */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_fds[i] >= 0) {
            close(client_fds[i]);
        }
    }
    
    ipc_server_destroy(server_fd, socket_path);
    
    /* Cleanup native plugins */
    if (g_native_plugins) {
        plugins_cleanup(&g_plugins);
    }
    
    /* Cleanup bettercap WebSocket client */
    if (g_bcap_enabled && g_bcap_ctx) {
        PWNAUI_LOG_INFO("Disconnecting bettercap WebSocket...");
        bcap_destroy(g_bcap_ctx);
        g_bcap_ctx = NULL;
    }
    
    /* Cleanup theme system */
    themes_cleanup();
    
    renderer_cleanup();
    icons_cleanup();
    font_cleanup();
    display_clear(0);  /* Clear to white */
    display_cleanup();
    remove_pidfile();
    
    if (g_daemon_mode) {
        closelog();
    }
    
    return EXIT_SUCCESS;
}
