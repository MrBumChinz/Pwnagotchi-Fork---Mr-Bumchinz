/**
 * model_inference.h -- ML model inference for PwnAUI Pi brain
 *
 * Provides fast int8 inference for all 5 trained models.
 * Weights are loaded from model_weights.h (auto-generated).
 *
 * All inference runs in <1ms on ARMv6 (Pi Zero W).
 */

#ifndef MODEL_INFERENCE_H
#define MODEL_INFERENCE_H

#include <stdint.h>

/* Feature struct passed to vulnerability + phase models */
typedef struct {
    float vendor_category;      /* 0=unknown, 1=residential, 2=commercial, 3=IoT */
    float encryption_type;      /* -1=OPEN, 0=WEP, 1=WPA/WPA2, 2=WPA3 */
    float channel_norm;         /* channel / 14.0, clipped [0,1] */
    float beacon_flag;          /* 0=standard, 1=custom, 2=hidden */
    float client_count_log;     /* log(clients+1) */
    float rssi_norm;            /* (rssi+100)/60, clipped [0,1] */
    float time_of_day;          /* hour/24.0 */
} ap_features_t;

/* Extended features for phase selector */
typedef struct {
    ap_features_t base;         /* First 7 features */
    float thompson_ratio;       /* alpha / (alpha + beta) */
    float has_pmkid;            /* 0 or 1 */
    float is_wpa3;              /* 0 or 1 */
} ap_features_ext_t;

/* Channel yield query */
typedef struct {
    float time_of_day;          /* hour / 24.0 */
    float gps_zone;             /* zone hash (0 if no GPS) */
    float day_of_week;          /* weekday / 7.0 */
} channel_query_t;

/* Password pattern query */
typedef struct {
    float vendor_category;
    float isp_id;
    float encryption_type;
    float ssid_length;
    float ssid_starts_upper;
    float is_wpa3;
} password_query_t;

/* ---- API ---- */

/**
 * Initialize inference engine. Call once at startup.
 * Checks model availability flags from model_weights.h.
 */
void model_inference_init(void);

/**
 * Model 1: Predict vulnerability (P(handshake capture in 60s))
 * Returns probability [0.0, 1.0]
 * Returns 0.5 if model not available.
 */
float predict_vulnerability(const ap_features_t *ap);

/**
 * Model 2: Predict best attack phase (0-7)
 * Uses AP features + Thompson state.
 * Returns recommended phase index, or -1 if model not available.
 */
int predict_attack_phase(const ap_features_ext_t *ap);

/**
 * Model 3: Predict channel yield probabilities
 * Fills yield[14] with expected capture probability per channel.
 * Returns 0 on success, -1 if model not available.
 */
int predict_channel_yield(const channel_query_t *query, float yield[14]);

/**
 * Model 4: Get optimal dwell time for a channel
 * Returns seconds to dwell, or default (30) if model not available.
 */
int get_dwell_time(int ap_count, int mobility_mode, float success_rate);

/**
 * Model 5: Predict password pattern category (0-7)
 * Returns category index, or -1 if model not available.
 * Category maps to hashcat mask on the PC side.
 */
int predict_password_pattern(const password_query_t *query);

/**
 * Helper: Build ap_features_t from raw AP data.
 */
ap_features_t make_ap_features(const char *vendor, const char *encryption,
                                int channel, const char *ssid,
                                int clients_seen, int best_rssi,
                                int last_seen_hour);

/**
 * Get model version currently loaded.
 */
int model_get_version(void);

/**
 * Check if a specific model is available.
 * model_id: 1-5
 */
int model_is_available(int model_id);

#endif /* MODEL_INFERENCE_H */
