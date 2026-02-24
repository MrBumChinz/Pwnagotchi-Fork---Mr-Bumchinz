/* ============================================================================
 * mobility_mode.h - Driving / Walking / Stationary Mode Detection
 *
 * Phase 1C: Fundamentally different strategies for different scenarios.
 *
 * Uses the existing mobility_score (GPS distance + AP churn) plus raw GPS
 * speed to classify into three mobility modes, each with its own set of
 * brain parameters:
 *
 *   STATIONARY: Deep, thorough attacks on nearby APs. Full attack suite.
 *   WALKING:    Balanced — prioritize high-probability attacks (PMKID, deauth).
 *   DRIVING:    Sprint mode — PMKID-only, fast channel hops, no deauths.
 *
 * Hysteresis prevents flapping: 3 consecutive readings in a new mode before
 * switching. On switch, parameters are applied to brain_config_t.
 *
 * Copyright (c) 2026. All rights reserved.
 * ============================================================================ */

#ifndef MOBILITY_MODE_H
#define MOBILITY_MODE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

/* Hysteresis: consecutive readings before mode switch */
#define MOB_HYSTERESIS_COUNT    3      /* AP-churn-only fallback: 3 readings */
#define MOB_GPS_HYSTERESIS      1      /* GPS-confirmed: instant (Doppler is reliable at 1Hz) */

/* GPS speed thresholds (km/h) */
#define MOB_SPEED_DRIVING       10.0    /* > 10 km/h = driving (was 15, too high for suburban) */
#define MOB_SPEED_WALKING        1.5    /* 1.5-10 km/h = walking */
                                        /* < 1.5 km/h = stationary */

/* Speed smoothing: EMA alpha (higher = more reactive)
 * At 0.9, speed tracks GPS within 1 reading.
 * Doppler speed is already filtered by the receiver chip. */
#define MOB_SPEED_SMOOTH_ALPHA  0.9f   /* Near-instant tracking: 1 reading to converge */
#define MOB_SPEED_MAX_SANE      120.0f  /* Reject GPS > 120 km/h as noise */
#define MOB_GPS_HOLDOVER_S      30      /* Use last GPS speed for up to 30s after dropout */
#define MOB_SWITCH_COOLDOWN_S   5       /* Min 5s between mode switches (was 8) */

/* AP churn thresholds (fraction of total APs changed per check) */
#define MOB_CHURN_DRIVING        0.6    /* > 60% AP turnover = driving */
#define MOB_CHURN_WALKING        0.25   /* 25-60% = walking (raised from 0.15 — too sensitive at low AP count) */
                                        /* < 25% = stationary */
#define MOB_MIN_APS_FOR_CHURN    5      /* Need >= 5 APs for churn to be meaningful */

/* Mode-specific parameters */

/* DRIVING: Fast scan, minimal attacks, no deauths */
#define MOB_DRV_RECON_TIME       3      /* 3s recon per channel */
#define MOB_DRV_HOP_RECON_TIME   2      /* 2s after attack */
#define MOB_DRV_MIN_RECON_TIME   1      /* 1s minimum */
#define MOB_DRV_MAX_RECON_TIME   8      /* 8s maximum */
#define MOB_DRV_THROTTLE_A       0.1f   /* Very fast association */
#define MOB_DRV_THROTTLE_D       0.0f   /* No deauth delay (disabled anyway) */

/* WALKING: Balanced */
#define MOB_WLK_RECON_TIME       7      /* 7s recon */
#define MOB_WLK_HOP_RECON_TIME   4      /* 4s after attack */
#define MOB_WLK_MIN_RECON_TIME   2      /* 2s minimum */
#define MOB_WLK_MAX_RECON_TIME   20     /* 20s maximum */
#define MOB_WLK_THROTTLE_A       0.3f   /* Fast association */
#define MOB_WLK_THROTTLE_D       0.3f   /* Fast deauth */

/* STATIONARY: Deep, thorough */
#define MOB_STA_RECON_TIME       10     /* 10s recon */
#define MOB_STA_HOP_RECON_TIME   5      /* 5s after attack */
#define MOB_STA_MIN_RECON_TIME   2      /* 2s minimum */
#define MOB_STA_MAX_RECON_TIME   30     /* 30s maximum */
#define MOB_STA_THROTTLE_A       0.5f   /* Normal association */
#define MOB_STA_THROTTLE_D       0.5f   /* Normal deauth */

/* Label strings for display */
#define MOB_LABEL_STATIONARY    "STA"
#define MOB_LABEL_WALKING       "WLK"
#define MOB_LABEL_DRIVING       "DRV"

/* ============================================================================
 * Types
 * ============================================================================ */

typedef enum {
    MOBILITY_STATIONARY = 0,    /* Parked / sitting still */
    MOBILITY_WALKING,           /* On foot, slow movement */
    MOBILITY_DRIVING,           /* In vehicle, fast movement */
    MOBILITY_MODE_COUNT
} mobility_mode_t;

/* Mode-specific parameter set */
typedef struct {
    int  recon_time;            /* Base recon time (seconds) */
    int  hop_recon_time;        /* Recon after attack */
    int  min_recon_time;        /* Minimum recon */
    int  max_recon_time;        /* Maximum recon */
    float throttle_a;           /* Association delay */
    float throttle_d;           /* Deauth delay */
    bool deauth_enabled;        /* Allow deauth attacks */
    bool associate_enabled;     /* Allow association attacks */
    bool pmkid_only;            /* Only do PMKID attacks (phases 0,7) */
} mobility_params_t;

/* Main mobility mode context */
typedef struct {
    /* Current state */
    mobility_mode_t current_mode;       /* Active mode */
    mobility_mode_t pending_mode;       /* Candidate mode (during hysteresis) */
    int pending_count;                  /* Consecutive readings in pending_mode */

    /* Input snapshot (updated each check) */
    float gps_speed_kmh;                /* Latest GPS speed */
    float smoothed_speed;               /* EMA-smoothed GPS speed */
    float mobility_score;               /* Latest brain mobility_score */
    float ap_churn_rate;                /* AP churn fraction (0.0-1.0) */
    int   total_aps;                    /* Total APs visible */

    /* Timestamps */
    time_t last_check;                  /* Last update_mobility call */
    time_t last_valid_gps;              /* Last time GPS speed was >= 0 */
    time_t mode_since;                  /* When current mode was entered */
    time_t session_start;               /* When monitoring started */

    /* Statistics */
    uint32_t switches;                  /* Total mode switches */
    time_t time_in_mode[MOBILITY_MODE_COUNT]; /* Cumulative time per mode */

    /* Cached parameters for current mode */
    mobility_params_t params;

    bool initialized;
} mobility_ctx_t;

/* ============================================================================
 * API Functions
 * ============================================================================ */

/**
 * Initialize mobility mode detector.
 *
 * @param ctx       Mobility context (caller-owned)
 * @return 0 on success
 */
int mobility_mode_init(mobility_ctx_t *ctx);

/**
 * Update mobility mode based on current inputs.
 *
 * Call this once per epoch or once per mobility check cycle.
 * If the mode changes, returns true and updates ctx->params.
 *
 * @param ctx           Mobility context
 * @param gps_speed     Current GPS speed in km/h (0 if no fix)
 * @param mob_score     Current brain mobility_score (0.0-1.0)
 * @param ap_churn      AP churn fraction (0.0-1.0)
 * @param total_aps     Total visible APs
 * @return true if mode changed (caller should apply new params)
 */
bool mobility_mode_update(mobility_ctx_t *ctx,
                          float gps_speed,
                          float mob_score,
                          float ap_churn,
                          int total_aps);

/**
 * Get current mobility mode.
 */
mobility_mode_t mobility_mode_get(const mobility_ctx_t *ctx);

/**
 * Get parameter set for current mode.
 */
const mobility_params_t *mobility_mode_get_params(const mobility_ctx_t *ctx);

/**
 * Get parameter set for a specific mode.
 */
void mobility_mode_get_mode_params(mobility_mode_t mode, mobility_params_t *out);

/**
 * Get display label for current mode ("STA", "WLK", "DRV").
 */
const char *mobility_mode_label(mobility_mode_t mode);

/**
 * Get full name for a mode.
 */
const char *mobility_mode_name(mobility_mode_t mode);

/**
 * Get status string for logging/display.
 *
 * @param ctx       Mobility context
 * @param buf       Output buffer
 * @param len       Buffer length
 */
void mobility_mode_status_str(const mobility_ctx_t *ctx, char *buf, size_t len);

/**
 * Get time spent in current mode (seconds).
 */
int mobility_mode_time_in_current(const mobility_ctx_t *ctx);

#endif /* MOBILITY_MODE_H */
