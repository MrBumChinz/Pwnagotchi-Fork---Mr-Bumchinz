/* ============================================================================
 * mobility_mode.c - Driving / Walking / Stationary Mode Detection (V3)
 *
 * V3: Multi-signal fusion.  Uses ALL available phone sensor data:
 *   - Activity Recognition API (ML-fused, trusted but 5-30s latency)
 *   - Accelerometer RMS (real-time body motion, <1s latency)
 *   - Step counter (hardware pedometer, definitive walking proof)
 *   - GPS Doppler speed (accurate above 5 km/h)
 *
 * V2 only used Activity Recognition + GPS speed, completely ignoring the
 * accelerometer and step counter — causing 5-30s lag during transitions.
 *
 * Copyright (c) 2026. All rights reserved.
 * ============================================================================ */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "mobility_mode.h"

/* ============================================================================
 * Static parameter tables (one per mode)
 * ============================================================================ */

static const mobility_params_t MODE_PARAMS[MOBILITY_MODE_COUNT] = {
    /* STATIONARY: Deep, thorough — all attacks enabled */
    [MOBILITY_STATIONARY] = {
        .recon_time       = MOB_STA_RECON_TIME,
        .hop_recon_time   = MOB_STA_HOP_RECON_TIME,
        .min_recon_time   = MOB_STA_MIN_RECON_TIME,
        .max_recon_time   = MOB_STA_MAX_RECON_TIME,
        .throttle_a       = MOB_STA_THROTTLE_A,
        .throttle_d       = MOB_STA_THROTTLE_D,
        .deauth_enabled   = true,
        .associate_enabled = true,
        .pmkid_only       = false,
    },

    /* WALKING: Balanced — PMKID + deauth + CSA, faster hops */
    [MOBILITY_WALKING] = {
        .recon_time       = MOB_WLK_RECON_TIME,
        .hop_recon_time   = MOB_WLK_HOP_RECON_TIME,
        .min_recon_time   = MOB_WLK_MIN_RECON_TIME,
        .max_recon_time   = MOB_WLK_MAX_RECON_TIME,
        .throttle_a       = MOB_WLK_THROTTLE_A,
        .throttle_d       = MOB_WLK_THROTTLE_D,
        .deauth_enabled   = true,
        .associate_enabled = true,
        .pmkid_only       = false,
    },

    /* DRIVING: Sprint mode — PMKID-only, no deauths, fastest hops */
    [MOBILITY_DRIVING] = {
        .recon_time       = MOB_DRV_RECON_TIME,
        .hop_recon_time   = MOB_DRV_HOP_RECON_TIME,
        .min_recon_time   = MOB_DRV_MIN_RECON_TIME,
        .max_recon_time   = MOB_DRV_MAX_RECON_TIME,
        .throttle_a       = MOB_DRV_THROTTLE_A,
        .throttle_d       = MOB_DRV_THROTTLE_D,
        .deauth_enabled   = false,
        .associate_enabled = false,
        .pmkid_only       = true,
    },
};

/* ============================================================================
 * Internal: classify using multi-signal fusion
 * ============================================================================ */

/**
 * Classify mobility mode using ALL available sensor data.  V3.3
 *
 * Signal priority (refined from 2nd field test):
 *   0. Stationary snap: accel noise + no steps + low GPS = STA
 *   1. GPS speed >= 10 km/h  → DRIVING (Doppler definitive, ALWAYS wins)
 *   2. Activity Recognition  → with GPS speed cross-validation:
 *      - IN_VEHICLE/ON_BICYCLE at GPS < 10 → treat as WALKING (not fast enough for DRV)
 *      - IN_VEHICLE/ON_BICYCLE at GPS = 0 + accel → keep DRV if already DRV, else WLK
 *      - STILL overridden by accel/steps
 *   3. Step counter active   → WALKING
 *   4. GPS speed 1-10        → WALKING
 *   5. Accelerometer motion  → WALKING (GPS absent/zero only)
 *   6. Default               → STATIONARY
 *
 * Key V3.3 fix: AR's IN_VEHICLE/ON_BICYCLE no longer blindly returns DRV.
 * Field test showed ON_BICYCLE(96-99%) reported continuously while riding at
 * 4-6 kph — clearly walking-speed.  AR can't distinguish fast vs slow bicycle.
 * GPS speed is the ground truth for speed; AR only confirms vehicle-type. */
static mobility_mode_t classify(const mobility_ctx_t *ctx, float gps_speed) {
    time_t now = time(NULL);

    /* ---- Gather all signal states ---- */

    /* Activity Recognition */
    const phone_activity_t *pa = &ctx->phone_activity;
    bool act_fresh = (pa->confidence >= MOB_ACTIVITY_MIN_CONF &&
                      pa->updated > 0 &&
                      (now - pa->updated) < MOB_ACTIVITY_STALE_S);

    /* Step counter: actively incrementing within window */
    bool stepping = (ctx->last_step_time > 0 &&
                     (now - ctx->last_step_time) < MOB_STEP_ACTIVE_S);

    /* Accelerometer: EMA-smoothed, gravity removed */
    float accel = ctx->accel_hma;
    bool accel_moving = (accel >= MOB_ACCEL_WALKING);   /* > 0.4 m/s² */
    bool accel_still  = (accel < MOB_ACCEL_STILL);      /* < 0.15 m/s² */

    /* GPS speed */
    bool has_speed = (gps_speed >= 0.0f);
    bool speed_driving = (has_speed && gps_speed >= MOB_SPEED_DRIVING);  /* >= 10 km/h */
    bool speed_walking = (has_speed && gps_speed > MOB_SPEED_WALKING);   /* > 1 km/h */
    bool speed_stopped = (!has_speed || gps_speed < 1.0f);              /* definitely not moving by GPS */

    /* ---- 0. Stationary snap-to-zero: accel noise floor = not moving ---- */
    if (accel_still && !stepping && speed_stopped) {
        return MOBILITY_STATIONARY;
    }

    /* ---- 1. GPS speed >= 10 km/h = DRIVING ---- */
    /* Doppler is definitive at these speeds.  ALWAYS wins, regardless of
     * what Activity Recognition says.  This fixes: at 10-12 km/h, AR said
     * STILL(100%) which blocked DRV in V3.0 because AR was checked first. */
    if (speed_driving) return MOBILITY_DRIVING;

    /* ---- 2. Activity Recognition ---- */
    if (act_fresh) {
        /* IN_VEHICLE / ON_BICYCLE — only trust for DRV if GPS agrees or absent.
         * Field data shows AR reports ON_BICYCLE(96-99%) for MINUTES while
         * riding at 4-6 kph (clearly walking-speed).  AR can't distinguish
         * "on a bicycle going fast" from "on a bicycle going slow".
         * GPS speed IS the truth for speed — AR is only useful when GPS is
         * absent or to differentiate walking-speed vehicle from actual walking.
         *
         * Logic:
         *   GPS >= 10           → already returned DRV above (step 1)
         *   GPS 1-10 + vehicle  → WALKING (slow vehicle = walking speed params)
         *   GPS 0 + vehicle     → Only DRV if accel shows vibration (not stopped)
         *   GPS 0 + no accel    → STATIONARY (AR is stale from parked vehicle) */
        if (strcmp(pa->activity, "IN_VEHICLE") == 0 ||
            strcmp(pa->activity, "ON_BICYCLE") == 0) {
            if (speed_stopped && !accel_moving) {
                /* Stopped + quiet = AR stale, fall through */
            } else if (speed_stopped && accel_moving) {
                /* GPS=0 but vibrating — might be idling in vehicle or
                 * slow crawl (Samsung Doppler bug).  Stay DRV if already DRV,
                 * otherwise treat as walking since we can't confirm speed. */
                if (ctx->current_mode == MOBILITY_DRIVING) {
                    return MOBILITY_DRIVING;  /* Don't downgrade while vibrating */
                }
                return MOBILITY_WALKING;
            } else if (speed_walking && !speed_driving) {
                /* GPS says 1-10 kph — definitely not highway driving.
                 * AR says vehicle but at walking speed = treat as walking.
                 * This fixes: ON_BICYCLE(99%) at 5 kph keeping DRV for minutes */
                return MOBILITY_WALKING;
            } else {
                return MOBILITY_DRIVING;
            }
        }

        if (strcmp(pa->activity, "WALKING") == 0 ||
            strcmp(pa->activity, "RUNNING") == 0 ||
            strcmp(pa->activity, "ON_FOOT") == 0) {
            /* V3.7 Fix 3: Override AR=WALKING when HMA + GPS both confirm STILL.
             * Test 3 data: AR=WALKING(100%) while user stationary, HMA=0.06/0.12.
             * Phone sensor false-positive — trust HMA+GPS over AR alone. */
            if (accel_still && speed_stopped) {
                return MOBILITY_STATIONARY;  /* HMA+GPS override AR false-positive */
            }
            return MOBILITY_WALKING;
        }

        if (strcmp(pa->activity, "STILL") == 0) {
            if (!stepping && !accel_moving) {
                return MOBILITY_STATIONARY;
            }
            /* STILL but HMA says moving — check for HMA overshoot.
             * If raw accel is at noise floor, the HMA spike is phantom:
             * a single phone bump overshoots in HMA but raw already dropped.
             * Field data: dozens of ghost STA→WLK→STA overnight from this. */
            if (!stepping && ctx->accel_magnitude < MOB_ACCEL_STILL) {
                return MOBILITY_STATIONARY;  /* Trust STILL + low raw accel */
            }
            /* sensors genuinely contradict STILL → fall through */
        }
    }

    /* ---- 3. Step counter = definitive walking proof ---- */
    if (stepping) return MOBILITY_WALKING;

    /* ---- 4. GPS speed 1-10 km/h = WALKING ---- */
    /* Checked BEFORE accelerometer!  Car vibration produces accel 1-2 m/s²
     * which would trigger "accel_moving → WALKING" and block DRV transitions.
     * GPS speed is more reliable than accel for distinguishing WLK from STA
     * when you're actually moving (accel can't tell walking from car vibration). */
    if (speed_walking) return MOBILITY_WALKING;

    /* ---- 5. Accelerometer shows body motion (no GPS / GPS=0) ---- */
    /* Only reaches here when GPS speed is 0 or unavailable.
     * At this point, accel > 0.4 means body motion without GPS confirmation.
     * Most likely walking (Samsung GPS Doppler bug: reports 0 below 5 km/h).
     * Require raw accel >= STILL threshold as spike guard:
     * HMA overshoots on single-sample spikes, but raw accel confirms
     * the signal is real (not just HMA prediction artifact). */
    if (accel_moving && ctx->accel_magnitude >= MOB_ACCEL_STILL)
        return MOBILITY_WALKING;

    /* ---- 6. Default: stationary ---- */
    return MOBILITY_STATIONARY;
}


/* ============================================================================
 * Public API
 * ============================================================================ */

int mobility_mode_init(mobility_ctx_t *ctx) {
    if (!ctx) return -1;

    memset(ctx, 0, sizeof(*ctx));
    ctx->current_mode = MOBILITY_STATIONARY;
    ctx->pending_mode = MOBILITY_STATIONARY;
    ctx->pending_count = 0;
    ctx->session_start = time(NULL);
    ctx->mode_since = ctx->session_start;
    ctx->last_check = ctx->session_start;

    memcpy(&ctx->params, &MODE_PARAMS[MOBILITY_STATIONARY], sizeof(mobility_params_t));

    ctx->initialized = true;

    fprintf(stderr, "[mobility] V3 initialized: multi-signal fusion (accel+steps+activity+gps), mode=STATIONARY\n");
    return 0;
}

void mobility_mode_set_activity(mobility_ctx_t *ctx,
                                const char *activity,
                                int confidence) {
    if (!ctx || !activity) return;
    strncpy(ctx->phone_activity.activity, activity,
            sizeof(ctx->phone_activity.activity) - 1);
    ctx->phone_activity.activity[sizeof(ctx->phone_activity.activity) - 1] = '\0';
    ctx->phone_activity.confidence = confidence;
    ctx->phone_activity.updated = time(NULL);
}

void mobility_mode_set_calibration(mobility_ctx_t *ctx, const char *mode) {
    if (!ctx || !mode) return;
    strncpy(ctx->calibration, mode, sizeof(ctx->calibration) - 1);
    ctx->calibration[sizeof(ctx->calibration) - 1] = '\0';
}

bool mobility_mode_update(mobility_ctx_t *ctx,
                          float gps_speed,
                          float mob_score,
                          float ap_churn,
                          int total_aps,
                          float accel,
                          int steps) {
    if (!ctx || !ctx->initialized) return false;

    time_t now = time(NULL);

    /* Store raw inputs — Hull Moving Average on accelerometer.
     * HMA(4) at 2s sampling = 8s window.  Unlike EMA, HMA has zero lag:
     *   WMA(2) of raw  = fast response
     *   WMA(4) of raw  = slow baseline
     *   diff = 2*WMA(2) - WMA(4)  — removes lag, may overshoot
     *   HMA = WMA(2) of diffs — smooths overshoot
     * Responds equally fast to rising AND falling accel. No snap-down hacks. */
    /* GPS speed dropout persistence: if previous speed was driving-level
     * and current reads 0, hold the previous speed for 1 cycle.
     * Samsung GPS Doppler drops to 0 momentarily during speed fluctuations.
     * Calibration data: 2 mismatches (25%) caused by single-cycle GPS=0 drops. */
    if (gps_speed <= 0.0f && ctx->prev_gps_speed_kmh >= MOB_SPEED_DRIVING) {
        gps_speed = ctx->prev_gps_speed_kmh;  /* Hold previous for 1 cycle */
    }
    ctx->prev_gps_speed_kmh = ctx->gps_speed_kmh;
    ctx->gps_speed_kmh = gps_speed;

    /* Push raw accel into ring buffer */
    ctx->accel_ring[ctx->accel_ring_idx] = accel;
    ctx->accel_ring_idx = (ctx->accel_ring_idx + 1) % MOB_HMA_PERIOD;
    if (ctx->accel_ring_count < MOB_HMA_PERIOD)
        ctx->accel_ring_count++;

    /* Compute HMA once we have enough samples */
    if (ctx->accel_ring_count >= MOB_HMA_PERIOD) {
        /* WMA(2) of the 2 newest raw samples: (2*newest + 1*prev) / 3 */
        int i0 = (ctx->accel_ring_idx - 1 + MOB_HMA_PERIOD) % MOB_HMA_PERIOD;
        int i1 = (ctx->accel_ring_idx - 2 + MOB_HMA_PERIOD) % MOB_HMA_PERIOD;
        float wma_half = (2.0f * ctx->accel_ring[i0] + 1.0f * ctx->accel_ring[i1]) / 3.0f;

        /* WMA(4) of all 4 raw samples: (4*newest + 3*prev + 2*prev2 + 1*oldest) / 10 */
        int i2 = (ctx->accel_ring_idx - 3 + MOB_HMA_PERIOD) % MOB_HMA_PERIOD;
        int i3 = (ctx->accel_ring_idx - 4 + MOB_HMA_PERIOD) % MOB_HMA_PERIOD;
        float wma_full = (4.0f * ctx->accel_ring[i0] + 3.0f * ctx->accel_ring[i1] +
                          2.0f * ctx->accel_ring[i2] + 1.0f * ctx->accel_ring[i3]) / 10.0f;

        /* Diff series: removes lag (may overshoot slightly) */
        float diff = 2.0f * wma_half - wma_full;
        if (diff < 0.0f) diff = 0.0f; /* Accel RMS can't be negative */

        /* Push diff into diff ring buffer */
        ctx->accel_diff[ctx->accel_diff_idx] = diff;
        ctx->accel_diff_idx = (ctx->accel_diff_idx + 1) % MOB_HMA_SQRT;
        if (ctx->accel_diff_count < MOB_HMA_SQRT)
            ctx->accel_diff_count++;

        /* Final HMA = WMA(2) of diff series */
        if (ctx->accel_diff_count >= MOB_HMA_SQRT) {
            int d0 = (ctx->accel_diff_idx - 1 + MOB_HMA_SQRT) % MOB_HMA_SQRT;
            int d1 = (ctx->accel_diff_idx - 2 + MOB_HMA_SQRT) % MOB_HMA_SQRT;
            ctx->accel_hma = (2.0f * ctx->accel_diff[d0] + 1.0f * ctx->accel_diff[d1]) / 3.0f;
        } else {
            ctx->accel_hma = diff; /* Only 1 diff so far, use it directly */
        }
    } else {
        /* Not enough samples yet — use raw value */
        ctx->accel_hma = accel;
    }
    ctx->accel_magnitude = accel;

    /* V3.7 broadened HMA snap-down: relaxed from V3.6 strict conditions.
     * V3.6 required AR=STILL(95%+) AND speed=0 — too strict, only caught 0/9
     * true detection errors in test 3.  Problems:
     *   - AR=STILL at 45-71% confidence still means stationary
     *   - AR=TILTING while phone in pocket = stationary
     *   - GPS drift (speed=1.9) blocks even when truly stopped
     * New: (STILL(50%+) OR TILTING) AND speed < 2.0 → force HMA=0 */
    {
        const phone_activity_t *pa = &ctx->phone_activity;
        bool ar_fresh = (pa->updated > 0 &&
                         (now - pa->updated) < MOB_ACTIVITY_STALE_S);
        bool ar_stationary = ar_fresh && (
            (strcmp(pa->activity, "STILL") == 0 && pa->confidence >= 50) ||
            (strcmp(pa->activity, "TILTING") == 0)
        );
        if (ar_stationary && gps_speed < 2.0f) {
            ctx->accel_hma = 0.0f;
            /* Clear ring buffers to prevent HMA bouncing back */
            for (int i = 0; i < MOB_HMA_PERIOD; i++)
                ctx->accel_ring[i] = 0.0f;
            for (int i = 0; i < MOB_HMA_SQRT; i++)
                ctx->accel_diff[i] = 0.0f;
        }
    }

    /* Track last time step count increased (for active walking detection).
     * Steps from Android hardware pedometer — no false positives.
     * Only track CHANGES after first reading (step_count starts at 0,
     * so the first reading of e.g. steps=24 would falsely trigger). */
    if (ctx->prev_step_count > 0 && steps > ctx->step_count) {
        ctx->last_step_time = now;
    }
    ctx->prev_step_count = ctx->step_count;
    ctx->step_count = steps;
    ctx->last_check = now;

    /* Reject absurd GPS readings */
    if (gps_speed > MOB_SPEED_MAX_SANE) {
        fprintf(stderr, "[mobility] GPS speed %.1f km/h rejected (>%.0f max)\n",
                gps_speed, MOB_SPEED_MAX_SANE);
        gps_speed = -1.0f;
    }

    /* Classify using phone activity (primary) or raw GPS speed (fallback) */
    mobility_mode_t detected = classify(ctx, gps_speed);

    /* Step delta for logging */
    int step_delta = 0;
    if (ctx->prev_step_count > 0) {
        step_delta = steps - ctx->prev_step_count;
        if (step_delta < 0 || step_delta > 50) step_delta = 0;
    }

    /* Debug log every 5s — show ALL signal sources */
    {
        static time_t _last_mob_log = 0;
        if (now - _last_mob_log >= 5) {
            _last_mob_log = now;
            FILE *f = fopen("/tmp/mobility_debug.log", "a");
            if (f) {
                struct tm *tm = localtime(&now);
                char ts[32];
                strftime(ts, sizeof(ts), "%H:%M:%S", tm);
                static const char *mn[] = {"STA","WLK","DRV"};
                const phone_activity_t *pa = &ctx->phone_activity;
                int pa_age = pa->updated > 0 ? (int)(now - pa->updated) : -1;
                bool stepping = (ctx->last_step_time > 0 &&
                                 (now - ctx->last_step_time) < MOB_STEP_ACTIVE_S);
                const char *gt = ctx->calibration[0] ? ctx->calibration : "-";
                bool mismatch = (ctx->calibration[0] &&
                                 strcmp(ctx->calibration, mn[detected]) != 0);
                fprintf(f, "[%s] raw=%.1f hma=%.2f accel=%.2f steps=%d sd=%d stp=%s "
                        "act=%s(%d%% %ds) "
                        "cur=%s det=%s pend=%s cnt=%d gt=%s%s\n",
                        ts, gps_speed, ctx->accel_hma, accel, steps, step_delta,
                        stepping ? "Y" : "N",
                        pa->activity[0] ? pa->activity : "NONE",
                        pa->confidence, pa_age,
                        mn[ctx->current_mode],
                        mn[detected],
                        mn[ctx->pending_mode],
                        ctx->pending_count,
                        gt,
                        mismatch ? " !!MISMATCH!!" : "");
                fclose(f);
            }
        }
    }

    /* Same as current mode — reset hysteresis */
    if (detected == ctx->current_mode) {
        ctx->pending_mode = ctx->current_mode;
        ctx->pending_count = 0;
        return false;
    }

    /* Different: track hysteresis */
    if (detected == ctx->pending_mode) {
        ctx->pending_count++;
    } else {
        ctx->pending_mode = detected;
        ctx->pending_count = 1;
    }

    /* Check if we've reached the threshold */
    if (ctx->pending_count >= MOB_HYSTERESIS) {
        /* Cooldown check */
        if ((now - ctx->mode_since) < MOB_SWITCH_COOLDOWN_S) {
            return false;
        }

        /* MODE SWITCH */
        mobility_mode_t old_mode = ctx->current_mode;
        ctx->time_in_mode[old_mode] += (now - ctx->mode_since);
        ctx->current_mode = detected;
        ctx->mode_since = now;
        ctx->pending_count = 0;
        ctx->switches++;

        memcpy(&ctx->params, &MODE_PARAMS[detected], sizeof(mobility_params_t));

        const phone_activity_t *pa = &ctx->phone_activity;
        fprintf(stderr, "[mobility] MODE SWITCH: %s -> %s "
                "(speed=%.1f act=%s(%d%%) switch #%u)\n",
                mobility_mode_name(old_mode),
                mobility_mode_name(detected),
                gps_speed,
                pa->activity[0] ? pa->activity : "NONE",
                pa->confidence,
                ctx->switches);

        return true;
    }

    return false;
}

mobility_mode_t mobility_mode_get(const mobility_ctx_t *ctx) {
    if (!ctx || !ctx->initialized) return MOBILITY_STATIONARY;
    return ctx->current_mode;
}

const mobility_params_t *mobility_mode_get_params(const mobility_ctx_t *ctx) {
    if (!ctx || !ctx->initialized) return &MODE_PARAMS[MOBILITY_STATIONARY];
    return &ctx->params;
}

void mobility_mode_get_mode_params(mobility_mode_t mode, mobility_params_t *out) {
    if (!out) return;
    if (mode < 0 || mode >= MOBILITY_MODE_COUNT) mode = MOBILITY_STATIONARY;
    memcpy(out, &MODE_PARAMS[mode], sizeof(mobility_params_t));
}

const char *mobility_mode_label(mobility_mode_t mode) {
    switch (mode) {
        case MOBILITY_STATIONARY: return MOB_LABEL_STATIONARY;
        case MOBILITY_WALKING:    return MOB_LABEL_WALKING;
        case MOBILITY_DRIVING:    return MOB_LABEL_DRIVING;
        default:                  return "???";
    }
}

const char *mobility_mode_name(mobility_mode_t mode) {
    switch (mode) {
        case MOBILITY_STATIONARY: return "STATIONARY";
        case MOBILITY_WALKING:    return "WALKING";
        case MOBILITY_DRIVING:    return "DRIVING";
        default:                  return "UNKNOWN";
    }
}

void mobility_mode_status_str(const mobility_ctx_t *ctx, char *buf, size_t len) {
    if (!ctx || !buf || len == 0) return;

    int in_mode_s = mobility_mode_time_in_current(ctx);
    int mins = in_mode_s / 60;
    int secs = in_mode_s % 60;

    snprintf(buf, len,
             "MOB[%s] spd=%.1fkm/h act=%s(%d%%) (%dm%ds) sw=%u",
             mobility_mode_label(ctx->current_mode),
             ctx->gps_speed_kmh,
             ctx->phone_activity.activity[0] ? ctx->phone_activity.activity : "NONE",
             ctx->phone_activity.confidence,
             mins, secs,
             ctx->switches);
}

int mobility_mode_time_in_current(const mobility_ctx_t *ctx) {
    if (!ctx || !ctx->initialized) return 0;
    return (int)(time(NULL) - ctx->mode_since);
}
