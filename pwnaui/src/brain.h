/**
 * brain.h - Pwnagotchi Brain/Automata State Machine
 *
 * Replaces Python pwnagotchi agent.py + automata.py + epoch.py
 * Runs as part of pwnaui daemon for maximum efficiency
 */

#ifndef BRAIN_H
#define BRAIN_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include "bcap_ws.h"
#include "thompson.h"
#include "channel_bandit.h"
#include "stealth.h"
#include "wifi_recovery.h"
#include "crack_manager.h"
#include "gps.h"
#include "ap_database.h"
#include "hash_sync.h"

/* ============================================================================
 * Constants
 * ========================================================================== */

#define BRAIN_MAX_CHANNELS      64  /* 2.4GHz (1-14) + 5GHz (up to 25 channels) */
#define BRAIN_MAX_HISTORY       1000
#define BRAIN_HISTORY_TTL       60      /* 60 seconds - prevent spam, not block learning */
#define BRAIN_MAC_STR_LEN       18      /* "AA:BB:CC:DD:EE:FF\0" */

/* Attack blacklist: skip APs that resist all attempts */
#define BRAIN_BLACKLIST_MAX     64      /* Max blacklisted APs */
#define BRAIN_BLACKLIST_TTL     3600    /* Blacklist expiry: 1 hour */
#define BRAIN_BLACKLIST_THRESHOLD 20   /* Deauths before blacklisting */

/* Attack-type Thompson Sampling (#2) */
#define BRAIN_NUM_ATTACK_PHASES 8       /* Phases 0-7 */
#define BRAIN_PHASE_PMKID       0
#define BRAIN_PHASE_CSA         1
#define BRAIN_PHASE_DEAUTH      2
#define BRAIN_PHASE_PMF_BYPASS  3
#define BRAIN_PHASE_DISASSOC    4
#define BRAIN_PHASE_ROGUE_M2    5
#define BRAIN_PHASE_PROBE       6
#define BRAIN_PHASE_PASSIVE     7
#define BRAIN_ATTACK_ALPHA_INIT 1.0f    /* Uniform prior */
#define BRAIN_ATTACK_BETA_INIT  1.0f

/* ============================================================================
 * Mood System
 * ========================================================================== */

typedef enum {
    MOOD_STARTING = 0,
    MOOD_READY,
    MOOD_NORMAL,
    MOOD_BORED,
    MOOD_SAD,
    MOOD_ANGRY,
    MOOD_LONELY,
    MOOD_EXCITED,
    MOOD_GRATEFUL,
    MOOD_SLEEPING,
    MOOD_REBOOTING,
    MOOD_NUM_MOODS
} brain_mood_t;

/* String names for moods (for UI/logging) */
extern const char *brain_mood_names[];

/* Frustration diagnosis — WHY attacks are failing */
typedef enum {
    FRUST_GENERIC = 0,      /* Unknown / multiple reasons */
    FRUST_NO_CLIENTS,       /* All uncaptured APs have 0 clients */
    FRUST_WPA3,             /* All uncaptured APs are WPA3 (PMF) */
    FRUST_WEAK_SIGNAL,      /* All uncaptured APs have borderline RSSI */
    FRUST_DEAUTHS_IGNORED,  /* Sent deauths but got nothing back */
} brain_frustration_t;

extern const char *brain_frustration_names[];

/* ============================================================================
 * Configuration (from config.toml [personality] section)
 * ========================================================================== */

typedef struct {
    /* Timing */
    int recon_time;             /* Default: 30 seconds for initial recon */
    int min_recon_time;         /* Default: 5 seconds minimum on channel */
    int max_recon_time;         /* Default: 60 seconds maximum on channel */
    int hop_recon_time;         /* Default: 10 seconds after deauth */
    int ap_ttl;                 /* Default: 120 seconds before AP considered gone */
    int sta_ttl;                /* Default: 300 seconds before STA considered gone */
    
    /* Throttling */
    float throttle_a;           /* Default: 0.5 seconds delay after associate */
    float throttle_d;           /* Default: 0.5 seconds delay after deauth */
    
    /* Epoch thresholds for mood transitions */
    int bored_num_epochs;       /* Default: 15 epochs before bored */
    int sad_num_epochs;         /* Default: 25 epochs before sad */
    int excited_num_epochs;     /* Default: 10 active epochs before excited */
    int max_misses_for_recon;   /* Default: 5 misses before lonely/angry */
    int mon_max_blind_epochs;   /* Default: 50 epochs with no APs before restart */
    
    /* Features */
    bool associate;             /* Enable association attacks */
    bool deauth;                /* Enable deauthentication attacks */
    bool filter_weak;           /* Filter out weak APs */
    int min_rssi;               /* Minimum RSSI to interact with (e.g., -80) */
    
    /* Channels (NULL = all supported) */
    int *channels;
    int num_channels;
    
    /* Bond system (for peer support network) */
    float bond_encounters_factor;   /* Default: 100 */

    /* Home mode (#12) — pause attacks when home network visible */
    char home_ssid[64];             /* Home SSID (empty = disabled) */
    char home_psk[128];             /* Home WPA PSK (for auto-connect) */
    int home_min_rssi;              /* Min RSSI to trigger home mode (-60) */

    /* Sprint 8: 2nd Home (hotspot for internet) */
    char home2_ssid[64];
    char home2_psk[128];
    int  home2_min_rssi;

    /* Sprint 8: GitHub hash sync config */
    hash_sync_config_t sync_config;

    /* Sprint 6: Stealth enhancements */
    bool mac_rotation_enabled;          /* #15: Rotate wlan0mon MAC */
    int  mac_rotation_interval;         /* Seconds between rotations (default: 1800) */
    int  tx_power_min;                  /* #14: Min TX power in dBm (default: 5) */
    int  tx_power_max;                  /* #14: Max TX power in dBm (default: 30) */

    /* Sprint 6 #17: Geo-fencing */
    bool geo_fence_enabled;             /* Restrict attacks to radius */
    double geo_fence_lat;               /* Center latitude */
    double geo_fence_lon;               /* Center longitude */
    double geo_fence_radius_m;          /* Radius in meters (default: unlimited) */

    /* Sprint 7 #18: Per-attack phase enable flags (all default true) */
    bool attack_phase_enabled[BRAIN_NUM_ATTACK_PHASES];
} brain_config_t;

/* ============================================================================
 * Epoch Tracking
 * ========================================================================== */

typedef struct {
    int epoch_num;              /* Current epoch number */
    time_t epoch_started;       /* When this epoch started */
    float epoch_duration;       /* Duration of last epoch (seconds) */
    
    /* Consecutive epoch counters */
    int inactive_for;           /* Epochs with no activity */
    int active_for;             /* Epochs with activity */
    int blind_for;              /* Epochs with no visible APs */
    int sad_for;                /* Epochs in sad state */
    int bored_for;              /* Epochs in bored state */
    
    /* Current epoch stats */
    bool did_deauth;            /* Deauth'd this epoch (this channel) */
    bool did_associate;         /* Associated this epoch (this channel) */
    bool did_handshake;         /* Got handshake this epoch */
    bool any_activity;          /* Any activity this epoch */
    
    int num_deauths;            /* Number of deauths this epoch */
    int num_assocs;             /* Number of associations this epoch */
    int num_shakes;             /* Number of handshakes this epoch */
    int num_hops;               /* Number of channel hops this epoch */
    int num_missed;             /* Number of missed interactions */
    int num_slept;              /* Seconds spent sleeping */
    
    /* Peer tracking */
    int num_peers;
    float tot_bond_factor;
    float avg_bond_factor;
} brain_epoch_t;

/* ============================================================================
 * Interaction History (for throttling)
 * ========================================================================== */

typedef struct {
    char mac[BRAIN_MAC_STR_LEN];
    time_t last_interaction;
} brain_history_entry_t;

/* Attack failure tracking: counts deauths per AP with no handshake result */
typedef struct {
    char mac[BRAIN_MAC_STR_LEN];
    int deauth_count;           /* Total deauths sent to this AP */
    bool got_handshake;         /* Did we ever get a handshake? */
    time_t first_attack;        /* When we first attacked */
    /* Per-AP attack-type Thompson Sampling (#2) */
    float atk_alpha[BRAIN_NUM_ATTACK_PHASES]; /* Success counts per phase */
    float atk_beta[BRAIN_NUM_ATTACK_PHASES];  /* Failure counts per phase */
    int last_attack_phase;      /* Last phase used on this AP */
    bool is_wpa3;               /* Encryption-aware routing (#10) */
} brain_attack_tracker_t;

/* Blacklist entry: AP that resists all deauths */
typedef struct {
    char mac[BRAIN_MAC_STR_LEN];
    time_t blacklisted_at;      /* When blacklisted */
} brain_blacklist_entry_t;

/* ============================================================================
 * Main Brain Context
 * ========================================================================== */

typedef struct {
    /* Configuration */
    brain_config_t config;
    
    /* Current state */
    brain_mood_t mood;
    brain_frustration_t frustration;  /* WHY we're sad/angry */
    brain_epoch_t epoch;
    
    /* Bettercap connection */
    bcap_ws_ctx_t *bcap;
    
    /* Thompson Sampling brain (smart entity selection) */
    ts_brain_t *thompson;
    
    /* Channel bandit (smart channel selection) */
    cb_bandit_t channel_bandit;

    /* Stealth system (WIDS evasion, MAC rotation) */
    stealth_ctx_t *stealth;
    wifi_recovery_ctx_t *wifi_recovery;
    crack_mgr_t *crack_mgr;
    
    /* Current operating mode */
    ts_mode_t current_mode;
    time_t mode_started;
    int mode_handshakes;        /* Handshakes in current mode */
    
    /* Channel tracking */
    int current_channel;
    int supported_channels[BRAIN_MAX_CHANNELS];
    int num_supported_channels;
    int aps_on_channel;
    
    /* Interaction history */
    brain_history_entry_t *history;
    int history_count;
    int history_capacity;
    
    /* Attack failure tracking & blacklist */
    brain_attack_tracker_t attack_tracker[BRAIN_BLACKLIST_MAX];
    int attack_tracker_count;
    brain_blacklist_entry_t blacklist[BRAIN_BLACKLIST_MAX];
    int blacklist_count;
    
    /* Stats */
    int total_aps;
    int total_handshakes;
    char last_pwnd[64];         /* Last pwned AP hostname/MAC */
    
    /* Timing */
    time_t started_at;
    
    /* Thread control */
    pthread_t thread;
    pthread_mutex_t lock;
    bool running;
    bool started;
    
    /* Callbacks for UI integration */
    void (*on_mood_change)(brain_mood_t mood, void *user_data);
    void (*on_deauth)(const bcap_ap_t *ap, const bcap_sta_t *sta, void *user_data);
    void (*on_associate)(const bcap_ap_t *ap, void *user_data);
    void (*on_handshake)(const bcap_handshake_t *hs, void *user_data);
    void (*on_epoch)(int epoch_num, const brain_epoch_t *data, void *user_data);
    void (*on_channel_change)(int channel, void *user_data);
    void (*on_attack_phase)(int phase, void *user_data);

    void *callback_user_data;
    
    /* Pending attack tracking for deferred Thompson outcome */
    char pending_attack_mac[BRAIN_MAC_STR_LEN];
    time_t pending_attack_time;
    float pending_robustness;
    long hs_bytes_before_epoch;

    /* GPS / Mobility (#9) */
    gps_data_t *gps;                /* Pointer to global GPS data (NULL if no GPS) */
    double last_lat;                /* Last known latitude */
    double last_lon;                /* Last known longitude */
    float mobility_score;           /* 0.0 = stationary, 1.0 = fast movement */
    time_t last_mobility_check;     /* Last time we computed mobility */
    int mobility_ap_delta;          /* AP count change since last check */
    int last_ap_count;              /* Previous epoch AP count */

    /* Manual mode (custom button toggle) */
    bool manual_mode;              /* true = manual (attacks paused) */
    time_t manual_mode_toggled;    /* When mode was last changed */

    /* Home mode (#12) */
    bool home_mode_active;          /* Currently in home mode */
    time_t home_mode_entered;       /* When home mode was activated */

    /* Sprint 8: 2nd Home (hotspot) state */
    bool home2_mode_active;
    time_t home2_mode_entered;

    /* Sprint 8: Hash sync state */
    time_t last_hash_sync;
    int ap_db_upsert_count;

    /* Sprint 6: Stealth state */
    int  tx_power_current;              /* Current TX power in dBm */
    bool geo_fence_active;              /* Currently inside geo-fence? */
    time_t last_mac_rotation;           /* Timestamp of last MAC rotation */
} brain_ctx_t;

/* ============================================================================
 * API Functions
 * ========================================================================== */

/**
 * Create default configuration
 */
brain_config_t brain_config_default(void);

/**
 * Initialize brain context
 * @param config Configuration (will be copied)
 * @param bcap Bettercap WebSocket context (must be connected)
 * @return Brain context or NULL on error
 */
brain_ctx_t *brain_create(const brain_config_t *config, bcap_ws_ctx_t *bcap);

/**
 * Start the brain loop (spawns thread)
 * @param ctx Brain context
 * @return 0 on success, -1 on error
 */
int brain_start(brain_ctx_t *ctx);

/**
 * Stop the brain loop
 * @param ctx Brain context
 */
void brain_stop(brain_ctx_t *ctx);

/**
 * Destroy brain context and free resources
 * @param ctx Brain context
 */
void brain_destroy(brain_ctx_t *ctx);

/**
 * Get current mood
 * @param ctx Brain context
 * @return Current mood
 */
brain_mood_t brain_get_mood(brain_ctx_t *ctx);

/**
 * Get frustration reason (why attacks are failing).
 * Only meaningful when mood is SAD or ANGRY.
 */
brain_frustration_t brain_get_frustration(brain_ctx_t *ctx);

/**
 * Get current epoch data (thread-safe copy)
 * @param ctx Brain context
 * @param epoch Output epoch data
 */
void brain_get_epoch(brain_ctx_t *ctx, brain_epoch_t *epoch);

/**
 * Get uptime in seconds
 * @param ctx Brain context
 * @return Seconds since brain started
 */
int brain_get_uptime(brain_ctx_t *ctx);

/**
 * Set callback functions
 */
void brain_set_callbacks(brain_ctx_t *ctx,
    void (*on_mood_change)(brain_mood_t mood, void *user_data),
    void (*on_deauth)(const bcap_ap_t *ap, const bcap_sta_t *sta, void *user_data),
    void (*on_associate)(const bcap_ap_t *ap, void *user_data),
    void (*on_handshake)(const bcap_handshake_t *hs, void *user_data),
    void (*on_epoch)(int epoch_num, const brain_epoch_t *data, void *user_data),
    void (*on_channel_change)(int channel, void *user_data),
    void *user_data);

/* ============================================================================
 * Internal Functions (exposed for testing)
 * ========================================================================== */

/* Epoch management */
void brain_epoch_reset(brain_epoch_t *epoch);
void brain_epoch_track(brain_epoch_t *epoch, bool deauth, bool assoc, 
                       bool handshake, bool hop, bool miss, int inc);
void brain_epoch_next(brain_ctx_t *ctx);

/* Mood transitions */
void brain_set_mood(brain_ctx_t *ctx, brain_mood_t mood);
bool brain_has_support_network(brain_ctx_t *ctx, float factor);

/* Actions */
int brain_recon(brain_ctx_t *ctx);
int brain_set_channel(brain_ctx_t *ctx, int channel);
int brain_associate(brain_ctx_t *ctx, const bcap_ap_t *ap);
int brain_deauth(brain_ctx_t *ctx, const bcap_ap_t *ap, const bcap_sta_t *sta);

/* History/throttling */
bool brain_should_interact(brain_ctx_t *ctx, const char *mac);
void brain_add_history(brain_ctx_t *ctx, const char *mac);
void brain_prune_history(brain_ctx_t *ctx);

/* Utility */
void mac_to_str(const mac_addr_t *mac, char *str);
int str_to_mac(const char *str, mac_addr_t *mac);

/* Handshake query (checks local pcap cache, not bettercap) */
bool brain_has_full_handshake(const char *bssid);

/* Sprint 8: AP Database stats */
int brain_get_ap_db_stats(ap_db_stats_t *stats);


#endif /* BRAIN_H */
