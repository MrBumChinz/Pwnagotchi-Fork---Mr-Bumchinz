/*
 * bcap_test.c - Test program for bcap_ws WebSocket client
 * 
 * Compile:
 *   gcc -o bcap_test bcap_test.c bcap_ws.c cJSON.c -lpthread
 * 
 * Run:
 *   ./bcap_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "bcap_ws.h"

static volatile bool running = true;

/* Signal handler for clean shutdown */
static void signal_handler(int sig) {
    (void)sig;
    printf("\n[test] Caught signal, shutting down...\n");
    running = false;
}

/* Event callback - called when WiFi events arrive */
static void on_event(const bcap_event_t *event, void *user_data) {
    (void)user_data;
    
    char mac_str[18];
    
    switch (event->type) {
        case BCAP_EVT_AP_NEW:
            bcap_format_mac(&event->data.ap.bssid, mac_str);
            printf("[EVENT] AP NEW: %s (%s) ch=%d rssi=%d\n",
                   mac_str, event->data.ap.ssid,
                   event->data.ap.channel, event->data.ap.rssi);
            break;
            
        case BCAP_EVT_AP_LOST:
            bcap_format_mac(&event->data.ap.bssid, mac_str);
            printf("[EVENT] AP LOST: %s (%s)\n",
                   mac_str, event->data.ap.ssid);
            break;
            
        case BCAP_EVT_CLIENT_NEW:
            bcap_format_mac(&event->data.sta.mac, mac_str);
            printf("[EVENT] CLIENT NEW: %s rssi=%d\n",
                   mac_str, event->data.sta.rssi);
            break;
            
        case BCAP_EVT_HANDSHAKE:
            bcap_format_mac(&event->data.hs.ap_bssid, mac_str);
            printf("[EVENT] *** HANDSHAKE *** AP=%s SSID=%s %s%s\n",
                   mac_str, event->data.hs.ssid,
                   event->data.hs.pmkid ? "PMKID " : "",
                   event->data.hs.full ? "FULL" : "");
            break;
            
        case BCAP_EVT_DEAUTH:
            printf("[EVENT] DEAUTH detected\n");
            break;
            
        default:
            printf("[EVENT] %s\n", bcap_event_type_name(event->type));
            break;
    }
}

/* State change callback */
static void on_state_change(bool connected, void *user_data) {
    (void)user_data;
    printf("[STATE] Connection: %s\n", connected ? "CONNECTED" : "DISCONNECTED");
}

int main(int argc, char **argv) {
    printf("=== bcap_ws Test Program ===\n");
    printf("Testing pure-socket WebSocket client for bettercap\n\n");
    
    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Initialize config */
    bcap_config_t config;
    bcap_config_init(&config);
    
    /* Override host/port if provided */
    if (argc > 1) {
        strncpy(config.host, argv[1], sizeof(config.host) - 1);
    }
    if (argc > 2) {
        config.port = atoi(argv[2]);
    }
    if (argc > 3) {
        strncpy(config.username, argv[3], sizeof(config.username) - 1);
    }
    if (argc > 4) {
        strncpy(config.password, argv[4], sizeof(config.password) - 1);
    }
    
    /* Set callbacks */
    config.on_event = on_event;
    config.on_state_change = on_state_change;
    config.auto_reconnect = true;
    config.max_reconnect_attempts = 5;
    
    printf("Connecting to %s:%d%s\n", config.host, config.port, config.path);
    printf("Credentials: %s / %s\n\n", config.username, config.password);
    
    /* Create context */
    bcap_ws_ctx_t *ctx = bcap_create(&config);
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }
    
    /* Connect with async thread (handles reconnection) */
    if (bcap_connect_async(ctx) < 0) {
        fprintf(stderr, "Failed to connect\n");
        bcap_destroy(ctx);
        return 1;
    }
    
    /* Subscribe to WiFi events */
    bcap_subscribe(ctx, "wifi.*");
    
    printf("\nListening for events (Ctrl+C to quit)...\n");
    printf("-------------------------------------------\n");
    
    /* Main loop - just print stats periodically */
    int loop_count = 0;
    while (running) {
        sleep(1);
        loop_count++;
        
        /* Print stats every 10 seconds */
        if (loop_count % 10 == 0) {
            printf("[STATS] APs: %d | Stations: %d | Handshakes: %d | Connected: %s\n",
                   bcap_get_ap_count(ctx),
                   bcap_get_sta_count(ctx),
                   bcap_get_handshake_count(ctx),
                   bcap_is_connected(ctx) ? "yes" : "no");
        }
    }
    
    /* Cleanup */
    printf("\nDisconnecting...\n");
    bcap_disconnect(ctx);
    bcap_destroy(ctx);
    
    printf("Done.\n");
    return 0;
}
