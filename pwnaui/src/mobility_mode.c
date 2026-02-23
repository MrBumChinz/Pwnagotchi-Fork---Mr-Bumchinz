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
 * GPS speed is primary signal; AP churn is fallback when speed is 0 (no fix).
 */
static mobility_mode_t classify(float gps_speed, float mob_score, float ap_churn) {
    /* GPS speed >= 0 means we have a GPS fix.
     * GPS speed < 0 means no GPS fix -- use AP churn as fallback.
     * When GPS says stationary (speed=0), trust it over AP churn. */

    if (gps_speed >= 0.0f) {
        /* We have GPS -- it's authoritative */
        if (gps_speed > MOB_SPEED_DRIVING) {
            return MOBILITY_DRIVING;
        }
        if (gps_speed > MOB_SPEED_WALKING) {
            return MOBILITY_WALKING;
        }
        /* GPS says not moving fast enough for walking */
        return MOBILITY_STATIONARY;
    }

    /* No GPS fix -- fall back to AP churn / mobility_score.
     * These are much noisier, so use higher thresholds. */
    if (ap_churn > MOB_CHURN_DRIVING && mob_score > 0.5f) {
        return MOBILITY_DRIVING;
    }
    if (ap_churn > MOB_CHURN_WALKING && mob_score > 0.2f) {
        return MOBILITY_WALKING;
    }
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
                          int total_aps) {
    if (!ctx || !ctx->initialized) return false;

    time_t now = time(NULL);

    /* Store inputs */
    ctx->gps_speed_kmh = gps_speed;
    ctx->mobility_score = mob_score;
    ctx->ap_churn_rate = ap_churn;
    ctx->total_aps = total_aps;
    ctx->last_check = now;

    /* Classify current reading */
    mobility_mode_t detected = classify(gps_speed, mob_score, ap_churn);

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
                fprintf(f, "[%s] spd=%.1f mob=%.2f churn=%.2f aps=%d "
                        "cur=%s det=%s pend=%s cnt=%d\n",
                        ts, gps_speed, mob_score, ap_churn, total_aps,
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

        /* Adaptive hysteresis based on signal clarity.
     * When GPS clearly confirms the mode, switch instantly.
     * When near threshold boundaries or no GPS, require more readings. */
    int threshold;
    if (gps_speed < 0.0f) {
        /* No GPS fix -- using noisy AP churn fallback, need more readings */
        threshold = MOB_HYSTERESIS_COUNT;
    } else if (gps_speed > MOB_SPEED_DRIVING + 5.0f) {
        /* Clearly driving (>20 km/h) -- instant */
        threshold = 1;
    } else if (gps_speed > MOB_SPEED_WALKING + 1.5f && gps_speed < MOB_SPEED_DRIVING - 3.0f) {
        /* Clearly walking (3-12 km/h) -- instant */
        threshold = 1;
    } else if (gps_speed < 0.3f) {
        /* GPS says basically zero -- instant stationary */
        threshold = 1;
    } else {
        /* GPS near a threshold boundary -- 2 readings for safety */
        threshold = MOB_GPS_HYSTERESIS;
    }
    if (ctx->pending_count >= threshold) {
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
