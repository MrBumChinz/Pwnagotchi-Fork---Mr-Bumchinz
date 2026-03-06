/* ============================================================================
 * mobility_mode.h - Driving / Walking / Stationary Mode Detection
 *
 * V3: Multi-signal fusion.  Uses ALL available phone sensor data:
 *   - Activity Recognition API (ML-fused, 5-30s latency)
 *   - Accelerometer RMS (real-time body motion, <1s latency)
 *   - Step counter (hardware pedometer, definitive walking proof)
 *   - GPS Doppler speed (accurate above 5 km/h)
 *
 * V2 only used Activity Recognition + GPS speed, completely ignoring the
 * accelerometer and step counter.  This caused 5-30 second detection lag
 * because Activity Recognition is ML-batched and GPS Doppler reports 0
 * on Samsung chips below 5 km/h.
 *
 * Decision logic (V3 — multi-signal fusion):
 *   1. GPS speed >= 10 km/h               -> DRIVING   (Doppler definitive)
 *   2. Phone Activity (confidence >= 60%)  -> trust it, UNLESS STILL is
 *                                             contradicted by accel/steps
 *   3. Step counter incrementing (8s)      -> WALKING   (hardware pedometer)
 *   4. Accelerometer RMS > 0.4 m/s^2      -> WALKING   (body in motion)
 *   5. GPS speed 1-10 km/h                -> WALKING
 *   6. Default                            -> STATIONARY
 *
 * Hysteresis: 2 consecutive readings in new mode before switching.
 * 5-second cooldown between switches.
 *
 * Copyright (c) 2026. All rights reserved.
 * ============================================================================ */

#ifndef MOBILITY_MODE_H
#define MOBILITY_MODE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* ============================================================================
 * Constants — kept intentionally minimal
 * ============================================================================ */

/* GPS speed thresholds (km/h) — only used when phone activity unavailable */
#define MOB_SPEED_DRIVING        9.0f   /* > 9 km/h = driving (lowered from 10 — calibration showed 9.8 border mismatches) */
#define MOB_SPEED_WALKING        1.0f   /* 1-10 km/h = walking */
#define MOB_SPEED_MAX_SANE     150.0f   /* Reject GPS > 150 km/h as noise */

/* Hysteresis: 2 consecutive same-mode readings before switching */
#define MOB_HYSTERESIS          1

/* Cooldown: minimum seconds between mode switches (reduced from 10 in V2) */
#define MOB_SWITCH_COOLDOWN_S   5

/* Phone activity confidence threshold (0-100) */
#define MOB_ACTIVITY_MIN_CONF   60

/* Phone activity staleness: ignore if older than 15s */
#define MOB_ACTIVITY_STALE_S    15

/* Accelerometer thresholds (m/s^2 RMS linear, gravity removed)
 * Stationary noise floor: ~0.02-0.10 m/s^2
 * Walking body bounce:   ~0.5-3.0 m/s^2
 * Driving vibration:     ~0.1-0.5 m/s^2 (smooth road)
 * These are conservative: 0.15 avoids false positives from sensor noise,
 * 0.4 catches walking without triggering on gentle car vibration. */
#define MOB_ACCEL_STILL         0.15f   /* Below = definitely stationary */
#define MOB_ACCEL_WALKING       0.4f    /* Above = body in motion */

/* Step activity: if steps incremented within this window, actively walking */
#define MOB_STEP_ACTIVE_S       8

/* Hull Moving Average parameters.
 * HMA(n) = WMA(sqrt(n)) of [ 2*WMA(n/2) - WMA(n) ]
 * Period 4 at 2s sampling = 8s window.  Responds as fast as alpha=0.9 EMA
 * with no lag, no snap-down hacks, and equal speed in both directions. */
#define MOB_HMA_PERIOD          4       /* n — ring buffer size for raw accel */
#define MOB_HMA_HALF            2       /* n/2 — short WMA period */
#define MOB_HMA_SQRT            2       /* sqrt(n) — final smoothing period */

/* Mode-specific parameters */

/* DRIVING: Fast scan, PMKID-only, no deauths */
#define MOB_DRV_RECON_TIME       3
#define MOB_DRV_HOP_RECON_TIME   2
#define MOB_DRV_MIN_RECON_TIME   1
#define MOB_DRV_MAX_RECON_TIME   8
#define MOB_DRV_THROTTLE_A       0.1f
#define MOB_DRV_THROTTLE_D       0.0f

/* WALKING: Balanced — PMKID + deauth + CSA */
#define MOB_WLK_RECON_TIME       7
#define MOB_WLK_HOP_RECON_TIME   4
#define MOB_WLK_MIN_RECON_TIME   2
#define MOB_WLK_MAX_RECON_TIME   20
#define MOB_WLK_THROTTLE_A       0.3f
#define MOB_WLK_THROTTLE_D       0.3f

/* STATIONARY: Deep, thorough — all attacks */
#define MOB_STA_RECON_TIME       10
#define MOB_STA_HOP_RECON_TIME   5
#define MOB_STA_MIN_RECON_TIME   2
#define MOB_STA_MAX_RECON_TIME   30
#define MOB_STA_THROTTLE_A       0.5f
#define MOB_STA_THROTTLE_D       0.5f

/* Label strings */
#define MOB_LABEL_STATIONARY    "STA"
#define MOB_LABEL_WALKING       "WLK"
#define MOB_LABEL_DRIVING       "DRV"

/* ============================================================================
 * Types
 * ============================================================================ */

typedef enum {
    MOBILITY_STATIONARY = 0,
    MOBILITY_WALKING,
    MOBILITY_DRIVING,
    MOBILITY_MODE_COUNT
} mobility_mode_t;

/* Mode-specific parameter set */
typedef struct {
    int  recon_time;
    int  hop_recon_time;
    int  min_recon_time;
    int  max_recon_time;
    float throttle_a;
    float throttle_d;
    bool deauth_enabled;
    bool associate_enabled;
    bool pmkid_only;
} mobility_params_t;

/* Phone activity data (from Android Activity Recognition API) */
typedef struct {
    char activity[16];      /* "STILL", "WALKING", "RUNNING", "IN_VEHICLE", "ON_BICYCLE", "UNKNOWN" */
    int  confidence;        /* 0-100 */
    time_t updated;         /* When this was last set */
} phone_activity_t;

/* Main mobility context — drastically simplified */
typedef struct {
    /* Current state */
    mobility_mode_t current_mode;
    mobility_mode_t pending_mode;
    int pending_count;

    /* Raw inputs (no smoothing) */
    float gps_speed_kmh;            /* Raw GPS speed this reading */
    float prev_gps_speed_kmh;       /* Previous cycle GPS speed (for dropout persistence) */
    float accel_magnitude;          /* Phone accel RMS (m/s²) */
    int   step_count;               /* Cumulative steps */
    int   prev_step_count;          /* Previous for delta */
    time_t last_step_time;          /* When step_count last increased (for active walking detection) */

    /* Hull Moving Average for accelerometer smoothing.
     * Eliminates lag vs EMA — responds equally fast up and down. */
    float  accel_ring[MOB_HMA_PERIOD]; /* Raw accel ring buffer (4 samples) */
    int    accel_ring_idx;             /* Next write position */
    int    accel_ring_count;           /* Samples received (0..4) */
    float  accel_diff[MOB_HMA_SQRT];  /* Diff series ring buffer (2 samples) */
    int    accel_diff_idx;             /* Next write position */
    int    accel_diff_count;           /* Diffs computed (0..2) */
    float  accel_hma;                  /* HMA output (used by classify) */

    phone_activity_t phone_activity; /* Android Activity Recognition result */

    /* Calibration ground truth from app (user presses STA/WLK/DRV button) */
    char calibration[4];               /* "STA", "WLK", "DRV", or "" */

    /* Timestamps */
    time_t last_check;
    time_t mode_since;
    time_t session_start;

    /* Statistics */
    uint32_t switches;
    time_t time_in_mode[MOBILITY_MODE_COUNT];

    /* Cached parameters for current mode */
    mobility_params_t params;

    bool initialized;
} mobility_ctx_t;

/* ============================================================================
 * API Functions
 * ============================================================================ */

/**
 * Initialize mobility mode detector.
 */
int mobility_mode_init(mobility_ctx_t *ctx);

/**
 * Update mobility mode based on current inputs.
 *
 * @param ctx           Mobility context
 * @param gps_speed     Raw GPS speed in km/h (-1 if no fix)
 * @param mob_score     Brain mobility_score (unused in V2, kept for API compat)
 * @param ap_churn      AP churn fraction (unused in V2, kept for API compat)
 * @param total_aps     Total visible APs (unused in V2, kept for API compat)
 * @param accel         Phone accelerometer RMS magnitude (m/s²)
 * @param steps         Phone step counter (cumulative)
 * @return true if mode changed (caller should apply new params)
 */
bool mobility_mode_update(mobility_ctx_t *ctx,
                          float gps_speed,
                          float mob_score,
                          float ap_churn,
                          int total_aps,
                          float accel,
                          int steps);

/**
 * Set phone activity from Android Activity Recognition API.
 * Call this when bt_gps_receiver parses an "activity" field from gps.json.
 */
void mobility_mode_set_activity(mobility_ctx_t *ctx,
                                const char *activity,
                                int confidence);

/**
 * Set calibration ground truth from app.
 * User presses STA/WLK/DRV button in app settings → sent via BT JSON.
 */
void mobility_mode_set_calibration(mobility_ctx_t *ctx, const char *mode);

mobility_mode_t mobility_mode_get(const mobility_ctx_t *ctx);
const mobility_params_t *mobility_mode_get_params(const mobility_ctx_t *ctx);
void mobility_mode_get_mode_params(mobility_mode_t mode, mobility_params_t *out);
const char *mobility_mode_label(mobility_mode_t mode);
const char *mobility_mode_name(mobility_mode_t mode);
void mobility_mode_status_str(const mobility_ctx_t *ctx, char *buf, size_t len);
int mobility_mode_time_in_current(const mobility_ctx_t *ctx);

#endif /* MOBILITY_MODE_H */
