/*
 * stationary_mode.h — Stationary Mode Pipeline (Phase 2C)
 *
 * Carpet bomb then harvest. Four-phase attack cycle:
 *   Phase 1 (5s):     Full 14-channel environment scan, build triage list
 *   Phase 2 (30-60s): Channel-grouped attack burst (use 1D batching)
 *     Gold:   deauth_bidi + csa_beacon + assoc_pmkid
 *     Silver: deauth_bidi + assoc_pmkid
 *     Bronze: assoc_pmkid only
 *   Phase 3:          Circle-back: re-attack APs where M1 seen but not M2
 *   Phase 4 (60s):    Passive soak: zero injection, just listen for EAPOL
 *
 * Includes diminishing returns detection and per-AP attack rotation.
 */

#ifndef STATIONARY_MODE_H
#define STATIONARY_MODE_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* ── Phase definitions ───────────────────────────────────────────── */

typedef enum {
    STAT_PHASE_SCAN    = 0,   /* Environment scan + triage build       */
    STAT_PHASE_BURST   = 1,   /* Channel-grouped attack burst          */
    STAT_PHASE_CIRCLE  = 2,   /* Circle-back: re-attack M1-only APs   */
    STAT_PHASE_SOAK    = 3,   /* Passive listen, zero injection        */
    STAT_PHASE_COUNT   = 4
} stat_phase_t;

/* ── Timing constants ────────────────────────────────────────────── */

#define STAT_SCAN_SEC            5      /* Phase 1 duration              */
#define STAT_BURST_MIN_SEC       30     /* Phase 2 minimum               */
#define STAT_BURST_MAX_SEC       60     /* Phase 2 maximum               */
#define STAT_CIRCLE_MAX_SEC      15     /* Phase 3 maximum               */
#define STAT_SOAK_SEC            60     /* Phase 4 duration              */

/* ── Diminishing returns ─────────────────────────────────────────── */

#define STAT_STALE_CYCLES        3      /* Cycles with 0 captures = stale*/
#define STAT_AGGRESSIVE_RATE     2.0f   /* HS/min rate to stay aggressive*/
#define STAT_PERM_SOAK_AFTER     3      /* Enter perm soak after N stale */

/* ── Attack rotation ─────────────────────────────────────────────── */

#define STAT_MAX_FAIL_BEFORE_ROTATE  3  /* Failures before trying next   */
#define STAT_MAX_TRACKED_APS    64      /* Max APs with per-AP tracking  */

/* ── Attack types for rotation ───────────────────────────────────── */

typedef enum {
    STAT_ATK_DEAUTH_BIDI = 0,     /* Targeted deauth (both directions)  */
    STAT_ATK_CSA_BEACON  = 1,     /* Channel switch announcement        */
    STAT_ATK_PMKID_ASSOC = 2,     /* PMKID elicitation                  */
    STAT_ATK_DEAUTH_BCAST = 3,    /* Broadcast deauth                   */
    STAT_ATK_DISASSOC    = 4,     /* Disassociation                     */
    STAT_ATK_COUNT       = 5
} stat_attack_type_t;

/* ── Per-AP attack tracker ───────────────────────────────────────── */

typedef struct {
    char     bssid[18];              /* AP BSSID                        */
    int      fail_count[STAT_ATK_COUNT]; /* Consecutive fails per type  */
    stat_attack_type_t preferred;    /* Current preferred attack type    */
    int      rotations;              /* Times we've rotated attack type  */
    bool     has_m1;                 /* Seen M1 from EAPOL monitor?     */
    bool     has_m2;                 /* Seen M2 (crackable pair)?       */
    bool     captured;               /* Full handshake captured?         */
    int      total_attacks;          /* Total attack attempts            */
    time_t   last_attacked;          /* Time of last attack              */
} stat_ap_tracker_t;

/* ── Cycle statistics ────────────────────────────────────────────── */

typedef struct {
    int      cycle_num;              /* Which attack cycle this is       */
    int      captures_this_cycle;    /* Handshakes captured this cycle   */
    int      attacks_this_cycle;     /* Attack attempts this cycle       */
    float    capture_rate;           /* Handshakes per minute            */
    time_t   cycle_start;            /* When this cycle started          */
    int      total_captures_session; /* Session-lifetime captures        */
    int      stale_cycles;           /* Consecutive 0-capture cycles     */
} stat_cycle_stats_t;

/* ── Main stationary context ─────────────────────────────────────── */

typedef struct {
    /* Current phase */
    stat_phase_t phase;
    time_t       phase_start;
    int          phase_duration_sec;    /* Dynamic: shortened/extended  */

    /* Per-AP tracking */
    stat_ap_tracker_t ap_trackers[STAT_MAX_TRACKED_APS];
    int               ap_tracker_count;

    /* Cycle tracking */
    stat_cycle_stats_t cycle;

    /* Diminishing returns */
    bool         permanent_soak;     /* Entered permanent passive soak  */
    int          consecutive_stale;  /* Consecutive 0-capture cycles    */

    /* Session */
    time_t       session_start;
    bool         active;

    /* Circle-back candidates */
    int          circle_back_count;  /* APs with M1 but no M2           */
} stat_ctx_t;

/* ── API ─────────────────────────────────────────────────────────── */

/**
 * Initialize stationary mode context.
 */
void stat_init(stat_ctx_t *ctx);

/**
 * Start a new stationary session.
 */
void stat_session_start(stat_ctx_t *ctx);

/**
 * Advance to the next phase in the cycle.
 * Returns the new phase.
 */
stat_phase_t stat_advance_phase(stat_ctx_t *ctx);

/**
 * Check if the current phase has expired (time-based).
 * @return true if phase time is up
 */
bool stat_phase_expired(stat_ctx_t *ctx);

/**
 * Get the suggested attack type for a specific AP.
 * Handles rotation: if deauth failed 3x, suggest CSA, etc.
 *
 * @param ctx    Stationary context
 * @param bssid  AP BSSID string
 * @return       Suggested attack type
 */
stat_attack_type_t stat_get_attack_type(stat_ctx_t *ctx, const char *bssid);

/**
 * Record an attack attempt result for rotation tracking.
 *
 * @param ctx      Stationary context
 * @param bssid    Target AP BSSID
 * @param type     Attack type used
 * @param success  Whether attack led to EAPOL progress
 */
void stat_record_attack(stat_ctx_t *ctx, const char *bssid,
                        stat_attack_type_t type, bool success);

/**
 * Record an EAPOL state update for circle-back tracking.
 *
 * @param ctx      Stationary context
 * @param bssid    AP BSSID
 * @param has_m1   M1 frame seen
 * @param has_m2   M2 frame seen (crackable)
 */
void stat_record_eapol_state(stat_ctx_t *ctx, const char *bssid,
                             bool has_m1, bool has_m2);

/**
 * Record a handshake capture. Updates diminishing returns counters.
 */
void stat_record_capture(stat_ctx_t *ctx);

/**
 * Check if we should enter permanent passive soak (diminishing returns).
 * @return true if should soak permanently
 */
bool stat_should_permanent_soak(stat_ctx_t *ctx);

/**
 * Start a new attack cycle. Resets per-cycle counters.
 */
void stat_new_cycle(stat_ctx_t *ctx);

/**
 * Get the number of APs with M1 but no M2 (circle-back candidates).
 * @return count of circle-back targets
 */
int stat_get_circle_back_count(stat_ctx_t *ctx);

/**
 * Check if an AP is a circle-back candidate (M1 seen, no M2).
 */
bool stat_is_circle_back(stat_ctx_t *ctx, const char *bssid);

/**
 * Get the burst duration for this cycle (adaptive).
 * Shorter if capture rate is high, longer if low.
 */
int stat_get_burst_duration(stat_ctx_t *ctx);

/**
 * End the stationary session. Log summary.
 */
void stat_session_end(stat_ctx_t *ctx);

/**
 * Dump stationary mode statistics.
 */
void stat_dump_stats(stat_ctx_t *ctx);

/**
 * Get phase name string.
 */
const char *stat_phase_name(stat_phase_t phase);

#endif /* STATIONARY_MODE_H */
