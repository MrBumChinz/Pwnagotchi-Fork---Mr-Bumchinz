/**
 * thompson.c - Thompson Sampling Implementation
 *
 * Binary Thompson Sampling for entity selection on Pi Zero W
 * Memory-efficient, no dynamic allocation in hot path
 *
 * Based on PI-BRAIN.md v2.2 architecture
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>

#include "thompson.h"

/* ============================================================================
 * Predefined Actions
 * ========================================================================== */

const ts_action_t TS_ACTION_PROBE = {
    .name = "probe",
    .cost_time = 2.0f,
    .cost_energy = 0.05f,
    .cost_risk = 0.1f
};

const ts_action_t TS_ACTION_PASSIVE_SCAN = {
    .name = "passive_scan",
    .cost_time = 5.0f,
    .cost_energy = 0.02f,
    .cost_risk = 0.01f
};

const ts_action_t TS_ACTION_ASSOCIATE = {
    .name = "associate",
    .cost_time = 3.0f,
    .cost_energy = 0.08f,
    .cost_risk = 0.2f
};

const ts_action_t TS_ACTION_DEAUTH = {
    .name = "deauth",
    .cost_time = 1.0f,
    .cost_energy = 0.03f,
    .cost_risk = 0.3f
};

const ts_action_t TS_ACTION_WAIT = {
    .name = "wait",
    .cost_time = 0.1f,
    .cost_energy = 0.0f,
    .cost_risk = 0.0f
};

/* Mode names */
static const char *mode_names[] = {
    [MODE_PASSIVE_DISCOVERY] = "passive_discovery",
    [MODE_ACTIVE_TARGETING] = "active_targeting",
    [MODE_COOLDOWN] = "cooldown",
    [MODE_SYNC_WINDOW] = "sync_window"
};

/* ============================================================================
 * Random Number Generation
 * ========================================================================== */

/* Simple xorshift64 PRNG - fast and good enough for Thompson Sampling */
static uint64_t rng_state = 0;

static void rng_seed(void) {
    rng_state = (uint64_t)time(NULL) ^ (uint64_t)clock();
    if (rng_state == 0) rng_state = 0xDEADBEEF;
}

static uint64_t rng_next(void) {
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

/* Uniform [0, 1) */
static float rng_uniform(void) {
    return (float)(rng_next() >> 11) / (float)(1ULL << 53);
}

/* Standard normal via Box-Muller */
static float rng_normal(void) {
    float u1 = rng_uniform();
    float u2 = rng_uniform();
    if (u1 < 1e-10f) u1 = 1e-10f;
    return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * 3.14159265f * u2);
}

/**
 * Gamma distribution sampling via Marsaglia and Tsang's method
 * For shape >= 1
 */
static float rng_gamma(float shape) {
    if (shape < 1.0f) {
        /* For shape < 1, use shape+1 then scale */
        float u = rng_uniform();
        return rng_gamma(shape + 1.0f) * powf(u, 1.0f / shape);
    }
    
    float d = shape - 1.0f / 3.0f;
    float c = 1.0f / sqrtf(9.0f * d);
    
    while (1) {
        float x, v;
        do {
            x = rng_normal();
            v = 1.0f + c * x;
        } while (v <= 0.0f);
        
        v = v * v * v;
        float u = rng_uniform();
        
        if (u < 1.0f - 0.0331f * (x * x) * (x * x)) {
            return d * v;
        }
        
        if (logf(u) < 0.5f * x * x + d * (1.0f - v + logf(v))) {
            return d * v;
        }
    }
}

/**
 * Beta distribution via ratio of Gammas
 * Beta(a, b) = Gamma(a) / (Gamma(a) + Gamma(b))
 */
float ts_beta_sample(float alpha, float beta) {
    if (alpha <= 0.0f) alpha = 0.01f;
    if (beta <= 0.0f) beta = 0.01f;
    
    float x = rng_gamma(alpha);
    float y = rng_gamma(beta);
    
    return x / (x + y);
}

/* ============================================================================
 * Signal Tracker (EWMA + MAD)
 * ========================================================================== */

static void signal_tracker_init(ts_signal_tracker_t *tracker) {
    tracker->level = -50.0f;
    tracker->alpha = 0.3f;
    tracker->window_count = 0;
    tracker->window_idx = 0;
    memset(tracker->window, 0, sizeof(tracker->window));
}

float ts_update_signal(ts_entity_t *entity, int8_t rssi) {
    ts_signal_tracker_t *t = &entity->signal;
    
    /* Add to window */
    t->window[t->window_idx] = rssi;
    t->window_idx = (t->window_idx + 1) % TS_MAD_WINDOW_SIZE;
    if (t->window_count < TS_MAD_WINDOW_SIZE) {
        t->window_count++;
    }
    
    /* Median filter on last 3 samples */
    float rssi_filtered = (float)rssi;
    if (t->window_count >= 3) {
        int8_t recent[3];
        int idx = (t->window_idx - 1 + TS_MAD_WINDOW_SIZE) % TS_MAD_WINDOW_SIZE;
        recent[0] = t->window[idx];
        idx = (idx - 1 + TS_MAD_WINDOW_SIZE) % TS_MAD_WINDOW_SIZE;
        recent[1] = t->window[idx];
        idx = (idx - 1 + TS_MAD_WINDOW_SIZE) % TS_MAD_WINDOW_SIZE;
        recent[2] = t->window[idx];
        
        /* Sort 3 elements */
        if (recent[0] > recent[1]) { int8_t tmp = recent[0]; recent[0] = recent[1]; recent[1] = tmp; }
        if (recent[1] > recent[2]) { int8_t tmp = recent[1]; recent[1] = recent[2]; recent[2] = tmp; }
        if (recent[0] > recent[1]) { int8_t tmp = recent[0]; recent[0] = recent[1]; recent[1] = tmp; }
        
        rssi_filtered = (float)recent[1];  /* Median */
    }
    
    /* EWMA update */
    t->level = t->alpha * rssi_filtered + (1.0f - t->alpha) * t->level;
    entity->last_rssi = rssi;
    
    /* Calculate MAD (Median Absolute Deviation) for robustness score */
    if (t->window_count < 3) {
        return 1.0f;  /* Not enough data */
    }
    
    /* Find median of window */
    int8_t sorted[TS_MAD_WINDOW_SIZE];
    memcpy(sorted, t->window, t->window_count);
    
    /* Simple insertion sort for small array */
    for (int i = 1; i < t->window_count; i++) {
        int8_t key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > key) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }
    
    float median = (float)sorted[t->window_count / 2];
    
    /* Calculate MAD */
    float abs_devs[TS_MAD_WINDOW_SIZE];
    for (int i = 0; i < t->window_count; i++) {
        abs_devs[i] = fabsf((float)t->window[i] - median);
    }
    
    /* Sort abs_devs */
    for (int i = 1; i < t->window_count; i++) {
        float key = abs_devs[i];
        int j = i - 1;
        while (j >= 0 && abs_devs[j] > key) {
            abs_devs[j + 1] = abs_devs[j];
            j--;
        }
        abs_devs[j + 1] = key;
    }
    
    float mad = abs_devs[t->window_count / 2];
    if (mad < 1.0f) mad = 1.0f;
    
    /* Robustness = 1 / (1 + MAD) */
    return 1.0f / (1.0f + mad);
}

/* ============================================================================
 * Entity Management
 * ========================================================================== */

static void entity_init(ts_entity_t *entity, const char *mac) {
    memset(entity, 0, sizeof(ts_entity_t));
    strncpy(entity->entity_id, mac, TS_MAC_STR_LEN - 1);
    entity->alpha = 1.0f;   /* Neutral prior */
    entity->beta = 1.0f;
    entity->status = ENTITY_ACTIVE;
    entity->first_seen = time(NULL);
    entity->last_seen = entity->first_seen;
    entity->in_use = true;
    signal_tracker_init(&entity->signal);
}

ts_entity_t *ts_get_or_create_entity(ts_brain_t *brain, const char *mac) {
    pthread_mutex_lock(&brain->lock);
    
    /* Look for existing */
    for (int i = 0; i < TS_MAX_ENTITIES; i++) {
        if (brain->entities[i].in_use &&
            strcasecmp(brain->entities[i].entity_id, mac) == 0) {
            brain->entities[i].last_seen = time(NULL);
            pthread_mutex_unlock(&brain->lock);
            return &brain->entities[i];
        }
    }
    
    /* Find empty slot */
    for (int i = 0; i < TS_MAX_ENTITIES; i++) {
        if (!brain->entities[i].in_use) {
            entity_init(&brain->entities[i], mac);
            brain->entity_count++;
            pthread_mutex_unlock(&brain->lock);
            return &brain->entities[i];
        }
    }
    
    pthread_mutex_unlock(&brain->lock);
    fprintf(stderr, "[thompson] entity table full (%d)\n", TS_MAX_ENTITIES);
    return NULL;
}

ts_entity_t *ts_find_entity(ts_brain_t *brain, const char *mac) {
    pthread_mutex_lock(&brain->lock);
    
    for (int i = 0; i < TS_MAX_ENTITIES; i++) {
        if (brain->entities[i].in_use &&
            strcasecmp(brain->entities[i].entity_id, mac) == 0) {
            pthread_mutex_unlock(&brain->lock);
            return &brain->entities[i];
        }
    }
    
    pthread_mutex_unlock(&brain->lock);
    return NULL;
}

void ts_update_entity_metadata(ts_entity_t *entity,
                               const char *ssid,
                               const char *vendor_oui,
                               uint8_t channel,
                               uint16_t beacon_interval,
                               const char *encryption) {
    if (ssid) strncpy(entity->ssid, ssid, TS_SSID_MAX_LEN - 1);
    if (vendor_oui) strncpy(entity->vendor_oui, vendor_oui, TS_VENDOR_MAX_LEN - 1);
    entity->channel = channel;
    entity->beacon_interval = (beacon_interval / 50) * 50;  /* Bucket to 50ms */
    if (encryption) strncpy(entity->encryption, encryption, sizeof(entity->encryption) - 1);
    
    ts_compute_soft_identity(entity);
}

/* ============================================================================
 * Soft Identity (Behavioral Hash)
 * ========================================================================== */

/* Simple hash function for soft identity */
static uint32_t fnv1a_hash(const char *str, size_t len) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)str[i];
        hash *= 16777619u;
    }
    return hash;
}

void ts_compute_soft_identity(ts_entity_t *entity) {
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "%s_%d_%d_%s",
                       entity->vendor_oui,
                       entity->beacon_interval / 50,
                       entity->channel,
                       entity->encryption);
    
    uint32_t hash = fnv1a_hash(buf, len);
    snprintf(entity->soft_identity, TS_IDENTITY_HASH_LEN, "%08x%08x",
             hash, fnv1a_hash(buf + len/2, len - len/2));
}

bool ts_detect_identity_drift(ts_entity_t *entity,
                              const char *new_ssid,
                              const char *new_vendor,
                              uint8_t new_channel,
                              uint16_t new_beacon,
                              const char *new_encryption) {
    /* Compute new soft identity */
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "%s_%d_%d_%s",
                       new_vendor ? new_vendor : "",
                       new_beacon / 50,
                       new_channel,
                       new_encryption ? new_encryption : "");
    
    uint32_t hash = fnv1a_hash(buf, len);
    char new_identity[TS_IDENTITY_HASH_LEN];
    snprintf(new_identity, TS_IDENTITY_HASH_LEN, "%08x%08x",
             hash, fnv1a_hash(buf + len/2, len - len/2));
    
    /* Count character differences */
    int diff_count = 0;
    for (int i = 0; i < TS_IDENTITY_HASH_LEN - 1; i++) {
        if (entity->soft_identity[i] != new_identity[i]) {
            diff_count++;
        }
    }
    
    /* Threshold: >4 character difference = drift */
    return diff_count > 4;
}

/* ============================================================================
 * Thompson Sampling Core
 * ========================================================================== */

void ts_observe_outcome(ts_entity_t *entity, bool success, float robustness_score) {
    /* Clamp robustness to [0.1, 1.0] */
    float w = robustness_score;
    if (w < 0.1f) w = 0.1f;
    if (w > 1.0f) w = 1.0f;
    
    /* Update Thompson priors */
    if (success) {
        entity->alpha += w;
        entity->total_successes++;
    } else {
        entity->beta += w;
    }
    
    entity->total_interactions++;
    entity->last_seen = time(NULL);
    
    /* Keep entity active */
    if (entity->status == ENTITY_STALE) {
        entity->status = ENTITY_ACTIVE;
    }
}

void ts_decay_entity(ts_entity_t *entity, time_t now) {
    float dormant_days = (float)(now - entity->last_seen) / 86400.0f;
    
    if (dormant_days > TS_ARCHIVE_DAYS) {
        entity->status = ENTITY_ARCHIVED;
        /* Strong decay toward neutral */
        float lambda = 0.7f;
        entity->alpha = (1.0f - lambda) * entity->alpha + lambda * 1.0f;
        entity->beta = (1.0f - lambda) * entity->beta + lambda * 1.0f;
    } else if (dormant_days > TS_STALE_DAYS) {
        entity->status = ENTITY_STALE;
        /* Gradual decay */
        float lambda = 0.3f * (dormant_days / TS_STALE_DAYS);
        entity->alpha = (1.0f - lambda) * entity->alpha + lambda * 1.0f;
        entity->beta = (1.0f - lambda) * entity->beta + lambda * 1.0f;
    }
}

float ts_score_entity(ts_brain_t *brain, ts_entity_t *entity, const ts_action_t *action) {
    /* Sample success probability from Beta distribution */
    float success_prob = ts_beta_sample(entity->alpha, entity->beta);
    
    /* Calculate total cost */
    float total_cost = action->cost_time +
                       action->cost_energy * brain->cost_weight_energy +
                       action->cost_risk * brain->cost_weight_risk;
    
    /* Exploration bonus: uncertainty = 1 / sqrt(ESS) */
    float ess = entity->alpha + entity->beta;
    float uncertainty = 1.0f / sqrtf(ess);
    float exploration = brain->exploration_bonus * uncertainty;
    
    /* Factor in client boost (APs with clients more likely to yield handshakes) */
    float client_factor = entity->client_boost > 0 ? entity->client_boost : 1.0f;
    
    /* Score = (success_prob + exploration_bonus) * client_factor / cost */
    return (success_prob + exploration) * client_factor / (total_cost + 0.01f);
}

ts_entity_t *ts_decide_entity(ts_brain_t *brain,
                              ts_entity_t **entities,
                              int count,
                              const ts_action_t *action) {
    if (count == 0) return NULL;
    
    ts_entity_t *best = NULL;
    float best_score = -1.0f;
    
    for (int i = 0; i < count; i++) {
        ts_entity_t *e = entities[i];
        if (!e || !e->in_use) continue;
        if (e->status == ENTITY_FLAGGED || e->status == ENTITY_ARCHIVED) continue;
        
        float score = ts_score_entity(brain, e, action);
        if (score > best_score) {
            best_score = score;
            best = e;
        }
    }
    
    if (best) {
        brain->total_decisions++;
    }
    
    return best;
}

/* ============================================================================
 * Garbage Collection
 * ========================================================================== */

int ts_garbage_collect(ts_brain_t *brain) {
    time_t now = time(NULL);
    int evicted = 0;
    
    pthread_mutex_lock(&brain->lock);
    
    for (int i = 0; i < TS_MAX_ENTITIES; i++) {
        ts_entity_t *e = &brain->entities[i];
        if (!e->in_use) continue;
        
        float age_days = (float)(now - e->first_seen) / 86400.0f;
        float dormant_days = (float)(now - e->last_seen) / 86400.0f;
        
        /* Evict old + dormant entities */
        if (age_days > TS_EVICT_DAYS && dormant_days > TS_EVICT_DAYS) {
            e->in_use = false;
            brain->entity_count--;
            evicted++;
            continue;
        }
        
        /* Decay others */
        ts_decay_entity(e, now);
    }
    
    pthread_mutex_unlock(&brain->lock);
    
    if (evicted > 0) {
        fprintf(stderr, "[thompson] garbage collected %d entities\n", evicted);
    }
    
    return evicted;
}

/* ============================================================================
 * Mode Bandit
 * ========================================================================== */

ts_mode_t ts_select_mode(ts_brain_t *brain) {
    float samples[MODE_COUNT];
    float max_sample = -1.0f;
    ts_mode_t best_mode = MODE_PASSIVE_DISCOVERY;
    
    /* Sample from each mode's Beta distribution */
    for (int i = 0; i < MODE_COUNT; i++) {
        samples[i] = ts_beta_sample(brain->mode.alpha[i], brain->mode.beta[i]);
        if (samples[i] > max_sample) {
            max_sample = samples[i];
            best_mode = (ts_mode_t)i;
        }
    }
    
    /* If modes too similar, inject entropy */
    float min_sample = samples[0];
    for (int i = 1; i < MODE_COUNT; i++) {
        if (samples[i] < min_sample) min_sample = samples[i];
    }
    
    if (max_sample - min_sample < 0.1f) {
        best_mode = (ts_mode_t)(rng_next() % MODE_COUNT);
    }
    
    brain->mode.current_mode = best_mode;
    brain->mode.mode_started = time(NULL);
    
    return best_mode;
}

void ts_observe_mode_outcome(ts_brain_t *brain, ts_mode_t mode, bool success) {
    if (mode >= MODE_COUNT) return;
    
    if (success) {
        brain->mode.alpha[mode] += 1.0f;
    } else {
        brain->mode.beta[mode] += 1.0f;
    }
}

const char *ts_mode_name(ts_mode_t mode) {
    if (mode >= MODE_COUNT) return "unknown";
    return mode_names[mode];
}

/* ============================================================================
 * Brain Lifecycle
 * ========================================================================== */

ts_brain_t *ts_brain_create(void) {
    rng_seed();
    
    ts_brain_t *brain = calloc(1, sizeof(ts_brain_t));
    if (!brain) return NULL;
    
    /* Initialize cost weights */
    brain->cost_weight_time = 1.0f;
    brain->cost_weight_energy = 20.0f;
    brain->cost_weight_risk = 5.0f;
    brain->exploration_bonus = 0.3f;
    
    /* Initialize mode bandit */
    for (int i = 0; i < MODE_COUNT; i++) {
        brain->mode.alpha[i] = 1.0f; if (i == 1) brain->mode.alpha[i] = 5.0f;
        brain->mode.beta[i] = 1.0f; if (i >= 2) brain->mode.beta[i] = 3.0f;
    }
    
    brain->started_at = time(NULL);
    pthread_mutex_init(&brain->lock, NULL);
    
    return brain;
}

void ts_brain_destroy(ts_brain_t *brain) {
    if (!brain) return;
    pthread_mutex_destroy(&brain->lock);
    free(brain);
}

/* ============================================================================
 * Persistence
 * ========================================================================== */

/* Simple binary format for state persistence */
#define TS_STATE_MAGIC  0x54534252  /* "TSBR" */
#define TS_STATE_VERSION 1

int ts_save_state(ts_brain_t *brain, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    
    pthread_mutex_lock(&brain->lock);
    
    /* Header */
    uint32_t magic = TS_STATE_MAGIC;
    uint32_t version = TS_STATE_VERSION;
    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(&version, sizeof(version), 1, f);
    
    /* Brain stats */
    fwrite(&brain->total_decisions, sizeof(brain->total_decisions), 1, f);
    fwrite(&brain->total_handshakes, sizeof(brain->total_handshakes), 1, f);
    fwrite(&brain->started_at, sizeof(brain->started_at), 1, f);
    
    /* Mode bandit */
    fwrite(brain->mode.alpha, sizeof(brain->mode.alpha), 1, f);
    fwrite(brain->mode.beta, sizeof(brain->mode.beta), 1, f);
    
    /* Entity count */
    fwrite(&brain->entity_count, sizeof(brain->entity_count), 1, f);
    
    /* Entities */
    for (int i = 0; i < TS_MAX_ENTITIES; i++) {
        if (brain->entities[i].in_use) {
            fwrite(&brain->entities[i], sizeof(ts_entity_t), 1, f);
        }
    }
    
    pthread_mutex_unlock(&brain->lock);
    fclose(f);
    
    fprintf(stderr, "[thompson] saved state to %s (%d entities)\n",
            path, brain->entity_count);
    return 0;
}

int ts_load_state(ts_brain_t *brain, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    
    pthread_mutex_lock(&brain->lock);
    
    /* Header */
    uint32_t magic, version;
    fread(&magic, sizeof(magic), 1, f);
    fread(&version, sizeof(version), 1, f);
    
    if (magic != TS_STATE_MAGIC || version != TS_STATE_VERSION) {
        pthread_mutex_unlock(&brain->lock);
        fclose(f);
        fprintf(stderr, "[thompson] invalid state file\n");
        return -1;
    }
    
    /* Brain stats */
    fread(&brain->total_decisions, sizeof(brain->total_decisions), 1, f);
    fread(&brain->total_handshakes, sizeof(brain->total_handshakes), 1, f);
    fread(&brain->started_at, sizeof(brain->started_at), 1, f);
    
    /* Mode bandit */
    fread(brain->mode.alpha, sizeof(brain->mode.alpha), 1, f);
    fread(brain->mode.beta, sizeof(brain->mode.beta), 1, f);
    
    /* Entity count */
    int count;
    fread(&count, sizeof(count), 1, f);
    
    /* Clear existing entities */
    memset(brain->entities, 0, sizeof(brain->entities));
    brain->entity_count = 0;
    
    /* Load entities */
    for (int i = 0; i < count; i++) {
        ts_entity_t entity;
        if (fread(&entity, sizeof(entity), 1, f) != 1) break;
        
        /* Find slot and copy */
        for (int j = 0; j < TS_MAX_ENTITIES; j++) {
            if (!brain->entities[j].in_use) {
                brain->entities[j] = entity;
                brain->entity_count++;
                break;
            }
        }
    }
    
    pthread_mutex_unlock(&brain->lock);
    fclose(f);
    
    fprintf(stderr, "[thompson] loaded state from %s (%d entities)\n",
            path, brain->entity_count);
    return 0;
}
