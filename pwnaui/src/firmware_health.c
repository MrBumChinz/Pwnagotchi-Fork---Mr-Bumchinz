/* ============================================================================
 * firmware_health.c - BCM43438 Firmware Health & Injection Rate Limiter
 *
 * Phase 1B: Dynamic rate limiting for raw frame injection.
 *
 * The BCM43438 firmware crashes when hammered with too many injection frames.
 * This module provides:
 *   - Per-second rate limiting (adaptive: backs off on failures, ramps on success)
 *   - Pre-emptive cooldown scheduling (every N frames or M minutes)
 *   - Interface reset on catastrophic failure (ifconfig down/up cycle)
 *   - Logging to /tmp/fw_health.log for post-session diagnostics
 *
 * Copyright (c) 2026. All rights reserved.
 * ============================================================================ */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

#include "firmware_health.h"

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

static void fw_log(fw_health_t *fwh, const char *fmt, ...) {
    if (!fwh || !fwh->log_fp) return;

    /* Rotate if too large */
    long pos = ftell(fwh->log_fp);
    if (pos > FW_HEALTH_LOG_MAX) {
        fclose(fwh->log_fp);
        /* Rename old log */
        char old_path[128];
        snprintf(old_path, sizeof(old_path), "%s.old", FW_HEALTH_LOG_PATH);
        rename(FW_HEALTH_LOG_PATH, old_path);
        fwh->log_fp = fopen(FW_HEALTH_LOG_PATH, "a");
        if (!fwh->log_fp) return;
    }

    /* Timestamp */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    fprintf(fwh->log_fp, "[%04d-%02d-%02d %02d:%02d:%02d] ",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec);

    /* Message */
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fwh->log_fp, fmt, ap);
    va_end(ap);

    fprintf(fwh->log_fp, "\n");
    fflush(fwh->log_fp);
}

/* Bring interface down, wait, bring back up */
static int fw_reset_interface(fw_health_t *fwh) {
    char cmd[128];

    fw_log(fwh, "RESET: bringing %s down for %ds",
           fwh->iface, FW_HEALTH_COOLDOWN_DURATION_S);
    fprintf(stderr, "[fw_health] RESET: %s down for %ds\n",
            fwh->iface, FW_HEALTH_COOLDOWN_DURATION_S);

    /* ifconfig down */
    snprintf(cmd, sizeof(cmd), "ifconfig %s down 2>/dev/null", fwh->iface);
    int ret = system(cmd);
    if (ret != 0) {
        /* Try ip link as fallback */
        snprintf(cmd, sizeof(cmd), "ip link set %s down 2>/dev/null", fwh->iface);
        ret = system(cmd);
    }

    /* Wait */
    sleep(FW_HEALTH_COOLDOWN_DURATION_S);

    /* ifconfig up */
    snprintf(cmd, sizeof(cmd), "ifconfig %s up 2>/dev/null", fwh->iface);
    ret = system(cmd);
    if (ret != 0) {
        snprintf(cmd, sizeof(cmd), "ip link set %s up 2>/dev/null", fwh->iface);
        ret = system(cmd);
    }

    fw_log(fwh, "RESET: %s back up (ret=%d)", fwh->iface, ret);
    fprintf(stderr, "[fw_health] RESET: %s back up\n", fwh->iface);

    return (ret == 0) ? 0 : -1;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int fw_health_init(fw_health_t *fwh, const char *iface) {
    if (!fwh || !iface) return -1;

    memset(fwh, 0, sizeof(*fwh));

    /* Interface name */
    size_t ilen = strlen(iface);
    if (ilen >= sizeof(fwh->iface)) ilen = sizeof(fwh->iface) - 1;
    memcpy(fwh->iface, iface, ilen);
    fwh->iface[ilen] = '\0';

    /* Defaults */
    fwh->max_fps = FW_HEALTH_DEFAULT_MAX_FPS;
    fwh->state = FW_STATE_HEALTHY;
    fwh->success_rate = 1.0f;
    fwh->current_second = time(NULL);

    /* Open log file */
    fwh->log_fp = fopen(FW_HEALTH_LOG_PATH, "a");
    if (!fwh->log_fp) {
        fprintf(stderr, "[fw_health] WARNING: cannot open %s: %s\n",
                FW_HEALTH_LOG_PATH, strerror(errno));
        /* Non-fatal — we can operate without logging */
    }

    /* Mutex */
    pthread_mutex_init(&fwh->lock, NULL);
    fwh->initialized = true;

    fw_log(fwh, "INIT: iface=%s max_fps=%d", fwh->iface, fwh->max_fps);
    fprintf(stderr, "[fw_health] Initialized: %s, max %d fps\n",
            fwh->iface, fwh->max_fps);

    return 0;
}

bool fw_health_can_inject(fw_health_t *fwh) {
    if (!fwh || !fwh->initialized) return true;  /* Fail-open if not initialized */

    pthread_mutex_lock(&fwh->lock);

    /* Check cooldown period */
    if (fwh->cooldown_until > 0) {
        time_t now = time(NULL);
        if (now < fwh->cooldown_until) {
            fwh->total_throttled++;
            pthread_mutex_unlock(&fwh->lock);
            return false;
        }
        /* Cooldown expired */
        fwh->cooldown_until = 0;
        if (fwh->state == FW_STATE_COOLDOWN) {
            fwh->state = FW_STATE_HEALTHY;
            fw_log(fwh, "COOLDOWN: expired, resuming injection");
        }
    }

    /* Check if in reset state */
    if (fwh->state == FW_STATE_RESETTING || fwh->state == FW_STATE_FAILED) {
        fwh->total_throttled++;
        pthread_mutex_unlock(&fwh->lock);
        return false;
    }

    /* Per-second rate limit */
    time_t now = time(NULL);
    if (now != fwh->current_second) {
        /* New second window */
        fwh->frames_this_second = 0;
        fwh->current_second = now;
    }

    if ((int)fwh->frames_this_second >= fwh->max_fps) {
        fwh->total_throttled++;
        pthread_mutex_unlock(&fwh->lock);
        return false;
    }

    /* Allowed */
    fwh->frames_this_second++;
    pthread_mutex_unlock(&fwh->lock);
    return true;
}

void fw_health_report_success(fw_health_t *fwh) {
    if (!fwh || !fwh->initialized) return;

    pthread_mutex_lock(&fwh->lock);

    fwh->total_injected++;
    fwh->frames_since_cooldown++;
    fwh->consecutive_successes++;
    fwh->consecutive_failures = 0;

    /* Update rolling success rate (EMA with alpha=0.05) */
    fwh->success_rate = fwh->success_rate * 0.95f + 0.05f;

    /* Ramp up if sustained success */
    if (fwh->consecutive_successes >= FW_HEALTH_RAMP_THRESHOLD) {
        fwh->consecutive_successes = 0;
        if (fwh->max_fps < FW_HEALTH_MAX_FPS) {
            int old_fps = fwh->max_fps;
            fwh->max_fps += FW_HEALTH_RAMP_INCREMENT;
            if (fwh->max_fps > FW_HEALTH_MAX_FPS) fwh->max_fps = FW_HEALTH_MAX_FPS;
            if (fwh->max_fps != old_fps) {
                fw_log(fwh, "RAMP: fps %d -> %d (after %d consecutive OK)",
                       old_fps, fwh->max_fps, FW_HEALTH_RAMP_THRESHOLD);
            }
        }
        /* Return to healthy if we were degraded */
        if (fwh->state == FW_STATE_DEGRADED) {
            fwh->state = FW_STATE_HEALTHY;
            fw_log(fwh, "STATE: DEGRADED -> HEALTHY (sustained success)");
        }
    }

    pthread_mutex_unlock(&fwh->lock);
}

void fw_health_report_failure(fw_health_t *fwh) {
    if (!fwh || !fwh->initialized) return;

    pthread_mutex_lock(&fwh->lock);

    fwh->total_failed++;
    fwh->consecutive_failures++;
    fwh->consecutive_successes = 0;

    /* Update rolling success rate */
    fwh->success_rate = fwh->success_rate * 0.95f;

    /* Check escalation thresholds */
    if (fwh->consecutive_failures >= FW_HEALTH_FAIL_RESET) {
        /* Catastrophic — force interface reset */
        fw_log(fwh, "CRITICAL: %d consecutive failures — triggering reset",
               fwh->consecutive_failures);
        fprintf(stderr, "[fw_health] CRITICAL: %d consecutive failures — resetting %s\n",
                fwh->consecutive_failures, fwh->iface);

        fwh->state = FW_STATE_RESETTING;
        fwh->consecutive_failures = 0;
        fwh->resets_triggered++;
        fwh->last_reset = time(NULL);

        /* Release lock during reset (blocking I/O) */
        pthread_mutex_unlock(&fwh->lock);
        fw_reset_interface(fwh);

        pthread_mutex_lock(&fwh->lock);
        fwh->state = FW_STATE_DEGRADED;
        fwh->max_fps = FW_HEALTH_MIN_FPS;  /* Start slow after reset */
        fwh->frames_since_cooldown = 0;
        fw_log(fwh, "POST-RESET: fps set to %d", fwh->max_fps);
        pthread_mutex_unlock(&fwh->lock);
        return;

    } else if (fwh->consecutive_failures >= 2) {
        /* Back off — halve the rate */
        int old_fps = fwh->max_fps;
        fwh->max_fps /= FW_HEALTH_BACKOFF_DIVISOR;
        if (fwh->max_fps < FW_HEALTH_MIN_FPS) fwh->max_fps = FW_HEALTH_MIN_FPS;
        if (fwh->state != FW_STATE_DEGRADED) {
            fwh->state = FW_STATE_DEGRADED;
            fw_log(fwh, "STATE: -> DEGRADED (consecutive failures=%d)",
                   fwh->consecutive_failures);
        }
        if (fwh->max_fps != old_fps) {
            fw_log(fwh, "BACKOFF: fps %d -> %d (consec_fail=%d)",
                   old_fps, fwh->max_fps, fwh->consecutive_failures);
            fprintf(stderr, "[fw_health] BACKOFF: fps %d -> %d\n",
                    old_fps, fwh->max_fps);
        }
    }

    pthread_mutex_unlock(&fwh->lock);
}

bool fw_health_epoch_tick(fw_health_t *fwh) {
    if (!fwh || !fwh->initialized) return false;

    pthread_mutex_lock(&fwh->lock);

    time_t now = time(NULL);
    bool scheduled = false;

    /* Pre-emptive cooldown: if injected enough frames AND enough time elapsed */
    if (fwh->frames_since_cooldown >= FW_HEALTH_FRAMES_BEFORE_COOLDOWN &&
        (now - fwh->last_cooldown) >= FW_HEALTH_COOLDOWN_INTERVAL_S) {

        fwh->state = FW_STATE_COOLDOWN;
        fwh->cooldown_until = now + FW_HEALTH_COOLDOWN_DURATION_S;
        fwh->last_cooldown = now;
        fwh->cooldowns_triggered++;
        fwh->frames_since_cooldown = 0;
        scheduled = true;

        fw_log(fwh, "COOLDOWN: scheduled %ds rest (after %llu frames)",
               FW_HEALTH_COOLDOWN_DURATION_S,
               (unsigned long long)fwh->frames_since_cooldown);
        fprintf(stderr, "[fw_health] Pre-emptive cooldown: %ds rest\n",
                FW_HEALTH_COOLDOWN_DURATION_S);
    }

    /* Log periodic stats */
    fw_log(fwh, "TICK: state=%s fps=%d injected=%llu failed=%llu throttled=%llu rate=%.2f%%",
           fw_health_state_name(fwh->state),
           fwh->max_fps,
           (unsigned long long)fwh->total_injected,
           (unsigned long long)fwh->total_failed,
           (unsigned long long)fwh->total_throttled,
           fwh->success_rate * 100.0f);

    pthread_mutex_unlock(&fwh->lock);
    return scheduled;
}

int fw_health_force_reset(fw_health_t *fwh) {
    if (!fwh || !fwh->initialized) return -1;

    pthread_mutex_lock(&fwh->lock);
    fwh->state = FW_STATE_RESETTING;
    fwh->resets_triggered++;
    fwh->last_reset = time(NULL);
    pthread_mutex_unlock(&fwh->lock);

    int ret = fw_reset_interface(fwh);

    pthread_mutex_lock(&fwh->lock);
    fwh->state = (ret == 0) ? FW_STATE_DEGRADED : FW_STATE_FAILED;
    fwh->max_fps = FW_HEALTH_MIN_FPS;
    fwh->consecutive_failures = 0;
    fwh->consecutive_successes = 0;
    fwh->frames_since_cooldown = 0;
    pthread_mutex_unlock(&fwh->lock);

    return ret;
}

bool fw_health_in_cooldown(fw_health_t *fwh) {
    if (!fwh || !fwh->initialized) return false;

    pthread_mutex_lock(&fwh->lock);
    bool cooling = (fwh->state == FW_STATE_COOLDOWN ||
                    fwh->state == FW_STATE_RESETTING);
    if (fwh->cooldown_until > 0 && time(NULL) >= fwh->cooldown_until) {
        fwh->cooldown_until = 0;
        if (fwh->state == FW_STATE_COOLDOWN) fwh->state = FW_STATE_HEALTHY;
        cooling = false;
    }
    pthread_mutex_unlock(&fwh->lock);
    return cooling;
}

void fw_health_get_stats(fw_health_t *fwh, fw_health_stats_t *stats) {
    if (!fwh || !stats) return;

    pthread_mutex_lock(&fwh->lock);
    stats->total_injected = fwh->total_injected;
    stats->total_failed = fwh->total_failed;
    stats->total_throttled = fwh->total_throttled;
    stats->resets_triggered = fwh->resets_triggered;
    stats->cooldowns_triggered = fwh->cooldowns_triggered;
    stats->current_max_fps = fwh->max_fps;
    stats->state = fwh->state;
    stats->success_rate = fwh->success_rate;
    stats->last_reset = fwh->last_reset;
    stats->last_cooldown = fwh->last_cooldown;
    pthread_mutex_unlock(&fwh->lock);
}

void fw_health_status_str(fw_health_t *fwh, char *buf, size_t len) {
    if (!fwh || !buf || len == 0) return;

    pthread_mutex_lock(&fwh->lock);
    snprintf(buf, len,
             "FW[%s] fps=%d ok=%llu fail=%llu throt=%llu rate=%.0f%% rst=%u cd=%u",
             fw_health_state_name(fwh->state),
             fwh->max_fps,
             (unsigned long long)fwh->total_injected,
             (unsigned long long)fwh->total_failed,
             (unsigned long long)fwh->total_throttled,
             fwh->success_rate * 100.0f,
             fwh->resets_triggered,
             fwh->cooldowns_triggered);
    pthread_mutex_unlock(&fwh->lock);
}

const char *fw_health_state_name(fw_health_state_t state) {
    switch (state) {
        case FW_STATE_HEALTHY:   return "HEALTHY";
        case FW_STATE_DEGRADED:  return "DEGRADED";
        case FW_STATE_COOLDOWN:  return "COOLDOWN";
        case FW_STATE_RESETTING: return "RESETTING";
        case FW_STATE_FAILED:    return "FAILED";
        default:                 return "UNKNOWN";
    }
}

void fw_health_destroy(fw_health_t *fwh) {
    if (!fwh || !fwh->initialized) return;

    pthread_mutex_lock(&fwh->lock);
    fw_log(fwh, "SHUTDOWN: injected=%llu failed=%llu throttled=%llu resets=%u",
           (unsigned long long)fwh->total_injected,
           (unsigned long long)fwh->total_failed,
           (unsigned long long)fwh->total_throttled,
           fwh->resets_triggered);

    if (fwh->log_fp) {
        fclose(fwh->log_fp);
        fwh->log_fp = NULL;
    }
    fwh->initialized = false;
    pthread_mutex_unlock(&fwh->lock);
    pthread_mutex_destroy(&fwh->lock);
}
