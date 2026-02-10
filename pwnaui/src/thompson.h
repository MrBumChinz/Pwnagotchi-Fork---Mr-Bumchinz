/**
 * thompson.h - Thompson Sampling for Entity Selection
 *
 * Binary Thompson Sampling on resource-constrained devices
 * Based on PI-BRAIN.md v2.2 architecture
 *
 * Key principles:
 * - Thompson learns on BINARY outcomes only (success=1, failure=0)
 * - Cost-aware scoring: optimize success per cost
 * - Entity lifecycle: active ??? stale ??? archived ??? evicted
 * - EWMA + MAD for robust signal tracking
 */

#ifndef THOMPSON_H
#define THOMPSON_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>

/* ============================================================================
 * Constants
 * ========================================================================== */

#define TS_MAX_ENTITIES         200     /* Max active entities */
#define TS_IDENTITY_HASH_LEN    17      /* 16 chars + null */
#define TS_MAC_STR_LEN          18      /* "AA:BB:CC:DD:EE:FF\0" */
#define TS_SSID_MAX_LEN         33      /* 32 chars + null */
#define TS_VENDOR_MAX_LEN       9       /* 8 chars + null (OUI) */

/* Entity lifecycle thresholds (days) */
#define TS_STALE_DAYS           7       /* Reduce weight after 7 days */
#define TS_ARCHIVE_DAYS         30      /* Archive after 30 days */
#define TS_EVICT_DAYS           90      /* Delete after 90 days */

/* History TTL for throttling */
#define TS_HISTORY_TTL_SECS     1800    /* 30 minutes */

/* ============================================================================
 * Entity Status
 * ========================================================================== */

typedef enum {
    ENTITY_ACTIVE = 0,      /* Seen recently, full participation */
    ENTITY_STALE,           /* 7-30 days, reduced weight */
    ENTITY_ARCHIVED,        /* >30 days, frozen */
    ENTITY_FLAGGED          /* Marked as problematic, skip */
} ts_entity_status_t;

/* ============================================================================
 * Action Costs
 * ========================================================================== */

typedef struct {
    const char *name;
    float cost_time;        /* Seconds */
    float cost_energy;      /* mAh estimate */
    float cost_risk;        /* Detection risk 0-1 */
} ts_action_t;

/* Predefined actions */
extern const ts_action_t TS_ACTION_PROBE;
extern const ts_action_t TS_ACTION_PASSIVE_SCAN;
extern const ts_action_t TS_ACTION_ASSOCIATE;
extern const ts_action_t TS_ACTION_DEAUTH;
extern const ts_action_t TS_ACTION_WAIT;

/* ============================================================================
 * Signal Tracker (EWMA + MAD)
 * ========================================================================== */

#define TS_MAD_WINDOW_SIZE  10

typedef struct {
    float level;                        /* EWMA smoothed RSSI */
    float alpha;                        /* EWMA smoothing factor (0.3 default) */
    int8_t window[TS_MAD_WINDOW_SIZE];  /* Recent RSSI samples */
    int window_count;                   /* Samples in window */
    int window_idx;                     /* Circular buffer index */
} ts_signal_tracker_t;

/* ============================================================================
 * Entity (AP or Station)
 * ========================================================================== */

typedef struct {
    /* Identity */
    char entity_id[TS_MAC_STR_LEN];     /* Primary ID (MAC address) */
    char soft_identity[TS_IDENTITY_HASH_LEN]; /* Behavioral hash */
    
    /* Metadata for soft identity */
    char ssid[TS_SSID_MAX_LEN];
    char vendor_oui[TS_VENDOR_MAX_LEN];
    uint8_t channel;
    uint16_t beacon_interval;           /* ms, bucketed to 50ms */
    char encryption[16];                /* "WPA2", "OPEN", etc. */
    
    /* Thompson Sampling state (BINARY ONLY) */
    float alpha;                        /* Success count + prior (starts at 1.0) */
    float beta;                         /* Failure count + prior (starts at 1.0) */
    float client_boost;                 /* Boost for APs with clients (1.0 = neutral) */
    
    /* Signal tracking */
    ts_signal_tracker_t signal;
    int8_t last_rssi;
    
    /* Lifecycle */
    ts_entity_status_t status;
    time_t first_seen;
    time_t last_seen;
    time_t last_attacked;               /* Per-AP cooldown timer (AngryOxide timer_interact) */
    
    /* Analytics (separate from Thompson learning) */
    uint32_t total_interactions;
    uint32_t total_successes;
    float last_cost_seconds;
    
    /* Internal */
    bool in_use;                        /* Slot is occupied */
} ts_entity_t;

/* ============================================================================
 * Mode Bandit (Global operating mode)
 * ========================================================================== */

typedef enum {
    MODE_PASSIVE_DISCOVERY = 0,
    MODE_ACTIVE_TARGETING,
    MODE_COOLDOWN,
    MODE_SYNC_WINDOW,
    MODE_COUNT
} ts_mode_t;

typedef struct {
    float alpha[MODE_COUNT];            /* Mode success priors */
    float beta[MODE_COUNT];             /* Mode failure priors */
    ts_mode_t current_mode;
    time_t mode_started;
} ts_mode_bandit_t;

/* ============================================================================
 * Thompson Brain Context
 * ========================================================================== */

typedef struct {
    /* Entity storage (fixed array for memory predictability) */
    ts_entity_t entities[TS_MAX_ENTITIES];
    int entity_count;
    
    /* Mode bandit */
    ts_mode_bandit_t mode;
    
    /* Cost weights for scoring */
    float cost_weight_time;             /* Default: 1.0 */
    float cost_weight_energy;           /* Default: 20.0 */
    float cost_weight_risk;             /* Default: 5.0 */
    float exploration_bonus;            /* Default: 0.3 */
    
    /* Stats */
    uint32_t total_decisions;
    uint32_t total_handshakes;
    time_t started_at;
    
    /* Thread safety */
    pthread_mutex_t lock;
} ts_brain_t;

/* ============================================================================
 * API Functions
 * ========================================================================== */

/**
 * Initialize Thompson brain
 */
ts_brain_t *ts_brain_create(void);

/**
 * Destroy brain and free resources
 */
void ts_brain_destroy(ts_brain_t *brain);

/**
 * Find or create entity by MAC
 * @param brain Brain context
 * @param mac MAC address string "AA:BB:CC:DD:EE:FF"
 * @return Entity pointer or NULL if full
 */
ts_entity_t *ts_get_or_create_entity(ts_brain_t *brain, const char *mac);

/**
 * Find entity by MAC
 * @return Entity pointer or NULL if not found
 */
ts_entity_t *ts_find_entity(ts_brain_t *brain, const char *mac);

/**
 * Update entity metadata (for soft identity computation)
 */
void ts_update_entity_metadata(ts_entity_t *entity,
                               const char *ssid,
                               const char *vendor_oui,
                               uint8_t channel,
                               uint16_t beacon_interval,
                               const char *encryption);

/**
 * Observe outcome for entity (BINARY ONLY)
 * @param entity Entity that was interacted with
 * @param success Did the interaction succeed? (handshake, assoc ACK, etc.)
 * @param robustness_score Signal quality weight 0.1-1.0 (from MAD)
 */
void ts_observe_outcome(ts_entity_t *entity, bool success, float robustness_score);

/**
 * Update signal tracker with new RSSI
 * @return Robustness score (0-1, higher = more stable signal)
 */
float ts_update_signal(ts_entity_t *entity, int8_t rssi);

/**
 * Thompson Sampling: select best entity
 * @param brain Brain context
 * @param entities Array of candidate entities
 * @param count Number of candidates
 * @param action Action to be performed (for cost calculation)
 * @return Best entity or NULL
 */
ts_entity_t *ts_decide_entity(ts_brain_t *brain,
                              ts_entity_t **entities,
                              int count,
                              const ts_action_t *action);

/**
 * Score an entity for an action (success per cost with exploration bonus)
 */
float ts_score_entity(ts_brain_t *brain, ts_entity_t *entity, const ts_action_t *action);

/**
 * Sample from Beta distribution
 * Uses Gamma sampling: Beta(a,b) = Gamma(a,1) / (Gamma(a,1) + Gamma(b,1))
 */
float ts_beta_sample(float alpha, float beta);

/**
 * Get effective sample size (evidence strength)
 */
static inline float ts_ess(ts_entity_t *entity) {
    return entity->alpha + entity->beta;
}

/**
 * Get success rate estimate
 */
static inline float ts_success_rate(ts_entity_t *entity) {
    float ess = entity->alpha + entity->beta;
    return ess > 0 ? entity->alpha / ess : 0.5f;
}

/**
 * Decay priors toward neutral (1,1) based on dormancy
 */
void ts_decay_entity(ts_entity_t *entity, time_t now);

/**
 * Garbage collect: remove old entities, decay stale ones
 * @return Number of entities evicted
 */
int ts_garbage_collect(ts_brain_t *brain);

/**
 * Compute soft identity hash from metadata
 * Result written to entity->soft_identity
 */
void ts_compute_soft_identity(ts_entity_t *entity);

/**
 * Check if entity identity has drifted (AP reset, MAC rotation)
 * @param entity Existing entity
 * @param new_ssid New observation SSID
 * @param new_vendor New observation vendor
 * @param new_channel New observation channel
 * @param new_beacon New observation beacon interval
 * @param new_encryption New observation encryption
 * @return true if identity drifted significantly
 */
bool ts_detect_identity_drift(ts_entity_t *entity,
                              const char *new_ssid,
                              const char *new_vendor,
                              uint8_t new_channel,
                              uint16_t new_beacon,
                              const char *new_encryption);

/* ============================================================================
 * Mode Bandit API
 * ========================================================================== */

/**
 * Select operating mode via Thompson Sampling
 */
ts_mode_t ts_select_mode(ts_brain_t *brain);

/**
 * Observe mode outcome
 */
void ts_observe_mode_outcome(ts_brain_t *brain, ts_mode_t mode, bool success);

/**
 * Get mode name string
 */
const char *ts_mode_name(ts_mode_t mode);

/* ============================================================================
 * Persistence
 * ========================================================================== */

/**
 * Save brain state to file
 */
int ts_save_state(ts_brain_t *brain, const char *path);

/**
 * Load brain state from file
 */
int ts_load_state(ts_brain_t *brain, const char *path);

#endif /* THOMPSON_H */
