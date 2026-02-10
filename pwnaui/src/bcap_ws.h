/*
 * bcap_ws.h - Bettercap WebSocket Client
 * 
 * Connects to bettercap's WebSocket API for real-time WiFi events.
 * Replaces Python HTTP polling with native event streaming.
 * 
 * Part of the PWND project - Phase 3 (WebSocket)
 */

#ifndef BCAP_WS_H
#define BCAP_WS_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * Configuration
 * ========================================================================== */

#define BCAP_DEFAULT_HOST       "127.0.0.1"
#define BCAP_DEFAULT_PORT       8081
#define BCAP_DEFAULT_PATH       "/api/events"
#define BCAP_MAX_URL_LEN        256
#define BCAP_MAX_USER_LEN       64
#define BCAP_MAX_PASS_LEN       64
#define BCAP_RECONNECT_DELAY_MS 5000
#define BCAP_HEARTBEAT_MS       30000
#define BCAP_RX_BUFFER_SIZE     65536
#define BCAP_MAX_APS            256
#define BCAP_MAX_STAS           512

/* ==========================================================================
 * WiFi Event Types (from bettercap)
 * ========================================================================== */

typedef enum {
    BCAP_EVT_NONE = 0,
    
    /* AP Events */
    BCAP_EVT_AP_NEW,            /* wifi.ap.new */
    BCAP_EVT_AP_LOST,           /* wifi.ap.lost */
    
    /* Client Events */
    BCAP_EVT_CLIENT_NEW,        /* wifi.client.new */
    BCAP_EVT_CLIENT_LOST,       /* wifi.client.lost */
    BCAP_EVT_CLIENT_PROBE,      /* wifi.client.probe */
    
    /* Handshake Events */
    BCAP_EVT_HANDSHAKE,         /* wifi.client.handshake */
    
    /* Deauth Events */
    BCAP_EVT_DEAUTH,            /* wifi.deauthentication */
    
    /* Internal Events */
    BCAP_EVT_CONNECTED,         /* WebSocket connected */
    BCAP_EVT_DISCONNECTED,      /* WebSocket disconnected */
    BCAP_EVT_ERROR,             /* Error occurred */
    
    BCAP_EVT_COUNT
} bcap_event_type_t;

/* ==========================================================================
 * Data Structures
 * ========================================================================== */

/* MAC address (6 bytes) */
typedef struct {
    uint8_t addr[6];
} mac_addr_t;

/* Access Point information */
typedef struct {
    mac_addr_t bssid;           /* AP MAC address */
    char ssid[33];              /* SSID (max 32 chars + null) */
    int8_t rssi;                /* Signal strength (dBm) */
    uint8_t channel;            /* WiFi channel (1-14, 36-165) */
    uint16_t beacon_interval;   /* Beacon interval (ms) */
    char encryption[32];        /* e.g., "WPA2", "OPEN", "WEP" */
    char vendor[64];            /* Vendor from OUI lookup */
    time_t first_seen;          /* Unix timestamp */
    time_t last_seen;           /* Unix timestamp */
    uint32_t clients_count;     /* Number of associated clients */
    bool pmkid_available;       /* PMKID captured? */
    bool handshake_captured;    /* Full handshake captured? */
} bcap_ap_t;

/* Station (client) information */
typedef struct {
    mac_addr_t mac;             /* Client MAC address */
    mac_addr_t ap_bssid;        /* Associated AP (if any) */
    char vendor[64];            /* Vendor from OUI lookup */
    int8_t rssi;                /* Signal strength (dBm) */
    time_t first_seen;          /* Unix timestamp */
    time_t last_seen;           /* Unix timestamp */
    char probed_ssids[5][33];   /* Last 5 probed SSIDs */
    uint8_t probe_count;        /* Number of probed SSIDs */
    bool associated;            /* Is associated to an AP? */
} bcap_sta_t;

/* Handshake information */
typedef struct {
    mac_addr_t ap_bssid;        /* AP that handshake is for */
    mac_addr_t client_mac;      /* Client involved */
    char ssid[33];              /* AP SSID */
    char pcap_file[256];        /* Path to saved pcap */
    bool pmkid;                 /* Is PMKID (vs full handshake)? */
    bool full;                  /* Full 4-way handshake? */
    time_t captured_at;         /* Unix timestamp */
} bcap_handshake_t;

/* Generic WiFi event */
typedef struct {
    bcap_event_type_t type;     /* Event type */
    time_t timestamp;           /* When event occurred */
    
    union {
        bcap_ap_t ap;           /* AP data (for AP events) */
        bcap_sta_t sta;         /* Station data (for client events) */
        bcap_handshake_t hs;    /* Handshake data */
        struct {
            int code;
            char message[256];
        } error;                /* Error info */
    } data;
} bcap_event_t;

/* ==========================================================================
 * Callback Types
 * ========================================================================== */

/* Called when a WiFi event is received */
typedef void (*bcap_event_callback_t)(const bcap_event_t *event, void *user_data);

/* Called when connection state changes */
typedef void (*bcap_state_callback_t)(bool connected, void *user_data);

/* ==========================================================================
 * WebSocket Context
 * ========================================================================== */

typedef struct bcap_ws_ctx bcap_ws_ctx_t;

/* ==========================================================================
 * Configuration Structure
 * ========================================================================== */

typedef struct {
    char host[BCAP_MAX_URL_LEN];
    uint16_t port;
    char path[BCAP_MAX_URL_LEN];
    char username[BCAP_MAX_USER_LEN];
    char password[BCAP_MAX_PASS_LEN];
    bool use_ssl;
    int reconnect_delay_ms;
    int heartbeat_interval_ms;
    bool auto_reconnect;          /* Auto-reconnect on disconnect */
    int max_reconnect_attempts;   /* Max retries before giving up */
    
    /* Callbacks */
    bcap_event_callback_t on_event;
    bcap_state_callback_t on_state_change;
    void *user_data;
} bcap_config_t;

/* ==========================================================================
 * API Functions
 * ========================================================================== */

/**
 * Initialize default configuration
 */
void bcap_config_init(bcap_config_t *config);

/**
 * Create a new bettercap WebSocket context
 * @param config Configuration (NULL for defaults)
 * @return Context pointer or NULL on error
 */
bcap_ws_ctx_t* bcap_create(const bcap_config_t *config);

/**
 * Destroy context and free resources
 */
void bcap_destroy(bcap_ws_ctx_t *ctx);

/**
 * Connect to bettercap WebSocket (blocking)
 * @return 0 on success, -1 on error
 */
int bcap_connect(bcap_ws_ctx_t *ctx);

/**
 * Connect and start background service thread
 * Handles auto-reconnection and heartbeat
 * @return 0 on success, -1 on error
 */
int bcap_connect_async(bcap_ws_ctx_t *ctx);

/**
 * Disconnect from bettercap
 */
void bcap_disconnect(bcap_ws_ctx_t *ctx);

/**
 * Check if connected
 */
bool bcap_is_connected(bcap_ws_ctx_t *ctx);

/**
 * Process pending events (call in main loop)
 * @param timeout_ms Max time to wait for events (0 = non-blocking)
 * @return Number of events processed, -1 on error
 */
int bcap_poll(bcap_ws_ctx_t *ctx, int timeout_ms);

/**
 * Subscribe to event stream
 * @param stream Event stream name (e.g., "wifi.events")
 * @return 0 on success, -1 on error
 */
int bcap_subscribe(bcap_ws_ctx_t *ctx, const char *stream);

/**
 * Send command to bettercap
 * @param cmd Command string (e.g., "wifi.deauth ff:ff:ff:ff:ff:ff")
 * @return 0 on success, -1 on error
 */
int bcap_send_command(bcap_ws_ctx_t *ctx, const char *cmd);
int bcap_poll_aps(bcap_ws_ctx_t *ctx);

/**
 * Check if a full REST API sync is needed.
 * Returns true if initial sync not done or BCAP_SYNC_INTERVAL_S elapsed.
 * Between syncs, AP/STA data is maintained via WebSocket events.
 */
bool bcap_needs_sync(bcap_ws_ctx_t *ctx);

/**
 * Request AP list refresh
 */
int bcap_refresh_aps(bcap_ws_ctx_t *ctx);

/**
 * Request station list refresh
 */
int bcap_refresh_stations(bcap_ws_ctx_t *ctx);

/* ==========================================================================
 * State Access (thread-safe)
 * ========================================================================== */

/**
 * Get current AP count
 */
int bcap_get_ap_count(bcap_ws_ctx_t *ctx);

/**
 * Get current station count
 */
int bcap_get_sta_count(bcap_ws_ctx_t *ctx);
int bcap_get_sta(bcap_ws_ctx_t *ctx, int index, bcap_sta_t *sta);

/**
 * Get total handshakes captured
 */
int bcap_get_handshake_count(bcap_ws_ctx_t *ctx);

/**
 * Copy AP by index (thread-safe)
 * @param index AP index (0 to ap_count-1)
 * @param ap Output buffer
 * @return 0 on success, -1 if index out of range
 */
int bcap_get_ap(bcap_ws_ctx_t *ctx, int index, bcap_ap_t *ap);

/**
 * Find AP by BSSID
 * @param bssid MAC address to find
 * @param ap Output buffer
 * @return 0 on success, -1 if not found
 */
int bcap_find_ap(bcap_ws_ctx_t *ctx, const mac_addr_t *bssid, bcap_ap_t *ap);

/* ==========================================================================
 * Utility Functions
 * ========================================================================== */

/**
 * Parse MAC address string to mac_addr_t
 * @param str String like "aa:bb:cc:dd:ee:ff"
 * @param mac Output buffer
 * @return 0 on success, -1 on parse error
 */
int bcap_parse_mac(const char *str, mac_addr_t *mac);

/**
 * Format mac_addr_t to string
 * @param mac MAC address
 * @param buf Output buffer (at least 18 bytes)
 */
void bcap_format_mac(const mac_addr_t *mac, char *buf);

/**
 * Get event type name
 */
const char* bcap_event_type_name(bcap_event_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* BCAP_WS_H */
