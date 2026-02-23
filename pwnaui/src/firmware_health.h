/* ============================================================================
 * firmware_health.h - BCM43438 Firmware Health & Injection Rate Limiter
 *
 * Phase 1B: Prevents firmware crashes from over-injection.
 *
 * The BCM43438 Wi-Fi chip on the Pi Zero W is notorious for crashing when
 * flooded with raw injection frames.  This module wraps attack_raw_send()
 * with a dynamic rate limiter that:
 *
 *   1. Tracks frames-per-second and backs off on consecutive failures
 *   2. Schedules pre-emptive interface cooldowns after sustained injection
 *   3. Logs all firmware events to /tmp/fw_health.log
 *   4. Can trigger wifi_recovery on catastrophic failure
 *
 * Usage:
 *   fw_health_init(&fwh, "wlan0mon");
 *   if (fw_health_can_inject(&fwh)) {
 *       int ret = attack_raw_send(sock, frame, len);
 *       fw_health_report(ret > 0 ? true : false);
 *   }
 *   // In epoch end:
 *   fw_health_epoch_tick(&fwh);
 *   // On shutdown:
 *   fw_health_destroy(&fwh);
 *
 * Copyright (c) 2026. All rights reserved.
 * ============================================================================ */

#ifndef FIRMWARE_HEALTH_H
#define FIRMWARE_HEALTH_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

/* Rate limiting */
#define FW_HEALTH_DEFAULT_MAX_FPS   30   /* Initial max frames per second */
#define FW_HEALTH_MIN_FPS            5   /* Floor — never go below this */
#define FW_HEALTH_MAX_FPS           50   /* Ceiling — never exceed this */
#define FW_HEALTH_BACKOFF_DIVISOR    2   /* Halve on consecutive failures */
#define FW_HEALTH_RAMP_INCREMENT     5   /* Increase by 5 on sustained success */
#define FW_HEALTH_RAMP_THRESHOLD    10   /* Consecutive successes before ramp */
#define FW_HEALTH_FAIL_RESET         3   /* Consecutive failures → force reset */

/* Pre-emptive cooldown */
#define FW_HEALTH_FRAMES_BEFORE_COOLDOWN  1000  /* Total frames → schedule rest */
#define FW_HEALTH_COOLDOWN_INTERVAL_S     1800  /* 30 minutes between cooldowns */
#define FW_HEALTH_COOLDOWN_DURATION_S     5     /* 5 second interface rest */

/* Logging */
#define FW_HEALTH_LOG_PATH  "/tmp/fw_health.log"
#define FW_HEALTH_LOG_MAX   (256 * 1024)  /* Rotate at 256KB */

/* ============================================================================
 * Types
 * ============================================================================ */

/* Firmware health state — overall chip status */
typedef enum {
    FW_STATE_HEALTHY,       /* Normal operation */
    FW_STATE_DEGRADED,      /* Rate limited due to failures */
    FW_STATE_COOLDOWN,      /* Pre-emptive interface rest in progress */
    FW_STATE_RESETTING,     /* wifi_recovery in progress */
    FW_STATE_FAILED         /* Chip unresponsive — needs reboot */
} fw_health_state_t;

/* Statistics snapshot */
typedef struct {
    uint64_t total_injected;        /* Lifetime successful injections */
    uint64_t total_failed;          /* Lifetime failed injections */
    uint64_t total_throttled;       /* Frames skipped by rate limiter */
    uint32_t resets_triggered;      /* Number of interface resets */
    uint32_t cooldowns_triggered;   /* Number of pre-emptive cooldowns */
    int      current_max_fps;       /* Current dynamic FPS limit */
    fw_health_state_t state;        /* Current state */
    float    success_rate;          /* Rolling success rate (0.0-1.0) */
    time_t   last_reset;            /* Timestamp of most recent reset */
    time_t   last_cooldown;         /* Timestamp of most recent cooldown */
} fw_health_stats_t;

/* Main firmware health context */
typedef struct {
    /* Configuration */
    char iface[32];                 /* Monitor interface name */
    int  max_fps;                   /* Current dynamic FPS limit */

    /* Rate tracking (per-second window) */
    uint32_t frames_this_second;    /* Frames injected in current 1s window */
    time_t   current_second;        /* Start of current 1s window */

    /* Failure tracking */
    int  consecutive_failures;      /* Consecutive injection failures */
    int  consecutive_successes;     /* Consecutive injection successes */

    /* Lifetime counters */
    uint64_t total_injected;
    uint64_t total_failed;
    uint64_t total_throttled;
    uint32_t resets_triggered;
    uint32_t cooldowns_triggered;
    uint64_t frames_since_cooldown; /* Frames since last cooldown/reset */

    /* State */
    fw_health_state_t state;
    time_t last_reset;
    time_t last_cooldown;
    time_t cooldown_until;          /* Non-zero = in cooldown until this time */

    /* Rolling success rate (exponential moving average) */
    float success_rate;

    /* Logging */
    FILE *log_fp;

    /* Thread safety */
    pthread_mutex_t lock;
    bool initialized;
} fw_health_t;

/* ============================================================================
 * API Functions
 * ============================================================================ */

/**
 * Initialize firmware health monitor.
 *
 * @param fwh       Pointer to fw_health_t struct (caller-owned)
 * @param iface     Monitor interface name (e.g. "wlan0mon")
 * @return 0 on success, -1 on error
 */
int fw_health_init(fw_health_t *fwh, const char *iface);

/**
 * Check if injection is allowed right now.
 *
 * Call this BEFORE every attack_raw_send(). If it returns false, skip the
 * injection — the chip needs rest.
 *
 * @param fwh       Firmware health context
 * @return true if injection is allowed, false if throttled/cooling
 */
bool fw_health_can_inject(fw_health_t *fwh);

/**
 * Report a successful frame injection.
 *
 * Call this AFTER attack_raw_send() returns > 0.
 *
 * @param fwh       Firmware health context
 */
void fw_health_report_success(fw_health_t *fwh);

/**
 * Report a failed frame injection.
 *
 * Call this AFTER attack_raw_send() returns <= 0.
 * May trigger automatic rate reduction or interface reset.
 *
 * @param fwh       Firmware health context
 */
void fw_health_report_failure(fw_health_t *fwh);

/**
 * Epoch tick — call once per epoch to check pre-emptive cooldown schedule.
 *
 * Checks if total frames since last cooldown exceeds threshold and
 * schedules a rest period if needed.
 *
 * @param fwh       Firmware health context
 * @return true if a cooldown was just scheduled (brain should skip attacks)
 */
bool fw_health_epoch_tick(fw_health_t *fwh);

/**
 * Force an immediate interface reset.
 *
 * Brings the interface down, waits, then brings it back up.
 * Called automatically on FW_HEALTH_FAIL_RESET consecutive failures.
 *
 * @param fwh       Firmware health context
 * @return 0 on success, -1 on error
 */
int fw_health_force_reset(fw_health_t *fwh);

/**
 * Check if currently in cooldown period.
 *
 * @param fwh       Firmware health context
 * @return true if in cooldown, false if ready
 */
bool fw_health_in_cooldown(fw_health_t *fwh);

/**
 * Get current statistics snapshot.
 *
 * @param fwh       Firmware health context
 * @param stats     Output stats struct
 */
void fw_health_get_stats(fw_health_t *fwh, fw_health_stats_t *stats);

/**
 * Get printable status string.
 *
 * @param fwh       Firmware health context
 * @param buf       Output buffer
 * @param len       Buffer length
 */
void fw_health_status_str(fw_health_t *fwh, char *buf, size_t len);

/**
 * Get state name as string.
 *
 * @param state     Firmware health state enum
 * @return Static string
 */
const char *fw_health_state_name(fw_health_state_t state);

/**
 * Destroy firmware health context — close log, release mutex.
 *
 * @param fwh       Firmware health context
 */
void fw_health_destroy(fw_health_t *fwh);

#endif /* FIRMWARE_HEALTH_H */
