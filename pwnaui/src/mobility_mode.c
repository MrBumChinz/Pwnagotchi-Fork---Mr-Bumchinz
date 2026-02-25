/* ============================================================================
 * mobility_mode.c - Driving / Walking / Stationary Mode Detection
 *
 * Phase 1C: Automatic mobility-aware parameter adaptation.
 *
 * Classification uses GPS speed (primary) + AP churn (fallback when no GPS).
 * Hysteresis prevents flapping between modes.
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
 * Internal helpers
 * ============================================================================ */

/**
 * Classify inputs into a mobility mode.
 *
 * GPS Doppler speed is the PRIMARY and AUTHORITATIVE signal.
 * When GPS has a fix (or holdover from recent fix), speed determines mode:
 *   - speed > 10 = DRIVING
 *   - speed > 1.5 = WALKING 
 *   - speed <= 1.5 = STATIONARY (churn CANNOT override)
 *
 * AP churn is ONLY used when there is NO GPS fix AND no holdover.
 */
static mobility_mode_t classify(float gps_speed, float mob_score, float ap_churn, int total_aps, float accel) {
    /* GPS Doppler is available (>= 0) — trust it absolutely */
    if (gps_speed >= 0.0f) {
        if (gps_speed > MOB_SPEED_DRIVING) return MOBILITY_DRIVING;
        if (gps_speed > MOB_SPEED_WALKING) return MOBILITY_WALKING;

        /* GPS says < 1.5 km/h but accelerometer shows motion?
         * This happens when Samsung Doppler reports 0 at walking speed.
         * Trust the accelerometer as a tiebreaker. */
        if (accel > MOB_ACCEL_DRIVING) return MOBILITY_DRIVING;
        if (accel > MOB_ACCEL_WALKING) return MOBILITY_WALKING;

        return MOBILITY_STATIONARY;
    }

    /* No GPS fix (speed < 0). Check accelerometer first, then AP churn. */
    if (accel > MOB_ACCEL_DRIVING) return MOBILITY_DRIVING;
    if (accel > MOB_ACCEL_WALKING) return MOBILITY_WALKING;

    /* AP churn is the fallback signal.
     * Require minimum AP count to avoid noise from 2-3 APs. */
    if (total_aps >= MOB_MIN_APS_FOR_CHURN) {
        if (ap_churn > MOB_CHURN_DRIVING && mob_score > 0.5f) {
            return MOBILITY_DRIVING;
        }
        if (ap_churn > MOB_CHURN_WALKING && mob_score > 0.15f) {
            return MOBILITY_WALKING;
        }
    }

    /* No GPS, no accel, insufficient APs for churn — assume stationary */
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
    ctx->smoothed_speed = -1.0f;  /* No valid GPS yet */
    ctx->last_valid_gps = 0;       /* No GPS received yet */
    ctx->session_start = time(NULL);
    ctx->mode_since = ctx->session_start;
    ctx->last_check = ctx->session_start;

    /* Load stationary parameters as default */
    memcpy(&ctx->params, &MODE_PARAMS[MOBILITY_STATIONARY], sizeof(mobility_params_t));

    ctx->initialized = true;

    fprintf(stderr, "[mobility] Initialized: mode=STATIONARY\n");
    return 0;
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

    /* Store inputs */
    ctx->gps_speed_kmh = gps_speed;
    ctx->mobility_score = mob_score;
    ctx->ap_churn_rate = ap_churn;
    ctx->total_aps = total_aps;
    ctx->accel_magnitude = accel;
    ctx->prev_step_count = ctx->step_count;
    ctx->step_count = steps;
    ctx->last_check = now;

    /* GPS speed sanity clamp: reject absurd readings as noise */
    if (gps_speed > MOB_SPEED_MAX_SANE) {
        fprintf(stderr, "[mobility] GPS speed %.1f km/h rejected (>%.0f max sane)\n",
                gps_speed, MOB_SPEED_MAX_SANE);
        gps_speed = -1.0f;  /* Treat as no-GPS for this reading */
    }

    /* Asymmetric EMA speed smoothing + GPS holdover:
     * - Upward (accelerating): use MOB_SPEED_SMOOTH_ALPHA (0.9) — fast but dampens spikes
     * - Downward (decelerating): snap immediately to raw speed.
     * - Zero-speed suspicious: when smoothed was above walking pace but raw is
     *   exactly 0.0, use MOB_SPEED_ZERO_ALPHA (0.3) — Samsung Doppler reports 0 at
     *   walking/low driving speeds even when moving.  Full EMA would kill mode
     *   in 1 reading (0.9*0=0). Slow alpha decays over ~3-5 readings instead.
     * - GPS dropout (raw=-1.0): use last smoothed speed for up to 120s,
     *   decaying 15% per reading to gradually return to stationary.
     *   BT GPS drops for 10-100s regularly; without holdover, every dropout
     *   causes WLK->STA->WLK oscillation. */
    float smoothed = gps_speed;
    if (gps_speed >= 0.0f) {
        ctx->last_valid_gps = now;
        if (ctx->smoothed_speed < 0.0f) {
            /* First valid GPS reading — seed EMA */
            ctx->smoothed_speed = gps_speed;
        } else if (gps_speed < 0.01f && ctx->smoothed_speed > MOB_SPEED_WALKING) {
            /* Suspicious zero: speed was above walking pace but raw jumped to 0.
             * Samsung Doppler often reports 0 at walking/low driving speeds.
             * Use slow alpha so mode survives 3-5 spurious zeros. */
            ctx->smoothed_speed = MOB_SPEED_ZERO_ALPHA * gps_speed
                                + (1.0f - MOB_SPEED_ZERO_ALPHA) * ctx->smoothed_speed;
        } else if (gps_speed < ctx->smoothed_speed * 0.5f) {
            /* Speed dropped by >50% — user stopped or braked hard.
             * Snap to raw immediately, don't ghost. */
            ctx->smoothed_speed = gps_speed;
        } else {
            ctx->smoothed_speed = MOB_SPEED_SMOOTH_ALPHA * gps_speed
                                + (1.0f - MOB_SPEED_SMOOTH_ALPHA) * ctx->smoothed_speed;
        }
        smoothed = ctx->smoothed_speed;
    } else if (ctx->smoothed_speed >= 0.0f && ctx->last_valid_gps > 0 &&
               (now - ctx->last_valid_gps) < MOB_GPS_HOLDOVER_S) {
        /* GPS dropped but we had a recent fix — use holdover.
         * Decay smoothed speed 15% per call so we don't hold a stale
         * high speed forever if GPS truly dies. */
        ctx->smoothed_speed *= 0.85f;
        if (ctx->smoothed_speed < 0.5f) ctx->smoothed_speed = 0.0f;
        smoothed = ctx->smoothed_speed;
    } else {
        /* No GPS fix and no recent holdover — truly no GPS */
        smoothed = -1.0f;
        ctx->smoothed_speed = -1.0f;
    }

    /* Use smoothed speed for classification, not raw */
    mobility_mode_t detected = classify(smoothed, mob_score, ap_churn, total_aps, accel);

    /* Debug log every update for field diagnosis */
    {
        static time_t _last_mob_log = 0;
        if (now - _last_mob_log >= 5) {  /* Log every 5s to avoid spam */
            _last_mob_log = now;
            FILE *f = fopen("/tmp/mobility_debug.log", "a");
            if (f) {
                struct tm *tm = localtime(&now);
                char ts[32];
                strftime(ts, sizeof(ts), "%H:%M:%S", tm);
                static const char *mode_names[] = {"STA","WLK","DRV"};
                fprintf(f, "[%s] raw=%.1f smooth=%.1f mob=%.2f churn=%.2f aps=%d "
                        "accel=%.2f steps=%d "
                        "cur=%s det=%s pend=%s cnt=%d\n",
                        ts, gps_speed, ctx->smoothed_speed >= 0 ? ctx->smoothed_speed : -1.0f, mob_score, ap_churn, total_aps,
                        accel, steps,
                        mode_names[ctx->current_mode],
                        mode_names[detected],
                        mode_names[ctx->pending_mode],
                        ctx->pending_count);
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

    /* Different from current — track hysteresis */
    if (detected == ctx->pending_mode) {
        ctx->pending_count++;
    } else {
        /* New candidate — start counting */
        ctx->pending_mode = detected;
        ctx->pending_count = 1;
    }

            /* Hysteresis: GPS Doppler speed is authoritative and arrives at 1Hz.
     * With EMA alpha=0.9, smoothed speed tracks within 1 reading.
     * Use GPS_HYSTERESIS (1) for GPS-based switches (instant).
     * Use HYSTERESIS_COUNT (3) for AP-churn-only fallback which is noisy. */
    int threshold;
    if (gps_speed < 0.0f) {
        /* No GPS fix -- AP churn fallback is noisy */
        threshold = MOB_HYSTERESIS_COUNT;
    } else {
        /* GPS Doppler present -- trust it immediately */
        threshold = MOB_GPS_HYSTERESIS;
    }
    if (ctx->pending_count >= threshold) {
        /* Cooldown: don't switch again within MOB_SWITCH_COOLDOWN_S seconds.
         * Prevents rapid flapping when GPS noise alternates readings. */
        if ((now - ctx->mode_since) < MOB_SWITCH_COOLDOWN_S) {
            /* Too soon after last switch — suppress */
            return false;
        }

        /* MODE SWITCH */
        mobility_mode_t old_mode = ctx->current_mode;

        /* Accumulate time in old mode */
        ctx->time_in_mode[old_mode] += (now - ctx->mode_since);

        /* Switch */
        ctx->current_mode = detected;
        ctx->mode_since = now;
        ctx->pending_count = 0;
        ctx->switches++;

        /* Load new parameters */
        memcpy(&ctx->params, &MODE_PARAMS[detected], sizeof(mobility_params_t));

        fprintf(stderr, "[mobility] MODE SWITCH: %s -> %s "
                "(speed=%.1f km/h, score=%.2f, churn=%.2f, aps=%d, switch #%u)\n",
                mobility_mode_name(old_mode),
                mobility_mode_name(detected),
                gps_speed, mob_score, ap_churn, total_aps,
                ctx->switches);

        return true;  /* Caller should apply new params */
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
             "MOB[%s] spd=%.1fkm/h score=%.2f churn=%.2f aps=%d (%dm%ds) sw=%u",
             mobility_mode_label(ctx->current_mode),
             ctx->gps_speed_kmh,
             ctx->mobility_score,
             ctx->ap_churn_rate,
             ctx->total_aps,
             mins, secs,
             ctx->switches);
}

int mobility_mode_time_in_current(const mobility_ctx_t *ctx) {
    if (!ctx || !ctx->initialized) return 0;
    return (int)(time(NULL) - ctx->mode_since);
}
