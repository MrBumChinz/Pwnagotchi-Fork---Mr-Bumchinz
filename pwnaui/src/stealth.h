/**
 * stealth.h - Neurolyzer-inspired Stealth & Evasion System
 *
 * Features:
 * - Adaptive stealth levels (1=aggressive, 2=medium, 3=passive)
 * - WIDS/WIPS detection and evasion
 * - SSID whitelisting (protect home/office networks)
 * - Deauth throttling based on environment density
 * - MAC address rotation with realistic OUIs
 *
 * Inspired by: AlienMajik/pwnagotchi_plugins/neurolyzer.py
 */

#ifndef STEALTH_H
#define STEALTH_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* ============================================================================
 * Constants
 * ========================================================================== */

#define STEALTH_MAX_WHITELIST       32
#define STEALTH_MAX_SSID_LEN        64
#define STEALTH_MAX_WIDS_PATTERNS   16
#define STEALTH_MAC_STR_LEN         18

/* Minimum seconds between MAC rotations */
#define STEALTH_MIN_MAC_INTERVAL    30

/* OUI count for realistic MAC generation */
#define STEALTH_OUI_COUNT           16

/* ============================================================================
 * Stealth Levels
 * ========================================================================== */

typedef enum {
    STEALTH_LEVEL_AGGRESSIVE = 1,   /* High TX, more deauths, longer MAC interval */
    STEALTH_LEVEL_MEDIUM = 2,       /* Balanced approach */
    STEALTH_LEVEL_PASSIVE = 3       /* Low TX, fewer deauths, shorter MAC interval */
} stealth_level_t;

/* ============================================================================
 * Operation Modes (mirrors Neurolyzer)
 * ========================================================================== */

typedef enum {
    STEALTH_MODE_NORMAL = 0,        /* No stealth features */
    STEALTH_MODE_STEALTH,           /* Periodic MAC changes, basic evasion */
    STEALTH_MODE_NOIDED             /* Full evasion: MAC, channel, TX, throttle */
} stealth_mode_t;

/* ============================================================================
 * WIDS Detection Result
 * ========================================================================== */

typedef struct {
    bool detected;
    char ssid[STEALTH_MAX_SSID_LEN];
    int risk_level;                 /* 1-10, 10 = highest risk */
} wids_result_t;

/* ============================================================================
 * Stealth Configuration
 * ========================================================================== */

typedef struct {
    /* Operation mode */
    stealth_mode_t mode;

    /* Whitelist (SSIDs to never attack) */
    char whitelist[STEALTH_MAX_WHITELIST][STEALTH_MAX_SSID_LEN];
    int whitelist_count;

    /* MAC rotation */
    bool mac_rotation_enabled;
    int mac_rotation_interval;      /* Seconds between rotations */
    bool use_realistic_oui;         /* Use real vendor OUIs */

    /* Deauth throttling */
    float deauth_throttle;          /* 0.0-1.0 fraction of APs to deauth */
    int max_deauths_per_epoch;      /* Maximum deauths per epoch */

    /* WIDS detection */
    bool wids_detection_enabled;
    char wids_patterns[STEALTH_MAX_WIDS_PATTERNS][STEALTH_MAX_SSID_LEN];
    int wids_pattern_count;

    /* Adaptive behavior */
    bool adaptive_stealth;          /* Auto-adjust based on AP density */
    int crowded_threshold;          /* AP count for "crowded" (default: 20) */
    int quiet_threshold;            /* AP count for "quiet" (default: 5) */
} stealth_config_t;

/* ============================================================================
 * Stealth Context
 * ========================================================================== */

typedef struct {
    /* Configuration */
    stealth_config_t config;

    /* Current state */
    stealth_level_t current_level;
    time_t last_mac_change;
    time_t last_wids_check;
    int deauths_this_epoch;

    /* MAC tracking */
    char original_mac[STEALTH_MAC_STR_LEN];
    char current_mac[STEALTH_MAC_STR_LEN];
    bool mac_changed;

    /* Network interface */
    char interface[32];

    /* Statistics */
    int total_mac_rotations;
    int wids_detections;
    int whitelisted_skips;
    int throttled_deauths;

    /* Last adaptation time */
    time_t last_adaptation;
} stealth_ctx_t;

/* ============================================================================
 * API Functions
 * ========================================================================== */

/**
 * Create default stealth configuration
 */
stealth_config_t stealth_config_default(void);

/**
 * Initialize stealth context
 * @param config Configuration
 * @param interface Network interface name (e.g., "wlan0mon")
 * @return Stealth context or NULL on error
 */
stealth_ctx_t *stealth_create(const stealth_config_t *config, const char *interface);

/**
 * Destroy stealth context
 */
void stealth_destroy(stealth_ctx_t *ctx);

/**
 * Check if SSID is whitelisted (should not be attacked)
 * @param ctx Stealth context
 * @param ssid SSID to check
 * @return true if whitelisted
 */
bool stealth_is_whitelisted(stealth_ctx_t *ctx, const char *ssid);

/**
 * Check if a single SSID matches WIDS/honeypot patterns
 * @param ctx Stealth context
 * @param ssid SSID to check
 * @return true if SSID matches WIDS pattern
 */
bool stealth_is_wids_ap(stealth_ctx_t *ctx, const char *ssid);

/**
 * Add SSID to whitelist
 * @param ctx Stealth context
 * @param ssid SSID to add
 * @return 0 on success, -1 if full
 */
int stealth_add_whitelist(stealth_ctx_t *ctx, const char *ssid);

/**
 * Check for WIDS/WIPS networks in AP list
 * @param ctx Stealth context
 * @param ssids Array of SSIDs
 * @param count Number of SSIDs
 * @return WIDS detection result
 */
wids_result_t stealth_check_wids(stealth_ctx_t *ctx, const char **ssids, int count);

/**
 * Adapt stealth level based on environment
 * @param ctx Stealth context
 * @param ap_count Number of visible APs
 */
void stealth_adapt_level(stealth_ctx_t *ctx, int ap_count);

/**
 * Check if deauth should be throttled
 * @param ctx Stealth context
 * @return true if deauth should be skipped
 */
bool stealth_should_throttle_deauth(stealth_ctx_t *ctx);

/**
 * Record a deauth action (for throttling)
 * @param ctx Stealth context
 */
void stealth_record_deauth(stealth_ctx_t *ctx);

/**
 * Reset epoch counters (call at epoch start)
 * @param ctx Stealth context
 */
void stealth_epoch_reset(stealth_ctx_t *ctx);

/**
 * Check if MAC rotation is due
 * @param ctx Stealth context
 * @return true if rotation should happen
 */
bool stealth_should_rotate_mac(stealth_ctx_t *ctx);

/**
 * Generate a new realistic MAC address
 * @param ctx Stealth context
 * @param out_mac Buffer for new MAC (STEALTH_MAC_STR_LEN)
 * @return 0 on success
 */
int stealth_generate_mac(stealth_ctx_t *ctx, char *out_mac);

/**
 * Perform MAC address rotation
 * @param ctx Stealth context
 * @return 0 on success, -1 on error
 */
int stealth_rotate_mac(stealth_ctx_t *ctx);

/**
 * Restore original MAC address
 * @param ctx Stealth context
 * @return 0 on success
 */
int stealth_restore_mac(stealth_ctx_t *ctx);

/**
 * Get current stealth level
 * @param ctx Stealth context
 * @return Current stealth level
 */
stealth_level_t stealth_get_level(stealth_ctx_t *ctx);

/**
 * Get deauth throttle value for current level
 * @param ctx Stealth context
 * @return Throttle value 0.0-1.0
 */
float stealth_get_deauth_throttle(stealth_ctx_t *ctx);

/**
 * Get MAC rotation interval for current level
 * @param ctx Stealth context
 * @return Interval in seconds
 */
int stealth_get_mac_interval(stealth_ctx_t *ctx);

/**
 * Get human-readable stealth level name
 * @param level Stealth level
 * @return Level name string
 */
const char *stealth_level_name(stealth_level_t level);

/**
 * Get human-readable mode name
 * @param mode Stealth mode
 * @return Mode name string
 */
const char *stealth_mode_name(stealth_mode_t mode);

#endif /* STEALTH_H */
