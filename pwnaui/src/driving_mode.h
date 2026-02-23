/*
 * driving_mode.h — Driving Mode Pipeline (Phase 2B)
 *
 * PMKID spray + Map Blitz pipeline for in-vehicle operation.
 *
 * Strategy:
 *   - 50ms channel hops across all 14 channels (700ms/full sweep)
 *   - On each channel: send ONE association to strongest uncaptured AP
 *   - No deauths, no CSA — PMKID only
 *   - 15 sweeps per 10-second cycle
 *   - Every AP seen gets logged to ap_database with GPS breadcrumb
 *   - Track which channels had associations for PMKID harvesting on revisit
 *
 * Design: This module manages the sweep state machine. The actual
 * brain_epoch_driving() function in brain.c calls into this module
 * to get per-channel timing and track spray state.
 */

#ifndef DRIVING_MODE_H
#define DRIVING_MODE_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* ── Sweep constants ─────────────────────────────────────────────── */

#define DRV_HOP_MS               50     /* Dwell time per channel (ms)       */
#define DRV_MAX_CHANNELS         14     /* 2.4GHz channels 1-14              */
#define DRV_SWEEP_MS             700    /* Full sweep time (14 * 50ms)       */
#define DRV_CYCLE_SEC            10     /* One attack cycle duration         */
#define DRV_SWEEPS_PER_CYCLE     15     /* Target sweeps per cycle           */
#define DRV_MAX_ASSOC_PER_SWEEP  14     /* Max associations per full sweep   */
#define DRV_PMKID_LISTEN_MS      20     /* Extra listen time on revisit ch   */

/* ── Breadcrumb constants ────────────────────────────────────────── */

#define DRV_BREADCRUMB_MAX       512    /* Max breadcrumbs per session       */
#define DRV_BSSID_LEN            18     /* "xx:xx:xx:xx:xx:xx\0"             */

/* ── Per-channel association state ───────────────────────────────── */

typedef struct {
    bool    associated;              /* Sent assoc this sweep?             */
    char    assoc_bssid[DRV_BSSID_LEN]; /* BSSID we associated with       */
    int8_t  strongest_rssi;          /* Strongest uncaptured AP on ch      */
    uint8_t ap_count;                /* Number of APs visible on channel   */
    uint32_t total_associations;     /* Lifetime assoc count on this ch    */
    uint32_t pmkid_captures;         /* PMKIDs captured on this channel    */
} drv_channel_state_t;

/* ── AP breadcrumb (GPS sighting log) ────────────────────────────── */

typedef struct {
    char    bssid[DRV_BSSID_LEN];    /* AP MAC address                    */
    char    ssid[33];                /* SSID                              */
    int8_t  rssi;                    /* Signal strength at sighting       */
    uint8_t channel;                 /* Channel AP was seen on            */
    double  lat;                     /* GPS latitude                      */
    double  lon;                     /* GPS longitude                     */
    time_t  timestamp;               /* When we saw it                    */
    bool    associated;              /* Did we send an association?        */
    bool    pmkid_captured;          /* Did we get a PMKID?               */
} drv_breadcrumb_t;

/* ── Sweep state ─────────────────────────────────────────────────── */

typedef struct {
    int      sweep_num;              /* Current sweep number (0-based)    */
    int      channel_idx;            /* Current channel index in sweep    */
    time_t   sweep_start;            /* When current sweep started        */
    int      assoc_this_sweep;       /* Associations sent this sweep      */
    int      aps_this_sweep;         /* APs seen this sweep               */
} drv_sweep_state_t;

/* ── Driving mode context ────────────────────────────────────────── */

typedef struct {
    /* Sweep management */
    drv_sweep_state_t sweep;
    drv_channel_state_t channels[DRV_MAX_CHANNELS + 1]; /* Index by ch# 1-14 */

    /* Breadcrumb trail */
    drv_breadcrumb_t breadcrumbs[DRV_BREADCRUMB_MAX];
    int              breadcrumb_count;
    int              breadcrumb_write_idx;    /* Ring buffer index        */

    /* Session statistics */
    uint32_t total_sweeps;            /* Total sweeps completed           */
    uint32_t total_associations;      /* Total assoc attempts             */
    uint32_t total_pmkid_captures;    /* PMKIDs captured this session     */
    uint32_t total_aps_logged;        /* Unique APs breadcrumbed          */
    uint32_t total_new_aps;           /* APs never seen before            */
    time_t   session_start;           /* When driving mode activated      */
    time_t   last_sweep_time;         /* When last sweep completed        */

    /* Cycle tracking */
    int      sweeps_this_cycle;       /* Sweeps in current 10s cycle      */
    time_t   cycle_start;             /* When current cycle started       */
    int      pmkid_this_cycle;        /* PMKIDs in current cycle          */

    /* Performance */
    float    sweep_rate;              /* Sweeps per second (rolling avg)  */
    float    pmkid_rate;              /* PMKIDs per minute (rolling avg)  */

    bool     active;                  /* Is driving mode currently active */
} drv_ctx_t;

/* ── API ─────────────────────────────────────────────────────────── */

/**
 * Initialize the driving mode context. Call once at startup.
 */
void drv_init(drv_ctx_t *ctx);

/**
 * Start a new driving session. Resets per-session counters.
 * Called when mobility mode switches to DRIVING.
 */
void drv_session_start(drv_ctx_t *ctx);

/**
 * Begin a new sweep (full 14-channel pass).
 * Resets per-sweep channel states.
 */
void drv_sweep_begin(drv_ctx_t *ctx);

/**
 * Record that we're entering a channel during the sweep.
 * Returns the dwell time in milliseconds.
 *
 * @param ctx      Driving context
 * @param channel  WiFi channel number (1-14)
 * @return         Dwell time in ms (DRV_HOP_MS or extended for revisit)
 */
int drv_enter_channel(drv_ctx_t *ctx, int channel);

/**
 * Check if we should associate with a given AP on the current channel.
 * Returns true if the AP is a valid PMKID spray target.
 *
 * @param ctx             Driving context
 * @param channel         Channel number
 * @param rssi            AP signal strength
 * @param has_handshake   Already have a handshake for this AP?
 * @param pmkid_available Already have PMKID?
 * @return                true if should send association
 */
bool drv_should_associate(drv_ctx_t *ctx, int channel,
                          int8_t rssi, bool has_handshake,
                          bool pmkid_available);

/**
 * Record an association attempt on a channel.
 *
 * @param ctx      Driving context
 * @param channel  Channel number
 * @param bssid    BSSID string of the associated AP
 * @param rssi     Signal strength
 */
void drv_record_association(drv_ctx_t *ctx, int channel,
                            const char *bssid, int8_t rssi);

/**
 * Record a PMKID capture.
 *
 * @param ctx      Driving context
 * @param channel  Channel on which PMKID was captured
 */
void drv_record_pmkid(drv_ctx_t *ctx, int channel);

/**
 * Add an AP breadcrumb to the trail.
 *
 * @param ctx         Driving context
 * @param bssid       AP BSSID string
 * @param ssid        AP SSID
 * @param rssi        Signal strength
 * @param channel     Channel
 * @param lat         GPS latitude (0.0 if no fix)
 * @param lon         GPS longitude (0.0 if no fix)
 * @param associated  Whether we sent an association
 */
void drv_add_breadcrumb(drv_ctx_t *ctx,
                        const char *bssid, const char *ssid,
                        int8_t rssi, uint8_t channel,
                        double lat, double lon,
                        bool associated);

/**
 * Complete the current sweep. Updates sweep counters and rate.
 */
void drv_sweep_end(drv_ctx_t *ctx);

/**
 * Check if a new 10-second cycle should start.
 * Resets cycle counters if needed.
 *
 * @param ctx  Driving context
 * @return     true if a new cycle just started
 */
bool drv_check_cycle(drv_ctx_t *ctx);

/**
 * Get the channel order for driving mode.
 * Returns channels in a randomized order to avoid predictable patterns.
 *
 * @param ctx       Driving context
 * @param channels  Output: channel numbers (1-14)
 * @param count     Output: number of channels
 */
void drv_get_channel_order(drv_ctx_t *ctx, int *channels, int *count);

/**
 * Check if channel should get extra listen time (PMKID harvesting).
 * Returns true if we previously associated on this channel and
 * should listen longer for PMKID responses.
 *
 * @param ctx      Driving context
 * @param channel  Channel number
 * @return         true if extra listen time needed
 */
bool drv_should_harvest(drv_ctx_t *ctx, int channel);

/**
 * End the driving session. Log summary statistics.
 */
void drv_session_end(drv_ctx_t *ctx);

/**
 * Dump driving mode statistics to stderr.
 */
void drv_dump_stats(drv_ctx_t *ctx);

#endif /* DRIVING_MODE_H */
