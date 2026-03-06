/**
 * thompson_v3.h - v3.0 Brain Upgrades: LinUCB, Windowed Thompson,
 *                 Reward Shaping, Spatial/Temporal Context,
 *                 Hierarchical Priors, Predictive Scheduler, Federation
 *
 * Based on PI-BRAIN.md v3.0 Contextual Thompson Sampling architecture
 *
 * All v3.0 features are ADDITIVE — the existing Binary Thompson
 * (thompson.h) continues to work unchanged. v3.0 runs alongside it.
 *
 * Memory budget: ~87KB total (fits in Pi Zero W's 512MB with headroom)
 */

#ifndef THOMPSON_V3_H
#define THOMPSON_V3_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "thompson.h"

/* ============================================================================
 * Constants
 * ========================================================================== */

/* LinUCB */
#define V3_LINUCB_DIM           8       /* Feature vector dimension */
#define V3_LINUCB_ALPHA         1.0f    /* Exploration parameter */

/* Reward Levels */
#define V3_REWARD_LEVELS        6       /* Number of reward tiers */

/* Spatial */
#define V3_GEOHASH_PRECISION    6       /* ~1km blocks */
#define V3_GEOHASH_LEN          7       /* 6 chars + null */
#define V3_ZONE_CACHE_SIZE      100     /* LRU zone cache */
#define V3_ZONE_PARENT_PREC     5       /* Parent zone precision (~5km) */

/* Temporal */
#define V3_TEMPORAL_SLOTS       4       /* night, morning, afternoon, evening */

/* Hierarchy */
#define V3_MAX_CLUSTERS         32      /* Max cluster keys */
#define V3_CLUSTER_KEY_LEN      32      /* "vendor4_enc_band\0" */
#define V3_POP_MAX_ESS          100.0f  /* Population prior ESS cap */
#define V3_CLUSTER_MAX_ESS      50.0f   /* Cluster prior ESS cap */
#define V3_HIERARCHY_ESS_LOW    5.0f    /* Below: use population/cluster */
#define V3_HIERARCHY_ESS_MID    20.0f   /* Above: trust entity data */

/* Predictive Scheduler */
#define V3_SCHED_HORIZON_SEC    300     /* 5-minute planning horizon */
#define V3_SCHED_MAX_TARGETS    16      /* Max targets in schedule */

/* Windowed Thompson */
#define V3_WINDOWED_GAMMA       0.95f   /* Exponential discount factor */
#define V3_WINDOWED_MIN_ESS     2.0f    /* Floor ESS to prevent instability */

/* ============================================================================
 * Reward Level Enum
 * ========================================================================== */

typedef enum {
    REWARD_NOTHING       = 0,   /* No useful data captured */
    REWARD_BEACON        = 1,   /* Beacon seen, AP confirmed alive */
    REWARD_CLIENTS       = 2,   /* Client stations detected */
    REWARD_EAPOL_PARTIAL = 3,   /* Partial EAPOL exchange (1-2 frames) */
    REWARD_HANDSHAKE     = 4,   /* Full 4-way handshake captured */
    REWARD_VERIFIED      = 5,   /* Handshake verified / cracked */
} v3_reward_level_t;

/* Continuous shaped reward values for each level */
static const float V3_SHAPED_REWARDS[V3_REWARD_LEVELS] = {
    0.00f,  /* NOTHING */
    0.05f,  /* BEACON */
    0.15f,  /* CLIENTS */
    0.40f,  /* EAPOL_PARTIAL */
    0.85f,  /* HANDSHAKE */
    1.00f,  /* VERIFIED */
};

/* Threshold for binary Thompson: >= HANDSHAKE counts as success */
#define V3_BINARY_THRESHOLD     REWARD_HANDSHAKE

/* ============================================================================
 * LinUCB Contextual Bandit
 * ========================================================================== */

/**
 * LinUCB state: shared across all entities (global model).
 * A = d×d matrix (feature covariance)
 * b = d-vector (reward-weighted features)
 * theta = A^{-1} * b (weight vector, recomputed on demand)
 *
 * Memory: 8*8*4 + 8*4 + 8*4 = 320 bytes
 */
typedef struct {
    float A[V3_LINUCB_DIM][V3_LINUCB_DIM];  /* Feature covariance matrix */
    float b[V3_LINUCB_DIM];                   /* Reward-weighted feature sum */
    float alpha;                               /* Exploration parameter */
    uint32_t observation_count;                /* Total observations fed */
} v3_linucb_t;

/**
 * Feature vector for LinUCB.
 * 8 features:
 *   [0] vendor_cat    - Vendor category (0-7, normalized to 0-1)
 *   [1] encryption    - Encryption type (0=open, 0.33=WEP, 0.66=WPA2, 1=WPA3)
 *   [2] channel_norm  - Channel / 165.0
 *   [3] rssi_norm     - (RSSI + 100) / 70.0 clamped to [0,1]
 *   [4] time_of_day   - hour / 24.0
 *   [5] day_of_week   - day / 7.0
 *   [6] zone_prior    - Spatial zone success rate (0.5 if unknown)
 *   [7] ess_log       - log2(entity ESS + 1) / 10.0
 */
typedef struct {
    float x[V3_LINUCB_DIM];
} v3_feature_t;

/* ============================================================================
 * Windowed (Discounted) Thompson Sampling
 * ========================================================================== */

/**
 * Per-entity windowed Thompson state.
 * Uses exponential discounting: on each observation,
 *   alpha_w = gamma * alpha_w + success_weight
 *   beta_w  = gamma * beta_w  + (1 - success_weight)
 *
 * Floor: if alpha_w + beta_w < MIN_ESS, inject from hierarchy.
 */
typedef struct {
    float alpha_w;              /* Discounted success count */
    float beta_w;               /* Discounted failure count */
    float gamma;                /* Discount factor (0.95) */
} v3_windowed_ts_t;

/* ============================================================================
 * Spatial Zone Priors (Geohash LRU Cache)
 * ========================================================================== */

typedef struct {
    char geohash[V3_GEOHASH_LEN];
    float alpha;
    float beta;
    time_t last_used;
} v3_zone_entry_t;

typedef struct {
    v3_zone_entry_t zones[V3_ZONE_CACHE_SIZE];
    int count;
} v3_zone_cache_t;

/* ============================================================================
 * Temporal Context
 * ========================================================================== */

typedef struct {
    uint32_t success[V3_TEMPORAL_SLOTS];  /* Success count per slot */
    uint32_t total[V3_TEMPORAL_SLOTS];    /* Total observations per slot */
} v3_temporal_stats_t;

/* Temporal slot names: night=0, morning=1, afternoon=2, evening=3 */
static inline int v3_temporal_slot(int hour) {
    if (hour < 6) return 0;       /* night */
    if (hour < 12) return 1;      /* morning */
    if (hour < 18) return 2;      /* afternoon */
    return 3;                     /* evening */
}

/* ============================================================================
 * Hierarchical Bayesian Priors
 * ========================================================================== */

typedef struct {
    char key[V3_CLUSTER_KEY_LEN];   /* "aabb_wpa2_2g" */
    float alpha;
    float beta;
} v3_cluster_prior_t;

typedef struct {
    /* Population level (global) */
    float pop_alpha;
    float pop_beta;

    /* Cluster level */
    v3_cluster_prior_t clusters[V3_MAX_CLUSTERS];
    int cluster_count;
} v3_hierarchy_t;

/* ============================================================================
 * Predictive Attack Scheduler
 * ========================================================================== */

typedef struct {
    char entity_id[TS_MAC_STR_LEN];
    float expected_reward;      /* LinUCB predicted reward */
    float contact_time_sec;     /* Estimated remaining contact time */
    float attack_time_sec;      /* Estimated attack duration */
    int preferred_phase;        /* Best attack phase for this AP */
} v3_sched_target_t;

typedef struct {
    v3_sched_target_t targets[V3_SCHED_MAX_TARGETS];
    int target_count;
    float horizon_sec;
    time_t generated_at;
} v3_attack_schedule_t;

/* ============================================================================
 * Federation Export (anonymized, for fleet learning)
 * ========================================================================== */

typedef struct {
    /* LinUCB global model */
    float linucb_A[V3_LINUCB_DIM][V3_LINUCB_DIM];
    float linucb_b[V3_LINUCB_DIM];

    /* Population prior */
    float pop_alpha;
    float pop_beta;

    /* Cluster priors (anonymized keys) */
    v3_cluster_prior_t clusters[V3_MAX_CLUSTERS];
    int cluster_count;

    /* Zone priors (geohash only, no GPS coords) */
    v3_zone_entry_t zones[V3_ZONE_CACHE_SIZE];
    int zone_count;

    /* Aggregate stats */
    uint32_t total_observations;
    uint32_t entity_count;
    uint32_t device_id_hash;    /* Anonymized device ID */
} v3_federation_export_t;

/* ============================================================================
 * v3 Brain Extension Context
 * ========================================================================== */

/**
 * Master v3.0 context — sits alongside the existing ts_brain_t.
 * Created once, attached to brain_ctx_t.
 */
typedef struct {
    /* LinUCB global model */
    v3_linucb_t linucb;

    /* Spatial zone cache */
    v3_zone_cache_t zones;

    /* Global temporal stats */
    v3_temporal_stats_t temporal;

    /* Hierarchical priors */
    v3_hierarchy_t hierarchy;

    /* Current attack schedule */
    v3_attack_schedule_t schedule;

    /* Per-entity windowed Thompson (parallel to ts_entity_t) */
    v3_windowed_ts_t windowed[TS_MAX_ENTITIES];

    /* Per-entity reward level counts */
    uint16_t reward_counts[TS_MAX_ENTITIES][V3_REWARD_LEVELS];

    /* Per-entity temporal stats */
    v3_temporal_stats_t entity_temporal[TS_MAX_ENTITIES];

    /* Stats */
    uint32_t linucb_updates;
    uint32_t schedule_runs;
    time_t last_schedule_time;
} v3_brain_t;

/* ============================================================================
 * API Functions
 * ========================================================================== */

/* --- Lifecycle --- */
v3_brain_t *v3_brain_create(void);
void v3_brain_destroy(v3_brain_t *v3);

/* --- LinUCB --- */
void v3_linucb_init(v3_linucb_t *model);
void v3_linucb_update(v3_linucb_t *model, const v3_feature_t *features,
                       float reward);
float v3_linucb_predict(const v3_linucb_t *model, const v3_feature_t *features);

/* Build feature vector for an entity */
v3_feature_t v3_build_features(const ts_entity_t *entity,
                                const v3_brain_t *v3,
                                double latitude, double longitude);

/* --- Windowed Thompson --- */
void v3_windowed_init(v3_windowed_ts_t *wt);
void v3_windowed_observe(v3_windowed_ts_t *wt, bool success, float weight);
float v3_windowed_sample(const v3_windowed_ts_t *wt);
void v3_windowed_inject_prior(v3_windowed_ts_t *wt, float prior_alpha,
                               float prior_beta, float blend);

/* --- Reward Shaping --- */
float v3_shaped_reward(v3_reward_level_t level);
bool v3_is_binary_success(v3_reward_level_t level);

/* --- Spatial Zones --- */
void v3_geohash_encode(double lat, double lon, int precision, char *out);
v3_zone_entry_t *v3_zone_lookup(v3_zone_cache_t *cache, const char *geohash);
void v3_zone_update(v3_zone_cache_t *cache, const char *geohash,
                     bool success, float weight);
float v3_zone_prior(const v3_zone_cache_t *cache, const char *geohash);

/* --- Temporal --- */
void v3_temporal_observe(v3_temporal_stats_t *stats, int hour,
                          bool success);
float v3_temporal_rate(const v3_temporal_stats_t *stats, int hour);

/* --- Hierarchy --- */
void v3_hierarchy_init(v3_hierarchy_t *h);
void v3_hierarchy_build_cluster_key(const ts_entity_t *entity, char *out,
                                     int out_len);
v3_cluster_prior_t *v3_hierarchy_get_cluster(v3_hierarchy_t *h,
                                              const char *key);
void v3_hierarchy_update_population(v3_hierarchy_t *h, float alpha, float beta);
void v3_hierarchy_update_cluster(v3_hierarchy_t *h, const char *key,
                                  float alpha, float beta);
void v3_hierarchy_get_prior(const v3_hierarchy_t *h, const ts_entity_t *entity,
                             float *out_alpha, float *out_beta);

/* --- Predictive Scheduler --- */
void v3_schedule_build(v3_brain_t *v3, ts_brain_t *ts,
                        double lat, double lon, int mobility_mode);

/* --- Combined Scoring (Thompson + LinUCB) --- */
float v3_score_entity(v3_brain_t *v3, ts_brain_t *ts,
                       ts_entity_t *entity, int entity_idx,
                       const ts_action_t *action,
                       double lat, double lon);

/* --- Full v3 Observation Update --- */
void v3_observe(v3_brain_t *v3, ts_brain_t *ts,
                 ts_entity_t *entity, int entity_idx,
                 v3_reward_level_t reward_level,
                 double lat, double lon);

/* --- Federation --- */
void v3_federation_export(const v3_brain_t *v3, const ts_brain_t *ts,
                           v3_federation_export_t *out);
void v3_federation_import(v3_brain_t *v3, const v3_federation_export_t *fleet,
                           float blend_factor);

/* --- Persistence --- */
int v3_save_state(const v3_brain_t *v3, const char *path);
int v3_load_state(v3_brain_t *v3, const char *path);

/* --- PC Distillation Import/Export (JSON) --- */
int v3_distillation_import(v3_brain_t *v3, ts_brain_t *ts,
                           const char *json_path);
int v3_state_export_json(const v3_brain_t *v3, const ts_brain_t *ts,
                          const char *json_path);

#endif /* THOMPSON_V3_H */
