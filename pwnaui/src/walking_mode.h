/* ==========================================================================
 * walking_mode.h — Phase 2D: Walking mode pipeline
 *
 * Opportunistic hunter: fast 1/6/11 scan, attack top targets at RSSI peak,
 * proximity alerts for jackpot APs, GPS breadcrumbs every epoch.
 *
 * Relies on:
 *   - rssi_trend.h for approach/peak/depart scheduling
 *   - ap_triage.h for Gold/Silver/Bronze classification
 *   - eapol_monitor.h for capture verification
 *   - mobility_mode.h for MOBILITY_WALKING detection
 * ========================================================================== */
#ifndef WALKING_MODE_H
#define WALKING_MODE_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* ── Timing constants ─────────────────────────────────────────────────────── */
#define WALK_SCAN_MS             300    /* Fast scan per primary channel (ms) */
#define WALK_LISTEN_MS          2000    /* Post-attack listen window (ms)     */
#define WALK_MAX_TARGETS           3    /* Attack top N per epoch             */
#define WALK_CYCLE_MS          10000    /* Full walking cycle (ms)            */

/* Primary channels — scan these first (95%+ of consumer WiFi) */
#define WALK_PRIMARY_CH_1          1
#define WALK_PRIMARY_CH_6          6
#define WALK_PRIMARY_CH_11        11
#define WALK_NUM_PRIMARY           3

/* Secondary channels — scan if Thompson data shows them productive */
#define WALK_SECONDARY_MAX         5    /* Max additional channels to scan    */
#define WALK_SECONDARY_THRESHOLD 0.3f   /* Thompson alpha/(alpha+beta) min   */

/* ── Proximity alert thresholds ───────────────────────────────────────────── */
#define WALK_PROX_RSSI_MIN       -50    /* Min RSSI for proximity alert (dBm)*/
#define WALK_PROX_MIN_CLIENTS      1    /* Min clients for proximity alert   */
#define WALK_PROX_DISPLAY_MS    5000    /* Display alert for 5 seconds       */
#define WALK_PROX_COOLDOWN_S      30    /* Don't re-alert same AP for 30s    */
#define WALK_PROX_MAX_ALERTS       8    /* Max tracked proximity alerts      */

/* ── RSSI-based attack scheduling ─────────────────────────────────────────── */
/* Approaching: PMKID only (save deauth for peak)           */
/* Peak:        Full burst (deauth + CSA + PMKID)           */
/* Departing:   Final shot (one deauth + immediate listen)  */
typedef enum {
    WALK_ATTACK_PMKID_ONLY = 0,       /* Approaching — elicit PMKID          */
    WALK_ATTACK_FULL_BURST,            /* Peak — all attack types             */
    WALK_ATTACK_FINAL_SHOT             /* Departing — one last try            */
} walk_attack_strategy_t;

/* ── Per-AP target state ──────────────────────────────────────────────────── */
#define WALK_TARGET_MAC_LEN       18
#define WALK_TARGET_SSID_LEN      33

typedef struct {
    char     mac[WALK_TARGET_MAC_LEN];
    char     ssid[WALK_TARGET_SSID_LEN];
    int8_t   rssi;                     /* Latest RSSI                         */
    int8_t   peak_rssi;                /* Strongest seen this session         */
    uint8_t  channel;                  /* Current channel                     */
    uint8_t  clients;                  /* Client count                        */
    bool     has_handshake;            /* Already captured?                   */
    bool     pmkid_attempted;          /* Already tried PMKID?               */
    bool     attacked_at_peak;         /* Already burst-attacked at peak?    */
    bool     final_shot_done;          /* Already did departing shot?        */
    float    triage_score;             /* Triage score (for sorting)         */
    walk_attack_strategy_t strategy;   /* Current attack strategy            */
    time_t   last_seen;                /* When last detected                 */
    time_t   last_attacked;            /* When last attacked                 */
} walk_target_t;

#define WALK_MAX_TRACKED          64    /* Max APs tracked simultaneously     */

/* ── Proximity alert entry ────────────────────────────────────────────────── */
typedef struct {
    char     mac[WALK_TARGET_MAC_LEN];
    char     ssid[WALK_TARGET_SSID_LEN];
    int8_t   rssi;
    uint8_t  clients;
    time_t   triggered_at;             /* When alert fired                   */
    bool     active;                   /* Currently displaying?             */
} walk_proximity_alert_t;

/* ── GPS breadcrumb (lighter than driving mode's) ─────────────────────────── */
typedef struct {
    double   lat;
    double   lon;
    uint16_t ap_count;                 /* APs visible at this point          */
    uint16_t captures;                 /* Captures so far this session       */
    time_t   timestamp;
} walk_breadcrumb_t;

#define WALK_BREADCRUMB_MAX      256   /* Ring buffer size                    */

/* ── Session statistics ───────────────────────────────────────────────────── */
typedef struct {
    uint32_t  epochs;                  /* Epochs in walking mode              */
    uint32_t  scans;                   /* Channel scan sweeps completed       */
    uint32_t  attacks_sent;            /* Total attacks fired                 */
    uint32_t  pmkid_attempts;          /* PMKID associations sent            */
    uint32_t  handshakes;              /* Handshakes/PMKIDs captured          */
    uint32_t  proximity_alerts;        /* Proximity alerts triggered          */
    uint32_t  peak_attacks;            /* Attacks at RSSI peak               */
    uint32_t  final_shots;             /* Departing last-chance attacks       */
    uint32_t  unique_aps_seen;         /* Unique APs encountered              */
    time_t    session_start;           /* When walking session began          */
} walk_session_stats_t;

/* ── Walking mode context ─────────────────────────────────────────────────── */
typedef struct {
    bool      active;                  /* Session is running                  */

    /* Target tracking */
    walk_target_t  targets[WALK_MAX_TRACKED];
    int            target_count;

    /* Proximity alerts */
    walk_proximity_alert_t prox_alerts[WALK_PROX_MAX_ALERTS];
    int  prox_alert_count;
    bool prox_alert_active;            /* Currently displaying an alert?     */
    time_t prox_alert_expires;         /* When current alert display ends    */
    char prox_alert_text[128];         /* "[!] JACKPOT: Netgear-5G -42dBm"  */

    /* Secondary channel learning */
    uint8_t secondary_channels[WALK_SECONDARY_MAX];
    int     secondary_count;           /* 0-5 productive secondary channels  */

    /* GPS breadcrumbs */
    walk_breadcrumb_t breadcrumbs[WALK_BREADCRUMB_MAX];
    int  breadcrumb_head;
    int  breadcrumb_count;

    /* Session stats */
    walk_session_stats_t stats;

    /* Cycle timing */
    struct timespec cycle_start;
    uint32_t cycle_num;
} walk_ctx_t;

/* ── API ──────────────────────────────────────────────────────────────────── */

/**
 * Initialize walking mode context (zeroes everything).
 */
void walk_init(walk_ctx_t *ctx);

/**
 * Start a walking session. Called when mode transitions to MOBILITY_WALKING.
 */
void walk_session_start(walk_ctx_t *ctx);

/**
 * End a walking session. Dumps stats, resets targets.
 */
void walk_session_end(walk_ctx_t *ctx);

/**
 * Update a target's info from a bcap scan. Creates new entry if unseen.
 * Returns pointer to the target entry (NULL if table full and not interesting).
 */
walk_target_t *walk_update_target(walk_ctx_t *ctx, const char *mac,
                                  const char *ssid, int8_t rssi,
                                  uint8_t channel, uint8_t clients,
                                  bool has_handshake);

/**
 * Determine attack strategy for a target based on RSSI trend.
 *   APPROACHING → PMKID_ONLY
 *   PEAK        → FULL_BURST
 *   DEPARTING   → FINAL_SHOT
 *   UNKNOWN     → PMKID_ONLY (safe default)
 */
walk_attack_strategy_t walk_get_strategy(int rssi_trend_enum);

/**
 * Select top N targets from tracked list, sorted by triage_score.
 * Returns count of targets written to `out[]` (up to `max`).
 * Filters out already-captured and too-weak targets.
 */
int walk_select_targets(walk_ctx_t *ctx, walk_target_t *out, int max);

/**
 * Check if an AP triggers a proximity alert.
 * Returns true if alert fires (new AP, strong signal, has clients).
 */
bool walk_check_proximity(walk_ctx_t *ctx, const char *mac,
                          const char *ssid, int8_t rssi, uint8_t clients);

/**
 * Get the current proximity alert text (if active).
 * Returns NULL if no active alert.
 */
const char *walk_get_proximity_text(const walk_ctx_t *ctx);

/**
 * Record a GPS breadcrumb at current position.
 */
void walk_breadcrumb(walk_ctx_t *ctx, double lat, double lon,
                     uint16_t ap_count);

/**
 * Record a successful capture (handshake or PMKID).
 */
void walk_record_capture(walk_ctx_t *ctx, const char *mac);

/**
 * Record an attack attempt.
 */
void walk_record_attack(walk_ctx_t *ctx, const char *mac,
                        walk_attack_strategy_t strategy);

/**
 * Check & update proximity alert display expiry.
 * Call every epoch. Returns true if alert is currently showing.
 */
bool walk_proximity_tick(walk_ctx_t *ctx);

/**
 * Learn productive secondary channels from Thompson sampling data.
 * Call periodically to update which non-1/6/11 channels to scan.
 */
void walk_learn_secondary_channels(walk_ctx_t *ctx,
                                   const uint8_t *channels,
                                   const float *scores,
                                   int count);

/**
 * Get the list of channels to scan this cycle (primary + secondaries).
 * Returns count, fills `out[]` (up to `max`).
 */
int walk_get_scan_channels(const walk_ctx_t *ctx, uint8_t *out, int max);

/**
 * Dump session statistics to stderr.
 */
void walk_dump_stats(const walk_ctx_t *ctx);

#endif /* WALKING_MODE_H */
