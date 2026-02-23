/*
 * ap_triage.h — AP Triage System (Phase 2A)
 *
 * Classifies access points into Gold/Silver/Bronze/Exploit/Skip tiers
 * based on signal strength, client presence, encryption type, capture
 * status, and Thompson Sampling history.
 *
 * Budget allocation:
 *   Gold:    60% of attack time — strongest, most likely to yield
 *   Silver:  25% — decent targets, moderate probability
 *   Bronze:  10% — long shots (WPA3, no clients, weak signal)
 *   Exploit:  5% — PMKID available but no full 4-way yet
 *   Skip:     0% — already captured, whitelisted, or blacklisted
 *
 * Design principle: spend 80% of time on the 20% most likely to yield.
 */

#ifndef AP_TRIAGE_H
#define AP_TRIAGE_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* ── Tier classification ─────────────────────────────────────────── */

typedef enum {
    TRIAGE_SKIP    = 0,   /* Don't attack: captured/whitelisted/blacklisted */
    TRIAGE_BRONZE  = 1,   /* Low priority: WPA3, no clients, or weak signal */
    TRIAGE_EXPLOIT = 2,   /* PMKID opportunity: has PMKID, no full 4-way    */
    TRIAGE_SILVER  = 3,   /* Medium: decent RSSI, clients, uncaptured       */
    TRIAGE_GOLD    = 4    /* Highest: strong signal, clients, WPA2, fresh   */
} ap_triage_tier_t;

/* ── Budget allocation (percentage of epoch attack time) ─────────── */

#define TRIAGE_BUDGET_GOLD      0.60f
#define TRIAGE_BUDGET_SILVER    0.25f
#define TRIAGE_BUDGET_BRONZE    0.10f
#define TRIAGE_BUDGET_EXPLOIT   0.05f
#define TRIAGE_BUDGET_SKIP      0.00f

/* ── Classification thresholds ───────────────────────────────────── */

#define TRIAGE_RSSI_GOLD        (-55)   /* dBm: Gold requires > -55   */
#define TRIAGE_RSSI_SILVER_MIN  (-70)   /* dBm: Silver range -55..-70 */
#define TRIAGE_RSSI_BRONZE_MIN  (-85)   /* dBm: Below this = too weak */
#define TRIAGE_CLIENTS_MIN      1       /* Need >= 1 client for Gold  */
#define TRIAGE_THOMPSON_COLD    3.0f    /* alpha+beta < this = no data*/

/* ── Priority score weights (within-tier ranking) ────────────────── */

#define TRIAGE_W_RSSI           0.35f   /* Signal strength weight     */
#define TRIAGE_W_CLIENTS        0.25f   /* Client count weight        */
#define TRIAGE_W_THOMPSON       0.20f   /* Thompson success ratio wt  */
#define TRIAGE_W_FRESHNESS      0.20f   /* Time since last attack wt  */

/* ── Freshness decay ─────────────────────────────────────────────── */

#define TRIAGE_COOLDOWN_SEC     30      /* Min seconds between attacks*/
#define TRIAGE_FRESHNESS_WINDOW 300     /* 5 min: full freshness decay*/

/* ── Maximum tracked APs per triage session ──────────────────────── */

#define TRIAGE_MAX_APS          64

/* ── Input struct for classification ─────────────────────────────── */

typedef struct {
    /* From bcap_ap_t / scan data */
    int8_t   rssi;               /* Current signal strength (dBm)    */
    uint32_t clients_count;      /* Number of associated clients     */
    char     encryption[32];     /* "WPA2", "WPA3", "OPEN", "WEP"    */
    bool     pmkid_available;    /* PMKID captured from M1?          */
    bool     handshake_captured; /* Full 4-way handshake captured?   */

    /* From runtime checks */
    bool     is_whitelisted;     /* stealth_is_whitelisted() result  */
    bool     is_blacklisted;     /* brain_is_blacklisted() result    */

    /* From brain_attack_tracker_t */
    bool     is_wpa3;            /* WPA3 encryption detected         */
    int      deauth_count;       /* Total deauths sent to this AP    */
    bool     got_handshake;      /* Ever captured any handshake?     */

    /* From ts_entity_t (Thompson state) */
    float    thompson_alpha;     /* Success count + prior             */
    float    thompson_beta;      /* Failure count + prior             */
    float    client_boost;       /* Client boost factor               */

    /* Timing */
    time_t   last_attacked;      /* Unix time of last attack          */
    time_t   now;                /* Current time (for freshness calc) */
} ap_triage_input_t;

/* ── Classification result ───────────────────────────────────────── */

typedef struct {
    ap_triage_tier_t tier;       /* Assigned tier                     */
    float  priority_score;       /* 0.0..1.0 within-tier ranking      */
    float  time_budget_pct;      /* Allocated % of epoch time         */
    const char *reason;          /* Human-readable classification why */
} ap_triage_result_t;

/* ── Triage summary (per-epoch counts) ───────────────────────────── */

typedef struct {
    int      gold;               /* Count of GOLD APs                 */
    int      silver;             /* Count of SILVER APs               */
    int      bronze;             /* Count of BRONZE APs               */
    int      exploit;            /* Count of EXPLOIT APs              */
    int      skip;               /* Count of SKIP APs                 */
    int      total;              /* Total APs classified              */
    float    avg_gold_rssi;      /* Average RSSI of Gold targets      */
    float    avg_silver_rssi;    /* Average RSSI of Silver targets    */
    int      gold_rssi_sum;      /* Internal: running RSSI sum        */
    int      silver_rssi_sum;    /* Internal: running RSSI sum        */
} ap_triage_summary_t;

/* ── Triage batch (all APs classified in one epoch) ──────────────── */

typedef struct {
    ap_triage_tier_t tiers[TRIAGE_MAX_APS];
    float            scores[TRIAGE_MAX_APS];
    int              count;
    ap_triage_summary_t summary;
    time_t           build_time;
} ap_triage_batch_t;

/* ── API ─────────────────────────────────────────────────────────── */

/**
 * Classify a single AP into a triage tier.
 *
 * @param input  Populated input struct with AP data
 * @param result Output classification result
 */
void ap_triage_classify(const ap_triage_input_t *input,
                        ap_triage_result_t *result);

/**
 * Compute within-tier priority score (0.0 = lowest, 1.0 = highest).
 * Used to rank APs within the same tier for attack ordering.
 *
 * @param input  AP data
 * @return       Priority score 0.0..1.0
 */
float ap_triage_score(const ap_triage_input_t *input);

/**
 * Get the time budget percentage for a given tier.
 *
 * @param tier  Triage tier
 * @return      Budget fraction (0.0..1.0)
 */
float ap_triage_budget(ap_triage_tier_t tier);

/**
 * Get human-readable tier name.
 *
 * @param tier  Triage tier
 * @return      Static string: "GOLD", "SILVER", etc.
 */
const char *ap_triage_tier_name(ap_triage_tier_t tier);

/**
 * Initialize a triage summary (zero all counters).
 */
void ap_triage_summary_init(ap_triage_summary_t *s);

/**
 * Add a classification result to the running summary.
 *
 * @param s       Summary to update
 * @param result  Classification result to add
 * @param rssi    RSSI of the classified AP (for averaging)
 */
void ap_triage_summary_add(ap_triage_summary_t *s,
                           const ap_triage_result_t *result,
                           int8_t rssi);

/**
 * Finalize summary (compute averages). Call after all APs classified.
 */
void ap_triage_summary_finalize(ap_triage_summary_t *s);

/**
 * Log triage summary to stderr.
 *
 * @param s  Finalized summary
 */
void ap_triage_summary_dump(const ap_triage_summary_t *s);

/**
 * Initialize a triage batch.
 */
void ap_triage_batch_init(ap_triage_batch_t *batch);

/**
 * Check if an AP should be attacked in the current epoch based on
 * its tier and the current budget allocation.
 *
 * @param tier           AP's triage tier
 * @param tier_rank      AP's rank within its tier (0 = best)
 * @param tier_count     Total APs in this tier
 * @param total_budget_s Total attack time available (seconds)
 * @return               Allocated time for this AP (seconds), 0 = skip
 */
float ap_triage_allocate_time(ap_triage_tier_t tier,
                              int tier_rank,
                              int tier_count,
                              float total_budget_s);

/**
 * Check if an AP is in cooldown (attacked too recently).
 *
 * @param last_attacked  Unix timestamp of last attack
 * @param now            Current time
 * @return               true if still in cooldown
 */
bool ap_triage_in_cooldown(time_t last_attacked, time_t now);

#endif /* AP_TRIAGE_H */
