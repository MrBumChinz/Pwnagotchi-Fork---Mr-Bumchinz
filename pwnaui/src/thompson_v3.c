/**
 * thompson_v3.c - v3.0 Brain Implementation
 *
 * LinUCB contextual bandits, windowed Thompson sampling,
 * reward shaping, spatial/temporal context, hierarchical priors,
 * predictive scheduling, and federation export.
 *
 * Designed for Pi Zero W (ARMv6, 512MB RAM):
 * - No dynamic allocation in hot path
 * - All matrix ops are 8x8 (< 0.1ms per update)
 * - Total memory: ~87KB
 *
 * Based on PI-BRAIN.md v3.0 architecture
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "thompson_v3.h"
#include "cJSON.h"

/* Reuse RNG from thompson.c */
extern float ts_beta_sample(float alpha, float beta);

/* Simple uniform [0,1) — uses system rand() as fallback */
static float v3_randf(void) {
    return (float)rand() / (float)RAND_MAX;
}

/* ============================================================================
 * Lifecycle
 * ========================================================================== */

v3_brain_t *v3_brain_create(void) {
    v3_brain_t *v3 = calloc(1, sizeof(v3_brain_t));
    if (!v3) return NULL;

    v3_linucb_init(&v3->linucb);
    v3_hierarchy_init(&v3->hierarchy);

    /* Initialize windowed Thompson for all entity slots */
    for (int i = 0; i < TS_MAX_ENTITIES; i++) {
        v3_windowed_init(&v3->windowed[i]);
    }

    return v3;
}

void v3_brain_destroy(v3_brain_t *v3) {
    if (v3) free(v3);
}

/* ============================================================================
 * LinUCB Contextual Bandit
 * ========================================================================== */

void v3_linucb_init(v3_linucb_t *model) {
    memset(model, 0, sizeof(v3_linucb_t));

    /* A = Identity matrix */
    for (int i = 0; i < V3_LINUCB_DIM; i++) {
        model->A[i][i] = 1.0f;
    }

    model->alpha = V3_LINUCB_ALPHA;
    model->observation_count = 0;
}

/**
 * Solve 8x8 linear system A*theta = b using Gauss-Jordan elimination.
 * Modifies a copy of A. Returns theta in out[].
 * Returns false if singular.
 */
static bool solve_8x8(const float A[V3_LINUCB_DIM][V3_LINUCB_DIM],
                       const float b[V3_LINUCB_DIM],
                       float out[V3_LINUCB_DIM]) {
    /* Augmented matrix [A | b] */
    float aug[V3_LINUCB_DIM][V3_LINUCB_DIM + 1];
    for (int i = 0; i < V3_LINUCB_DIM; i++) {
        for (int j = 0; j < V3_LINUCB_DIM; j++) {
            aug[i][j] = A[i][j];
        }
        aug[i][V3_LINUCB_DIM] = b[i];
    }

    /* Forward elimination with partial pivoting */
    for (int col = 0; col < V3_LINUCB_DIM; col++) {
        /* Find pivot */
        int max_row = col;
        float max_val = fabsf(aug[col][col]);
        for (int row = col + 1; row < V3_LINUCB_DIM; row++) {
            if (fabsf(aug[row][col]) > max_val) {
                max_val = fabsf(aug[row][col]);
                max_row = row;
            }
        }

        if (max_val < 1e-10f) return false; /* Singular */

        /* Swap rows */
        if (max_row != col) {
            for (int j = 0; j <= V3_LINUCB_DIM; j++) {
                float tmp = aug[col][j];
                aug[col][j] = aug[max_row][j];
                aug[max_row][j] = tmp;
            }
        }

        /* Eliminate below */
        for (int row = col + 1; row < V3_LINUCB_DIM; row++) {
            float factor = aug[row][col] / aug[col][col];
            for (int j = col; j <= V3_LINUCB_DIM; j++) {
                aug[row][j] -= factor * aug[col][j];
            }
        }
    }

    /* Back substitution */
    for (int i = V3_LINUCB_DIM - 1; i >= 0; i--) {
        out[i] = aug[i][V3_LINUCB_DIM];
        for (int j = i + 1; j < V3_LINUCB_DIM; j++) {
            out[i] -= aug[i][j] * out[j];
        }
        out[i] /= aug[i][i];
    }

    return true;
}

/**
 * Compute x^T * A^{-1} * x (the uncertainty term).
 * We solve A*z = x, then return x^T * z = dot(x, z).
 */
static float quadratic_form(const float A[V3_LINUCB_DIM][V3_LINUCB_DIM],
                             const float x[V3_LINUCB_DIM]) {
    float z[V3_LINUCB_DIM];
    if (!solve_8x8(A, x, z)) return 1.0f; /* Fallback: high uncertainty */

    float result = 0.0f;
    for (int i = 0; i < V3_LINUCB_DIM; i++) {
        result += x[i] * z[i];
    }
    return result;
}

void v3_linucb_update(v3_linucb_t *model, const v3_feature_t *features,
                       float reward) {
    const float *x = features->x;

    /* A += x * x^T (rank-1 update) */
    for (int i = 0; i < V3_LINUCB_DIM; i++) {
        for (int j = 0; j < V3_LINUCB_DIM; j++) {
            model->A[i][j] += x[i] * x[j];
        }
    }

    /* b += reward * x */
    for (int i = 0; i < V3_LINUCB_DIM; i++) {
        model->b[i] += reward * x[i];
    }

    model->observation_count++;
}

float v3_linucb_predict(const v3_linucb_t *model, const v3_feature_t *features) {
    const float *x = features->x;

    /* theta = A^{-1} * b */
    float theta[V3_LINUCB_DIM];
    if (!solve_8x8(model->A, model->b, theta)) {
        return 0.5f; /* Fallback */
    }

    /* predicted_reward = x^T * theta */
    float pred = 0.0f;
    for (int i = 0; i < V3_LINUCB_DIM; i++) {
        pred += x[i] * theta[i];
    }

    /* UCB bonus = alpha * sqrt(x^T * A^{-1} * x) */
    float uncertainty = quadratic_form(model->A, x);
    if (uncertainty < 0.0f) uncertainty = 0.0f;
    float ucb = model->alpha * sqrtf(uncertainty);

    /* Clamp to [0, 2] — predicted reward + exploration can't be wildly large */
    float score = pred + ucb;
    if (score < 0.0f) score = 0.0f;
    if (score > 2.0f) score = 2.0f;

    return score;
}

/* ============================================================================
 * Feature Vector Construction
 * ========================================================================== */

/* Map vendor OUI prefix to category 0-7 */
static float vendor_category(const char *oui) {
    if (!oui || !oui[0]) return 0.5f;

    /* Hash first 4 chars of OUI to a category */
    uint32_t h = 0;
    for (int i = 0; i < 4 && oui[i]; i++) {
        h = h * 31 + (uint8_t)oui[i];
    }
    return (float)(h % 8) / 7.0f;
}

/* Map encryption string to 0-1 */
static float encryption_score(const char *enc) {
    if (!enc || !enc[0]) return 0.0f;
    if (strstr(enc, "WPA3") || strstr(enc, "SAE")) return 1.0f;
    if (strstr(enc, "WPA2")) return 0.66f;
    if (strstr(enc, "WPA")) return 0.5f;
    if (strstr(enc, "WEP")) return 0.33f;
    return 0.0f; /* OPEN */
}

v3_feature_t v3_build_features(const ts_entity_t *entity,
                                const v3_brain_t *v3,
                                double latitude, double longitude) {
    v3_feature_t f;
    memset(&f, 0, sizeof(f));

    /* [0] Vendor category */
    f.x[0] = vendor_category(entity->vendor_oui);

    /* [1] Encryption type */
    f.x[1] = encryption_score(entity->encryption);

    /* [2] Channel normalized */
    f.x[2] = (float)entity->channel / 165.0f;

    /* [3] RSSI normalized: map [-100, -30] to [0, 1] */
    float rssi_norm = ((float)entity->last_rssi + 100.0f) / 70.0f;
    if (rssi_norm < 0.0f) rssi_norm = 0.0f;
    if (rssi_norm > 1.0f) rssi_norm = 1.0f;
    f.x[3] = rssi_norm;

    /* [4] Time of day */
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    f.x[4] = (float)lt->tm_hour / 24.0f;

    /* [5] Day of week */
    f.x[5] = (float)lt->tm_wday / 7.0f;

    /* [6] Zone prior (spatial) */
    if (latitude != 0.0 || longitude != 0.0) {
        char geohash[V3_GEOHASH_LEN];
        v3_geohash_encode(latitude, longitude, V3_GEOHASH_PRECISION, geohash);
        f.x[6] = v3_zone_prior(&v3->zones, geohash);
    } else {
        f.x[6] = 0.5f; /* Unknown zone */
    }

    /* [7] ESS log: how much data we have */
    float ess = entity->alpha + entity->beta;
    f.x[7] = log2f(ess + 1.0f) / 10.0f;
    if (f.x[7] > 1.0f) f.x[7] = 1.0f;

    return f;
}

/* ============================================================================
 * Windowed (Discounted) Thompson Sampling
 * ========================================================================== */

void v3_windowed_init(v3_windowed_ts_t *wt) {
    wt->alpha_w = 1.0f;
    wt->beta_w = 1.0f;
    wt->gamma = V3_WINDOWED_GAMMA;
}

void v3_windowed_observe(v3_windowed_ts_t *wt, bool success, float weight) {
    if (weight < 0.1f) weight = 0.1f;
    if (weight > 1.0f) weight = 1.0f;

    /* Discount existing observations */
    wt->alpha_w *= wt->gamma;
    wt->beta_w *= wt->gamma;

    /* Add new observation */
    if (success) {
        wt->alpha_w += weight;
    } else {
        wt->beta_w += weight;
    }

    /* ESS floor: if alpha + beta drops too low, it means data decayed away.
     * Inject a small amount to prevent wild swings. */
    float ess = wt->alpha_w + wt->beta_w;
    if (ess < V3_WINDOWED_MIN_ESS) {
        float inject = (V3_WINDOWED_MIN_ESS - ess) / 2.0f;
        wt->alpha_w += inject;
        wt->beta_w += inject;
    }
}

float v3_windowed_sample(const v3_windowed_ts_t *wt) {
    return ts_beta_sample(wt->alpha_w, wt->beta_w);
}

void v3_windowed_inject_prior(v3_windowed_ts_t *wt, float prior_alpha,
                               float prior_beta, float blend) {
    wt->alpha_w = (1.0f - blend) * wt->alpha_w + blend * prior_alpha;
    wt->beta_w = (1.0f - blend) * wt->beta_w + blend * prior_beta;
}

/* ============================================================================
 * Reward Shaping
 * ========================================================================== */

float v3_shaped_reward(v3_reward_level_t level) {
    if (level < 0 || level >= V3_REWARD_LEVELS) return 0.0f;
    return V3_SHAPED_REWARDS[level];
}

bool v3_is_binary_success(v3_reward_level_t level) {
    return level >= V3_BINARY_THRESHOLD;
}

/* ============================================================================
 * Geohash Encoding
 * ========================================================================== */

static const char GEOHASH_BASE32[] = "0123456789bcdefghjkmnpqrstuvwxyz";

void v3_geohash_encode(double lat, double lon, int precision, char *out) {
    double lat_range[2] = {-90.0, 90.0};
    double lon_range[2] = {-180.0, 180.0};

    int bits[] = {16, 8, 4, 2, 1};
    int bit = 0;
    int ch = 0;
    int idx = 0;
    bool even = true;

    while (idx < precision) {
        if (even) {
            double mid = (lon_range[0] + lon_range[1]) / 2.0;
            if (lon > mid) {
                ch |= bits[bit];
                lon_range[0] = mid;
            } else {
                lon_range[1] = mid;
            }
        } else {
            double mid = (lat_range[0] + lat_range[1]) / 2.0;
            if (lat > mid) {
                ch |= bits[bit];
                lat_range[0] = mid;
            } else {
                lat_range[1] = mid;
            }
        }
        even = !even;

        if (bit < 4) {
            bit++;
        } else {
            out[idx++] = GEOHASH_BASE32[ch];
            bit = 0;
            ch = 0;
        }
    }
    out[idx] = '\0';
}

/* ============================================================================
 * Spatial Zone Cache (LRU)
 * ========================================================================== */

v3_zone_entry_t *v3_zone_lookup(v3_zone_cache_t *cache, const char *geohash) {
    for (int i = 0; i < cache->count; i++) {
        if (strcmp(cache->zones[i].geohash, geohash) == 0) {
            cache->zones[i].last_used = time(NULL);
            return &cache->zones[i];
        }
    }
    return NULL;
}

/* Evict least-recently-used zone entry */
static int zone_evict_lru(v3_zone_cache_t *cache) {
    if (cache->count == 0) return 0;

    int oldest = 0;
    for (int i = 1; i < cache->count; i++) {
        if (cache->zones[i].last_used < cache->zones[oldest].last_used) {
            oldest = i;
        }
    }
    return oldest;
}

void v3_zone_update(v3_zone_cache_t *cache, const char *geohash,
                     bool success, float weight) {
    v3_zone_entry_t *entry = v3_zone_lookup(cache, geohash);

    if (!entry) {
        /* Create new entry */
        int slot;
        if (cache->count < V3_ZONE_CACHE_SIZE) {
            slot = cache->count++;
        } else {
            slot = zone_evict_lru(cache);
        }
        entry = &cache->zones[slot];
        strncpy(entry->geohash, geohash, V3_GEOHASH_LEN - 1);
        entry->geohash[V3_GEOHASH_LEN - 1] = '\0';
        entry->alpha = 1.0f;
        entry->beta = 1.0f;
        entry->last_used = time(NULL);
    }

    if (success) {
        entry->alpha += weight;
    } else {
        entry->beta += weight;
    }
}

float v3_zone_prior(const v3_zone_cache_t *cache, const char *geohash) {
    /* Look for exact match first */
    for (int i = 0; i < cache->count; i++) {
        if (strcmp(cache->zones[i].geohash, geohash) == 0) {
            float ess = cache->zones[i].alpha + cache->zones[i].beta;
            if (ess > 2.0f) {
                return cache->zones[i].alpha / ess;
            }
        }
    }

    /* Fallback: parent zone (precision-5, ~5km) */
    if (strlen(geohash) >= V3_GEOHASH_PRECISION) {
        char parent[V3_GEOHASH_LEN];
        strncpy(parent, geohash, V3_ZONE_PARENT_PREC);
        parent[V3_ZONE_PARENT_PREC] = '\0';

        for (int i = 0; i < cache->count; i++) {
            if (strncmp(cache->zones[i].geohash, parent, V3_ZONE_PARENT_PREC) == 0) {
                float ess = cache->zones[i].alpha + cache->zones[i].beta;
                if (ess > 2.0f) {
                    return cache->zones[i].alpha / ess;
                }
            }
        }
    }

    return 0.5f; /* No data — neutral prior */
}

/* ============================================================================
 * Temporal Context
 * ========================================================================== */

void v3_temporal_observe(v3_temporal_stats_t *stats, int hour, bool success) {
    int slot = v3_temporal_slot(hour);
    stats->total[slot]++;
    if (success) {
        stats->success[slot]++;
    }
}

float v3_temporal_rate(const v3_temporal_stats_t *stats, int hour) {
    int slot = v3_temporal_slot(hour);
    if (stats->total[slot] < 3) return 0.5f; /* Not enough data */
    return (float)stats->success[slot] / (float)stats->total[slot];
}

/* ============================================================================
 * Hierarchical Bayesian Priors
 * ========================================================================== */

void v3_hierarchy_init(v3_hierarchy_t *h) {
    h->pop_alpha = 1.0f;
    h->pop_beta = 1.0f;
    h->cluster_count = 0;
    memset(h->clusters, 0, sizeof(h->clusters));
}

void v3_hierarchy_build_cluster_key(const ts_entity_t *entity, char *out,
                                     int out_len) {
    /* Key format: "vendor4_encryption_band" */
    char vendor4[5] = {0};
    if (entity->vendor_oui[0]) {
        strncpy(vendor4, entity->vendor_oui, 4);
    } else {
        strcpy(vendor4, "unkn");
    }

    const char *enc = "unkn";
    if (entity->encryption[0]) {
        if (strstr(entity->encryption, "WPA3")) enc = "wpa3";
        else if (strstr(entity->encryption, "WPA2")) enc = "wpa2";
        else if (strstr(entity->encryption, "WPA")) enc = "wpa";
        else if (strstr(entity->encryption, "WEP")) enc = "wep";
        else if (strstr(entity->encryption, "OPEN")) enc = "open";
    }

    const char *band = (entity->channel <= 14) ? "2g" : "5g";

    snprintf(out, out_len, "%s_%s_%s", vendor4, enc, band);
}

v3_cluster_prior_t *v3_hierarchy_get_cluster(v3_hierarchy_t *h,
                                              const char *key) {
    for (int i = 0; i < h->cluster_count; i++) {
        if (strcmp(h->clusters[i].key, key) == 0) {
            return &h->clusters[i];
        }
    }

    /* Create new if space */
    if (h->cluster_count < V3_MAX_CLUSTERS) {
        v3_cluster_prior_t *c = &h->clusters[h->cluster_count++];
        strncpy(c->key, key, V3_CLUSTER_KEY_LEN - 1);
        c->alpha = 1.0f;
        c->beta = 1.0f;
        return c;
    }

    return NULL; /* Full */
}

void v3_hierarchy_update_population(v3_hierarchy_t *h, float alpha, float beta) {
    /* Online update: blend new observation into population */
    h->pop_alpha += (alpha - 1.0f) * 0.01f; /* Slow update */
    h->pop_beta += (beta - 1.0f) * 0.01f;

    /* Cap ESS */
    float ess = h->pop_alpha + h->pop_beta;
    if (ess > V3_POP_MAX_ESS) {
        float scale = V3_POP_MAX_ESS / ess;
        h->pop_alpha *= scale;
        h->pop_beta *= scale;
    }
}

void v3_hierarchy_update_cluster(v3_hierarchy_t *h, const char *key,
                                  float alpha, float beta) {
    v3_cluster_prior_t *c = v3_hierarchy_get_cluster(h, key);
    if (!c) return;

    c->alpha += (alpha - 1.0f) * 0.05f; /* Moderate update */
    c->beta += (beta - 1.0f) * 0.05f;

    /* Cap ESS */
    float ess = c->alpha + c->beta;
    if (ess > V3_CLUSTER_MAX_ESS) {
        float scale = V3_CLUSTER_MAX_ESS / ess;
        c->alpha *= scale;
        c->beta *= scale;
    }
}

void v3_hierarchy_get_prior(const v3_hierarchy_t *h, const ts_entity_t *entity,
                             float *out_alpha, float *out_beta) {
    float entity_ess = entity->alpha + entity->beta;

    /* ESS-based blending:
     * < 5 ESS  → use population/cluster prior
     * 5-20 ESS → blend
     * > 20 ESS → trust entity data */

    if (entity_ess >= V3_HIERARCHY_ESS_MID) {
        /* Trust entity's own data */
        *out_alpha = entity->alpha;
        *out_beta = entity->beta;
        return;
    }

    /* Find cluster prior */
    char key[V3_CLUSTER_KEY_LEN];
    v3_hierarchy_build_cluster_key(entity, key, sizeof(key));

    float prior_alpha = h->pop_alpha;
    float prior_beta = h->pop_beta;

    for (int i = 0; i < h->cluster_count; i++) {
        if (strcmp(h->clusters[i].key, key) == 0) {
            float c_ess = h->clusters[i].alpha + h->clusters[i].beta;
            if (c_ess > 2.0f) {
                prior_alpha = h->clusters[i].alpha;
                prior_beta = h->clusters[i].beta;
            }
            break;
        }
    }

    if (entity_ess < V3_HIERARCHY_ESS_LOW) {
        /* Low data: use hierarchy almost entirely */
        float blend = 0.8f;
        *out_alpha = blend * prior_alpha + (1.0f - blend) * entity->alpha;
        *out_beta = blend * prior_beta + (1.0f - blend) * entity->beta;
    } else {
        /* Blending zone: linear interpolation */
        float t = (entity_ess - V3_HIERARCHY_ESS_LOW) /
                  (V3_HIERARCHY_ESS_MID - V3_HIERARCHY_ESS_LOW);
        float blend = 1.0f - t; /* 1.0 at ESS=5, 0.0 at ESS=20 */
        blend *= 0.5f;          /* Max 50% hierarchy influence */
        *out_alpha = (1.0f - blend) * entity->alpha + blend * prior_alpha;
        *out_beta = (1.0f - blend) * entity->beta + blend * prior_beta;
    }
}

/* ============================================================================
 * Predictive Attack Scheduler
 * ========================================================================== */

/* Estimate contact time based on mobility mode and RSSI trend */
static float estimate_contact_time(int mobility_mode, int rssi,
                                    float rssi_trend) {
    float base;

    switch (mobility_mode) {
    case 0: /* stationary */
        base = 300.0f; /* 5 minutes */
        break;
    case 1: /* walking */
        base = 60.0f;  /* 1 minute */
        break;
    case 2: /* driving */
        base = 15.0f;  /* 15 seconds */
        break;
    default:
        base = 120.0f;
        break;
    }

    /* RSSI trend adjustment: falling RSSI means moving away */
    if (rssi_trend < -2.0f) {
        base *= 0.5f; /* Moving away fast */
    } else if (rssi_trend < -0.5f) {
        base *= 0.75f;
    } else if (rssi_trend > 2.0f) {
        base *= 1.5f; /* Approaching */
    }

    /* Strong signal = probably close = more time */
    if (rssi > -50) base *= 1.3f;
    else if (rssi < -75) base *= 0.6f;

    return base;
}

/* Estimate attack time based on phase */
static float estimate_attack_time(int phase) {
    switch (phase) {
    case 0: return 3.0f;   /* PMKID */
    case 1: return 2.0f;   /* CSA */
    case 2: return 5.0f;   /* Deauth */
    case 3: return 8.0f;   /* PMF bypass */
    case 4: return 4.0f;   /* Disassoc */
    case 5: return 10.0f;  /* Rogue M2 */
    case 6: return 2.0f;   /* Probe */
    case 7: return 15.0f;  /* Passive */
    default: return 5.0f;
    }
}

void v3_schedule_build(v3_brain_t *v3, ts_brain_t *ts,
                        double lat, double lon, int mobility_mode) {
    v3_attack_schedule_t *sched = &v3->schedule;
    sched->target_count = 0;
    sched->horizon_sec = (float)V3_SCHED_HORIZON_SEC;
    sched->generated_at = time(NULL);

    /* Score all active entities */
    typedef struct { int idx; float score; } scored_t;
    scored_t scored[TS_MAX_ENTITIES];
    int n_scored = 0;

    for (int i = 0; i < TS_MAX_ENTITIES; i++) {
        ts_entity_t *e = &ts->entities[i];
        if (!e->in_use || e->status == ENTITY_FLAGGED ||
            e->status == ENTITY_ARCHIVED) continue;

        v3_feature_t features = v3_build_features(e, v3, lat, lon);
        float linucb_score = v3_linucb_predict(&v3->linucb, &features);

        scored[n_scored].idx = i;
        scored[n_scored].score = linucb_score;
        n_scored++;
    }

    /* Sort by score (descending) — simple insertion sort for small N */
    for (int i = 1; i < n_scored; i++) {
        scored_t key = scored[i];
        int j = i - 1;
        while (j >= 0 && scored[j].score < key.score) {
            scored[j + 1] = scored[j];
            j--;
        }
        scored[j + 1] = key;
    }

    /* Greedy packing: fit as many attacks into the horizon as possible */
    float time_remaining = sched->horizon_sec;
    float channel_switch_cost = 0.5f; /* 500ms channel switch */
    int last_channel = -1;

    for (int i = 0; i < n_scored && sched->target_count < V3_SCHED_MAX_TARGETS; i++) {
        int idx = scored[i].idx;
        ts_entity_t *e = &ts->entities[idx];

        /* Estimate contact time */
        float contact = estimate_contact_time(
            mobility_mode, e->last_rssi, 0.0f /* TODO: wire RSSI trend */);

        /* Find best attack phase for this entity */
        int best_phase = 0;
        float best_phase_score = -1.0f;
        for (int ph = 0; ph < 8; ph++) {
            /* Use per-AP attack phase Thompson from brain_attack_tracker */
            float phase_time = estimate_attack_time(ph);
            if (phase_time < contact && phase_time < time_remaining) {
                /* Score by success rate / time */
                float ps = v3_randf(); /* Placeholder: should sample from per-AP phase Thompson */
                if (ps > best_phase_score) {
                    best_phase_score = ps;
                    best_phase = ph;
                }
            }
        }

        float attack_time = estimate_attack_time(best_phase);

        /* Channel switch penalty */
        if (last_channel != -1 && e->channel != last_channel) {
            attack_time += channel_switch_cost;
        }

        if (attack_time > time_remaining) continue;
        if (attack_time > contact) continue;

        /* Add to schedule */
        v3_sched_target_t *t = &sched->targets[sched->target_count++];
        strncpy(t->entity_id, e->entity_id, TS_MAC_STR_LEN - 1);
        t->expected_reward = scored[i].score;
        t->contact_time_sec = contact;
        t->attack_time_sec = attack_time;
        t->preferred_phase = best_phase;

        time_remaining -= attack_time;
        last_channel = e->channel;
    }

    v3->schedule_runs++;
    v3->last_schedule_time = sched->generated_at;
}

/* ============================================================================
 * Combined v3.0 Scoring
 * ========================================================================== */

float v3_score_entity(v3_brain_t *v3, ts_brain_t *ts,
                       ts_entity_t *entity, int entity_idx,
                       const ts_action_t *action,
                       double lat, double lon) {
    /* 1. Base score from existing Thompson + cost */
    float thompson_score = ts_score_entity(ts, entity, action);

    /* 2. LinUCB contextual score */
    v3_feature_t features = v3_build_features(entity, v3, lat, lon);
    float linucb_score = v3_linucb_predict(&v3->linucb, &features);

    /* 3. Windowed Thompson sample (recent behavior) */
    float windowed_sample = 0.5f;
    if (entity_idx >= 0 && entity_idx < TS_MAX_ENTITIES) {
        windowed_sample = v3_windowed_sample(&v3->windowed[entity_idx]);
    }

    /* 4. Hierarchical prior for cold-start entities */
    float hier_alpha, hier_beta;
    v3_hierarchy_get_prior(&v3->hierarchy, entity, &hier_alpha, &hier_beta);
    float hier_rate = hier_alpha / (hier_alpha + hier_beta);

    /* 5. Temporal boost */
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    float temporal_rate = v3_temporal_rate(&v3->temporal, lt->tm_hour);

    /* Combine: weighted ensemble
     * Thompson:   40%  (proven baseline)
     * LinUCB:     25%  (contextual generalization)
     * Windowed:   20%  (recent behavior)
     * Hierarchy:  10%  (cold-start help)
     * Temporal:    5%  (time-of-day signal)
     */
    float combined = 0.40f * thompson_score
                   + 0.25f * linucb_score
                   + 0.20f * windowed_sample
                   + 0.10f * hier_rate
                   + 0.05f * temporal_rate;

    return combined;
}

/* ============================================================================
 * Full v3.0 Observation Update
 * ========================================================================== */

void v3_observe(v3_brain_t *v3, ts_brain_t *ts,
                 ts_entity_t *entity, int entity_idx,
                 v3_reward_level_t reward_level,
                 double lat, double lon) {
    float shaped = v3_shaped_reward(reward_level);
    bool binary_success = v3_is_binary_success(reward_level);
    float robustness = 1.0f; /* Could come from signal tracker */

    /* 1. Update standard Binary Thompson (unchanged from v2) */
    ts_observe_outcome(entity, binary_success, robustness);

    /* 2. Update Windowed Thompson */
    if (entity_idx >= 0 && entity_idx < TS_MAX_ENTITIES) {
        v3_windowed_observe(&v3->windowed[entity_idx], binary_success, robustness);
    }

    /* 3. Update LinUCB with shaped reward */
    v3_feature_t features = v3_build_features(entity, v3, lat, lon);
    v3_linucb_update(&v3->linucb, &features, shaped);

    /* 4. Update reward level counts */
    if (entity_idx >= 0 && entity_idx < TS_MAX_ENTITIES &&
        reward_level < V3_REWARD_LEVELS) {
        v3->reward_counts[entity_idx][reward_level]++;
    }

    /* 5. Update spatial zone */
    if (lat != 0.0 || lon != 0.0) {
        char geohash[V3_GEOHASH_LEN];
        v3_geohash_encode(lat, lon, V3_GEOHASH_PRECISION, geohash);
        v3_zone_update(&v3->zones, geohash, binary_success, robustness);
    }

    /* 6. Update temporal stats (global + per-entity) */
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    v3_temporal_observe(&v3->temporal, lt->tm_hour, binary_success);
    if (entity_idx >= 0 && entity_idx < TS_MAX_ENTITIES) {
        v3_temporal_observe(&v3->entity_temporal[entity_idx],
                             lt->tm_hour, binary_success);
    }

    /* 7. Update hierarchy (population + cluster) */
    v3_hierarchy_update_population(&v3->hierarchy, entity->alpha, entity->beta);

    char cluster_key[V3_CLUSTER_KEY_LEN];
    v3_hierarchy_build_cluster_key(entity, cluster_key, sizeof(cluster_key));
    v3_hierarchy_update_cluster(&v3->hierarchy, cluster_key,
                                 entity->alpha, entity->beta);

    /* 8. Inject hierarchical prior into windowed Thompson for cold-start */
    float ess = entity->alpha + entity->beta;
    if (ess < V3_HIERARCHY_ESS_LOW && entity_idx >= 0) {
        float ha, hb;
        v3_hierarchy_get_prior(&v3->hierarchy, entity, &ha, &hb);
        v3_windowed_inject_prior(&v3->windowed[entity_idx], ha, hb, 0.3f);
    }

    v3->linucb_updates++;
}

/* ============================================================================
 * Federation Export/Import
 * ========================================================================== */

void v3_federation_export(const v3_brain_t *v3, const ts_brain_t *ts,
                           v3_federation_export_t *out) {
    memset(out, 0, sizeof(v3_federation_export_t));

    /* LinUCB model */
    memcpy(out->linucb_A, v3->linucb.A, sizeof(out->linucb_A));
    memcpy(out->linucb_b, v3->linucb.b, sizeof(out->linucb_b));

    /* Population prior */
    out->pop_alpha = v3->hierarchy.pop_alpha;
    out->pop_beta = v3->hierarchy.pop_beta;

    /* Cluster priors (already anonymized — keys are feature-based, not MAC-based) */
    out->cluster_count = v3->hierarchy.cluster_count;
    memcpy(out->clusters, v3->hierarchy.clusters,
           v3->hierarchy.cluster_count * sizeof(v3_cluster_prior_t));

    /* Zone priors (geohash only, no raw GPS) */
    out->zone_count = v3->zones.count;
    memcpy(out->zones, v3->zones.zones,
           v3->zones.count * sizeof(v3_zone_entry_t));

    /* Stats */
    out->total_observations = v3->linucb.observation_count;
    out->entity_count = ts->entity_count;

    /* Anonymized device ID (hash of pointer address + start time) */
    uint32_t raw = (uint32_t)((uintptr_t)v3 ^ (uint32_t)ts->started_at);
    raw ^= raw >> 16;
    raw *= 0x45d9f3b;
    raw ^= raw >> 16;
    out->device_id_hash = raw;
}

void v3_federation_import(v3_brain_t *v3, const v3_federation_export_t *fleet,
                           float blend_factor) {
    /* Very conservative blending: default 10% fleet, 90% local */
    float b = blend_factor;
    if (b < 0.0f) b = 0.0f;
    if (b > 0.5f) b = 0.5f; /* Never more than 50% fleet influence */

    /* LinUCB matrices */
    for (int i = 0; i < V3_LINUCB_DIM; i++) {
        for (int j = 0; j < V3_LINUCB_DIM; j++) {
            v3->linucb.A[i][j] = (1.0f - b) * v3->linucb.A[i][j] +
                                  b * fleet->linucb_A[i][j];
        }
        v3->linucb.b[i] = (1.0f - b) * v3->linucb.b[i] +
                            b * fleet->linucb_b[i];
    }

    /* Population prior */
    v3->hierarchy.pop_alpha = (1.0f - b) * v3->hierarchy.pop_alpha +
                               b * fleet->pop_alpha;
    v3->hierarchy.pop_beta = (1.0f - b) * v3->hierarchy.pop_beta +
                              b * fleet->pop_beta;

    /* Merge cluster priors */
    for (int i = 0; i < fleet->cluster_count; i++) {
        v3_cluster_prior_t *c = v3_hierarchy_get_cluster(
            &v3->hierarchy, fleet->clusters[i].key);
        if (c) {
            c->alpha = (1.0f - b) * c->alpha + b * fleet->clusters[i].alpha;
            c->beta = (1.0f - b) * c->beta + b * fleet->clusters[i].beta;
        }
    }

    /* Merge zone priors */
    for (int i = 0; i < fleet->zone_count; i++) {
        v3_zone_entry_t *z = v3_zone_lookup(&v3->zones,
                                              fleet->zones[i].geohash);
        if (z) {
            z->alpha = (1.0f - b) * z->alpha + b * fleet->zones[i].alpha;
            z->beta = (1.0f - b) * z->beta + b * fleet->zones[i].beta;
        } else {
            /* Insert new zone from fleet */
            v3_zone_update(&v3->zones, fleet->zones[i].geohash, false, 0.0f);
            z = v3_zone_lookup(&v3->zones, fleet->zones[i].geohash);
            if (z) {
                z->alpha = fleet->zones[i].alpha;
                z->beta = fleet->zones[i].beta;
            }
        }
    }
}

/* ============================================================================
 * Persistence
 * ========================================================================== */

#define V3_STATE_MAGIC   0x56335342  /* "V3SB" */
#define V3_STATE_VERSION 1

int v3_save_state(const v3_brain_t *v3, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[thompson_v3] save failed: cannot open %s\n", path);
        return -1;
    }

    uint32_t magic = V3_STATE_MAGIC;
    uint32_t version = V3_STATE_VERSION;
    fwrite(&magic, 4, 1, f);
    fwrite(&version, 4, 1, f);

    /* LinUCB */
    fwrite(&v3->linucb, sizeof(v3_linucb_t), 1, f);

    /* Zone cache */
    fwrite(&v3->zones.count, sizeof(int), 1, f);
    fwrite(v3->zones.zones, sizeof(v3_zone_entry_t), v3->zones.count, f);

    /* Temporal stats */
    fwrite(&v3->temporal, sizeof(v3_temporal_stats_t), 1, f);

    /* Hierarchy */
    fwrite(&v3->hierarchy, sizeof(v3_hierarchy_t), 1, f);

    /* Windowed Thompson */
    fwrite(v3->windowed, sizeof(v3_windowed_ts_t), TS_MAX_ENTITIES, f);

    /* Reward counts */
    fwrite(v3->reward_counts, sizeof(v3->reward_counts), 1, f);

    /* Entity temporal */
    fwrite(v3->entity_temporal, sizeof(v3_temporal_stats_t), TS_MAX_ENTITIES, f);

    /* Stats */
    fwrite(&v3->linucb_updates, sizeof(uint32_t), 1, f);
    fwrite(&v3->schedule_runs, sizeof(uint32_t), 1, f);

    fclose(f);
    fprintf(stderr, "[thompson_v3] saved state to %s (linucb_updates=%u zones=%d clusters=%d)\n",
            path, v3->linucb_updates, v3->zones.count, v3->hierarchy.cluster_count);
    return 0;
}

int v3_load_state(v3_brain_t *v3, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    uint32_t magic, version;
    if (fread(&magic, 4, 1, f) != 1 || fread(&version, 4, 1, f) != 1) {
        fclose(f);
        return -1;
    }

    if (magic != V3_STATE_MAGIC || version != V3_STATE_VERSION) {
        fprintf(stderr, "[thompson_v3] incompatible state file (magic=0x%08x ver=%u)\n",
                magic, version);
        fclose(f);
        return -1;
    }

    /* LinUCB */
    if (fread(&v3->linucb, sizeof(v3_linucb_t), 1, f) != 1) goto corrupt;

    /* Zone cache */
    if (fread(&v3->zones.count, sizeof(int), 1, f) != 1) goto corrupt;
    if (v3->zones.count < 0 || v3->zones.count > V3_ZONE_CACHE_SIZE) goto corrupt;
    if (fread(v3->zones.zones, sizeof(v3_zone_entry_t), v3->zones.count, f)
        != (size_t)v3->zones.count) goto corrupt;

    /* Temporal stats */
    if (fread(&v3->temporal, sizeof(v3_temporal_stats_t), 1, f) != 1) goto corrupt;

    /* Hierarchy */
    if (fread(&v3->hierarchy, sizeof(v3_hierarchy_t), 1, f) != 1) goto corrupt;

    /* Windowed Thompson */
    if (fread(v3->windowed, sizeof(v3_windowed_ts_t), TS_MAX_ENTITIES, f)
        != TS_MAX_ENTITIES) goto corrupt;

    /* Reward counts */
    if (fread(v3->reward_counts, sizeof(v3->reward_counts), 1, f) != 1) goto corrupt;

    /* Entity temporal */
    if (fread(v3->entity_temporal, sizeof(v3_temporal_stats_t), TS_MAX_ENTITIES, f)
        != TS_MAX_ENTITIES) goto corrupt;

    /* Stats */
    if (fread(&v3->linucb_updates, sizeof(uint32_t), 1, f) != 1) goto corrupt;
    if (fread(&v3->schedule_runs, sizeof(uint32_t), 1, f) != 1) goto corrupt;

    fclose(f);
    fprintf(stderr, "[thompson_v3] loaded state from %s (linucb_updates=%u zones=%d clusters=%d)\n",
            path, v3->linucb_updates, v3->zones.count, v3->hierarchy.cluster_count);
    return 0;

corrupt:
    fclose(f);
    fprintf(stderr, "[thompson_v3] corrupt state file: %s\n", path);
    return -1;
}

/* ============================================================================
 * PC Distillation Import (JSON from /tmp/pc_distillation_v3.json)
 * ============================================================================
 * Reads the distillation payload produced by the PC Brain's Distiller class.
 * Imports: hierarchy priors, temporal patterns, zone priors, LinUCB model,
 *          and change-point resets.
 *
 * The JSON is deleted after successful import to prevent re-importing stale data.
 */

static char *read_file_contents(const char *path, long *out_size) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 1048576) { /* 1MB max */
        fclose(f);
        return NULL;
    }

    char *buf = malloc(fsize + 1);
    if (!buf) { fclose(f); return NULL; }

    fread(buf, 1, fsize, f);
    buf[fsize] = '\0';
    fclose(f);

    if (out_size) *out_size = fsize;
    return buf;
}

static void import_hierarchy(v3_brain_t *v3, cJSON *hier) {
    if (!hier) return;

    /* Population prior */
    cJSON *pop = cJSON_GetObjectItem(hier, "population");
    if (pop) {
        cJSON *pa = cJSON_GetObjectItem(pop, "alpha");
        cJSON *pb = cJSON_GetObjectItem(pop, "beta");
        if (pa && cJSON_IsNumber(pa) && pb && cJSON_IsNumber(pb)) {
            /* Blend 30% PC, 70% local */
            v3->hierarchy.pop_alpha = 0.7f * v3->hierarchy.pop_alpha +
                                      0.3f * (float)pa->valuedouble;
            v3->hierarchy.pop_beta = 0.7f * v3->hierarchy.pop_beta +
                                     0.3f * (float)pb->valuedouble;
        }
    }

    /* Cluster priors */
    cJSON *clusters = cJSON_GetObjectItem(hier, "cluster_priors");
    if (clusters && cJSON_IsObject(clusters)) {
        cJSON *item;
        cJSON_ArrayForEach(item, clusters) {
            if (!item->string) continue;
            cJSON *ca = cJSON_GetObjectItem(item, "alpha");
            cJSON *cb = cJSON_GetObjectItem(item, "beta");
            if (!ca || !cb || !cJSON_IsNumber(ca) || !cJSON_IsNumber(cb)) continue;

            v3_cluster_prior_t *c = v3_hierarchy_get_cluster(
                &v3->hierarchy, item->string);
            if (c) {
                c->alpha = 0.7f * c->alpha + 0.3f * (float)ca->valuedouble;
                c->beta = 0.7f * c->beta + 0.3f * (float)cb->valuedouble;
            }
        }
    }

    /* Entity priors */
    cJSON *entities = cJSON_GetObjectItem(hier, "entity_priors");
    if (entities && cJSON_IsObject(entities)) {
        cJSON *item;
        cJSON_ArrayForEach(item, entities) {
            /* Entity priors are informational — we don't directly set per-entity
             * data here since the Pi manages entities dynamically. The hierarchy
             * priors above will influence cold-start entities via
             * v3_hierarchy_get_prior(). */
            (void)item; /* Intentionally unused */
        }
    }
}

static void import_temporal(v3_brain_t *v3, cJSON *temporal) {
    if (!temporal) return;

    /* Global temporal pattern */
    cJSON *global = cJSON_GetObjectItem(temporal, "global");
    if (global && cJSON_IsObject(global)) {
        /* Slot names: night, morning, afternoon, evening */
        const char *slot_names[] = {"night", "morning", "afternoon", "evening"};
        for (int s = 0; s < V3_TEMPORAL_SLOTS; s++) {
            cJSON *rate = cJSON_GetObjectItem(global, slot_names[s]);
            if (rate && cJSON_IsNumber(rate)) {
                /* Inject PC-computed temporal data if local data is sparse */
                if (v3->temporal.total[s] < 10) {
                    /* Synthesize observations from PC rate */
                    float pc_rate = (float)rate->valuedouble;
                    uint32_t synth_total = 20;
                    uint32_t synth_success = (uint32_t)(pc_rate * synth_total);
                    /* Blend: 50% synthetic, 50% local */
                    v3->temporal.success[s] = (v3->temporal.success[s] +
                                               synth_success) / 2;
                    v3->temporal.total[s] = (v3->temporal.total[s] +
                                              synth_total) / 2;
                }
            }
        }
    }
}

static void import_zones(v3_brain_t *v3, cJSON *zones) {
    if (!zones || !cJSON_IsObject(zones)) return;

    cJSON *item;
    cJSON_ArrayForEach(item, zones) {
        if (!item->string) continue;
        cJSON *za = cJSON_GetObjectItem(item, "alpha");
        cJSON *zb = cJSON_GetObjectItem(item, "beta");
        if (!za || !zb || !cJSON_IsNumber(za) || !cJSON_IsNumber(zb)) continue;

        v3_zone_entry_t *z = v3_zone_lookup(&v3->zones, item->string);
        if (z) {
            /* Blend with existing */
            z->alpha = 0.7f * z->alpha + 0.3f * (float)za->valuedouble;
            z->beta = 0.7f * z->beta + 0.3f * (float)zb->valuedouble;
        } else {
            /* Insert new zone from PC analysis */
            v3_zone_update(&v3->zones, item->string, false, 0.0f);
            z = v3_zone_lookup(&v3->zones, item->string);
            if (z) {
                z->alpha = (float)za->valuedouble;
                z->beta = (float)zb->valuedouble;
            }
        }
    }
}

static void import_linucb(v3_brain_t *v3, cJSON *linucb) {
    if (!linucb) return;

    cJSON *A_arr = cJSON_GetObjectItem(linucb, "A");
    cJSON *b_arr = cJSON_GetObjectItem(linucb, "b");

    if (A_arr && cJSON_IsArray(A_arr)) {
        int size = cJSON_GetArraySize(A_arr);
        if (size == V3_LINUCB_DIM * V3_LINUCB_DIM) {
            for (int i = 0; i < V3_LINUCB_DIM; i++) {
                for (int j = 0; j < V3_LINUCB_DIM; j++) {
                    cJSON *val = cJSON_GetArrayItem(A_arr,
                                                     i * V3_LINUCB_DIM + j);
                    if (val && cJSON_IsNumber(val)) {
                        v3->linucb.A[i][j] = 0.7f * v3->linucb.A[i][j] +
                                              0.3f * (float)val->valuedouble;
                    }
                }
            }
        }
    }

    if (b_arr && cJSON_IsArray(b_arr)) {
        int size = cJSON_GetArraySize(b_arr);
        if (size == V3_LINUCB_DIM) {
            for (int i = 0; i < V3_LINUCB_DIM; i++) {
                cJSON *val = cJSON_GetArrayItem(b_arr, i);
                if (val && cJSON_IsNumber(val)) {
                    v3->linucb.b[i] = 0.7f * v3->linucb.b[i] +
                                      0.3f * (float)val->valuedouble;
                }
            }
        }
    }
}

static void import_change_points(v3_brain_t *v3, ts_brain_t *ts,
                                 cJSON *changes) {
    if (!changes || !cJSON_IsObject(changes) || !ts) return;

    /* Change points indicate entities whose behavior has shifted.
     * Reset their windowed Thompson to allow re-adaptation. */
    int reset_count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, changes) {
        if (!item->string) continue;
        /* item->string is the entity_id (MAC) */
        ts_entity_t *entity = ts_find_entity(ts, item->string);
        if (!entity) continue;

        int idx = (int)(entity - ts->entities);
        if (idx < 0 || idx >= TS_MAX_ENTITIES) continue;

        /* Reset windowed Thompson for this entity — fresh start */
        v3_windowed_init(&v3->windowed[idx]);

        /* Reset reward counts so stale data doesn't bias */
        memset(v3->reward_counts[idx], 0, sizeof(v3->reward_counts[idx]));

        reset_count++;
        fprintf(stderr, "[thompson_v3] change-point reset: %s (idx=%d)\n",
                item->string, idx);
    }

    if (reset_count > 0) {
        fprintf(stderr, "[thompson_v3] distillation reset %d/%d flagged entities\n",
                reset_count, cJSON_GetArraySize(changes));
    }
}

int v3_distillation_import(v3_brain_t *v3, ts_brain_t *ts,
                           const char *json_path) {
    if (!v3 || !json_path) return -1;

    long fsize = 0;
    char *json_str = read_file_contents(json_path, &fsize);
    if (!json_str) return -1;

    cJSON *root = cJSON_Parse(json_str);
    free(json_str);

    if (!root) {
        fprintf(stderr, "[thompson_v3] distillation JSON parse error: %s\n",
                json_path);
        return -1;
    }

    /* Verify version */
    cJSON *ver = cJSON_GetObjectItem(root, "version");
    if (!ver || !cJSON_IsString(ver) ||
        strncmp(ver->valuestring, "3.", 2) != 0) {
        fprintf(stderr, "[thompson_v3] distillation version mismatch: %s\n",
                ver ? ver->valuestring : "null");
        cJSON_Delete(root);
        return -1;
    }

    /* Import each component */
    import_hierarchy(v3, cJSON_GetObjectItem(root, "hierarchy"));
    import_temporal(v3, cJSON_GetObjectItem(root, "temporal"));
    import_zones(v3, cJSON_GetObjectItem(root, "zone_priors"));
    import_linucb(v3, cJSON_GetObjectItem(root, "linucb"));
    import_change_points(v3, ts, cJSON_GetObjectItem(root, "change_points"));

    /* Risk scores and clustering are informational (used by PC brain);
     * the hierarchy priors already incorporate their effects */

    cJSON *ts_json = cJSON_GetObjectItem(root, "timestamp");
    fprintf(stderr, "[thompson_v3] imported PC distillation (v%s, ts=%s)\n",
            ver->valuestring,
            (ts_json && cJSON_IsString(ts_json)) ? ts_json->valuestring : "unknown");

    cJSON_Delete(root);

    /* Remove the file to prevent re-importing stale data */
    remove(json_path);

    return 0;
}

/* ============================================================================
 * v3 State Export (JSON for PC to pull)
 * ============================================================================
 * Produces a JSON file at the given path that the PC Brain's pi_sync
 * can SCP to get the current v3 state for distillation input.
 */

int v3_state_export_json(const v3_brain_t *v3, const ts_brain_t *ts,
                          const char *json_path) {
    if (!v3 || !ts || !json_path) return -1;

    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;

    cJSON_AddStringToObject(root, "version", "3.0");

    /* Timestamp */
    char ts_buf[32];
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%S", lt);
    cJSON_AddStringToObject(root, "timestamp", ts_buf);

    /* Entity observations summary */
    cJSON *entities = cJSON_CreateArray();
    for (int i = 0; i < TS_MAX_ENTITIES; i++) {
        const ts_entity_t *e = &ts->entities[i];
        if (!e->in_use) continue;

        cJSON *ent = cJSON_CreateObject();
        cJSON_AddStringToObject(ent, "id", e->entity_id);
        cJSON_AddNumberToObject(ent, "alpha", e->alpha);
        cJSON_AddNumberToObject(ent, "beta", e->beta);
        cJSON_AddNumberToObject(ent, "channel", e->channel);
        cJSON_AddNumberToObject(ent, "last_rssi", e->last_rssi);

        if (e->vendor_oui[0])
            cJSON_AddStringToObject(ent, "vendor", e->vendor_oui);
        if (e->encryption[0])
            cJSON_AddStringToObject(ent, "encryption", e->encryption);

        /* Windowed Thompson state */
        cJSON_AddNumberToObject(ent, "w_alpha", v3->windowed[i].alpha_w);
        cJSON_AddNumberToObject(ent, "w_beta", v3->windowed[i].beta_w);

        /* Reward level counts */
        cJSON *rewards = cJSON_CreateArray();
        for (int r = 0; r < V3_REWARD_LEVELS; r++) {
            cJSON_AddItemToArray(rewards,
                cJSON_CreateNumber(v3->reward_counts[i][r]));
        }
        cJSON_AddItemToObject(ent, "reward_counts", rewards);

        /* Per-entity temporal */
        cJSON *temp = cJSON_CreateObject();
        for (int s = 0; s < V3_TEMPORAL_SLOTS; s++) {
            char slot_key[12];
            snprintf(slot_key, sizeof(slot_key), "s%d_ok", s);
            cJSON_AddNumberToObject(temp, slot_key,
                                     v3->entity_temporal[i].success[s]);
            snprintf(slot_key, sizeof(slot_key), "s%d_n", s);
            cJSON_AddNumberToObject(temp, slot_key,
                                     v3->entity_temporal[i].total[s]);
        }
        cJSON_AddItemToObject(ent, "temporal", temp);

        cJSON_AddItemToArray(entities, ent);
    }
    cJSON_AddItemToObject(root, "entities", entities);

    /* Observations: recent interaction log */
    cJSON *obs = cJSON_CreateArray();
    /* We don't have a raw observation log in memory — the entity summary
     * above captures the same information in aggregated form. The PC Brain
     * can reconstruct per-entity observations from alpha/beta/reward_counts. */
    cJSON_AddItemToObject(root, "observations", obs);

    /* LinUCB model */
    cJSON *linucb = cJSON_CreateObject();
    cJSON *A_arr = cJSON_CreateArray();
    for (int i = 0; i < V3_LINUCB_DIM; i++) {
        for (int j = 0; j < V3_LINUCB_DIM; j++) {
            cJSON_AddItemToArray(A_arr,
                cJSON_CreateNumber(v3->linucb.A[i][j]));
        }
    }
    cJSON_AddItemToObject(linucb, "A", A_arr);
    cJSON *b_arr = cJSON_CreateArray();
    for (int i = 0; i < V3_LINUCB_DIM; i++) {
        cJSON_AddItemToArray(b_arr, cJSON_CreateNumber(v3->linucb.b[i]));
    }
    cJSON_AddItemToObject(linucb, "b", b_arr);
    cJSON_AddNumberToObject(linucb, "observation_count",
                             v3->linucb.observation_count);
    cJSON_AddItemToObject(root, "linucb", linucb);

    /* Hierarchy */
    cJSON *hier = cJSON_CreateObject();
    cJSON_AddNumberToObject(hier, "pop_alpha", v3->hierarchy.pop_alpha);
    cJSON_AddNumberToObject(hier, "pop_beta", v3->hierarchy.pop_beta);
    cJSON *cl_obj = cJSON_CreateObject();
    for (int i = 0; i < v3->hierarchy.cluster_count; i++) {
        cJSON *cp = cJSON_CreateObject();
        cJSON_AddNumberToObject(cp, "alpha", v3->hierarchy.clusters[i].alpha);
        cJSON_AddNumberToObject(cp, "beta", v3->hierarchy.clusters[i].beta);
        cJSON_AddItemToObject(cl_obj, v3->hierarchy.clusters[i].key, cp);
    }
    cJSON_AddItemToObject(hier, "clusters", cl_obj);
    cJSON_AddItemToObject(root, "hierarchy", hier);

    /* Zone priors */
    cJSON *zone_obj = cJSON_CreateObject();
    for (int i = 0; i < v3->zones.count; i++) {
        cJSON *zp = cJSON_CreateObject();
        cJSON_AddNumberToObject(zp, "alpha", v3->zones.zones[i].alpha);
        cJSON_AddNumberToObject(zp, "beta", v3->zones.zones[i].beta);
        cJSON_AddItemToObject(zone_obj, v3->zones.zones[i].geohash, zp);
    }
    cJSON_AddItemToObject(root, "zone_priors", zone_obj);

    /* Global temporal */
    cJSON *gt = cJSON_CreateObject();
    const char *slot_names[] = {"night", "morning", "afternoon", "evening"};
    for (int s = 0; s < V3_TEMPORAL_SLOTS; s++) {
        if (v3->temporal.total[s] > 0) {
            cJSON_AddNumberToObject(gt, slot_names[s],
                (double)v3->temporal.success[s] / v3->temporal.total[s]);
        } else {
            cJSON_AddNumberToObject(gt, slot_names[s], 0.5);
        }
    }
    cJSON_AddItemToObject(root, "temporal", gt);

    /* Stats */
    cJSON_AddNumberToObject(root, "linucb_updates", v3->linucb_updates);
    cJSON_AddNumberToObject(root, "schedule_runs", v3->schedule_runs);
    cJSON_AddNumberToObject(root, "entity_count", ts->entity_count);

    /* Federation export data */
    cJSON *fed = cJSON_CreateObject();
    v3_federation_export_t fexp;
    v3_federation_export(v3, ts, &fexp);
    cJSON_AddNumberToObject(fed, "total_observations", fexp.total_observations);
    cJSON_AddNumberToObject(fed, "entity_count", fexp.entity_count);
    cJSON_AddNumberToObject(fed, "device_id_hash", fexp.device_id_hash);
    cJSON_AddItemToObject(root, "federated_export", fed);

    /* Write to file */
    char *output = cJSON_Print(root);
    cJSON_Delete(root);

    if (!output) return -1;

    FILE *f = fopen(json_path, "w");
    if (!f) {
        free(output);
        return -1;
    }

    fputs(output, f);
    fclose(f);
    free(output);

    fprintf(stderr, "[thompson_v3] exported state to %s (%d entities)\n",
            json_path, ts->entity_count);
    return 0;
}
