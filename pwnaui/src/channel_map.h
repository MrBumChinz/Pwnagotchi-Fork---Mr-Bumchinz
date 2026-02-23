/**
 * channel_map.h - Channel-Grouped Attack Batching (Phase 1D)
 *
 * Builds a yield-scored channel map from visible APs.
 * Sorts channels by expected handshake yield so the attack loop
 * visits the most productive channels first and skips
 * fully-captured ones entirely.
 *
 * Complements channel_bandit.h (Thompson exploration/exploitation)
 * with a deterministic scoring layer:
 *   expected_yield = uncaptured_count * W_UNCAP
 *                  + client_count      * W_CLIENT
 *                  + thompson_score    * W_THOMPSON
 *                  + recency_bonus     * W_RECENCY
 *
 * Also computes per-channel listen windows: channels that recently
 * produced handshakes get a longer post-attack dwell.
 *
 * Zero heap allocation - everything fits in fixed-size structs.
 *
 * Copyright (c) 2025 PwnaUI Project
 */

#ifndef CHANNEL_MAP_H
#define CHANNEL_MAP_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* ── Constants ───────────────────────────────────────────────── */

/** Max WiFi channels we track (1-14 2.4 GHz + 36-165 5 GHz). */
#define CMAP_MAX_CHANNELS    32

/** Max APs we can index per channel. */
#define CMAP_MAX_APS_PER_CH  16

/** Default post-attack listen window (ms). */
#define CMAP_LISTEN_BASE_MS  800

/** Bonus listen time when channel was recently productive (ms). */
#define CMAP_LISTEN_BONUS_MS 1200

/** Recency window for "recently productive" (seconds). */
#define CMAP_RECENCY_WINDOW  300   /* 5 minutes */

/** Yield weight: uncaptured APs. */
#define CMAP_W_UNCAPTURED    3.0f

/** Yield weight: client count. */
#define CMAP_W_CLIENTS       2.0f

/** Yield weight: Thompson Sampling score. */
#define CMAP_W_THOMPSON      1.5f

/** Yield weight: recency bonus for productive channels. */
#define CMAP_W_RECENCY       1.0f

/* ── Types ───────────────────────────────────────────────────── */

/**
 * Per-channel summary built from one scan cycle.
 * Sorted by expected_yield descending.
 */
typedef struct {
    int      channel;           /**< WiFi channel number (1-165)          */
    int      ap_count;          /**< Total visible APs on this channel    */
    int      client_count;      /**< Total associated clients             */
    int      uncaptured_count;  /**< APs without a full handshake yet     */
    float    expected_yield;    /**< Composite score (higher = attack first) */
    float    thompson_score;    /**< Raw Thompson sample for this channel */
    time_t   last_productive;   /**< Last time a HS was captured here     */
    int      listen_ms;         /**< Post-attack listen window (ms)       */

    /* AP index cache: indices into bcap AP array for APs on this channel.
     * Avoids a second pass through the AP list during attack. */
    int      ap_indices[CMAP_MAX_APS_PER_CH];
    int      ap_index_count;
} channel_map_entry_t;

/**
 * Full channel map - rebuilt each epoch from scan data.
 */
typedef struct {
    channel_map_entry_t entries[CMAP_MAX_CHANNELS]; /**< Sorted by yield   */
    int                 count;                       /**< Active entries    */
    time_t              build_time;                  /**< When map was built */

    /* Persistent state across epochs */
    time_t   productive_ts[166];  /**< Per-channel last-productive timestamps
                                       (indexed by channel number 1-165)     */
    uint32_t epoch_visits[166];   /**< Total visits per channel              */
    uint32_t epoch_captures[166]; /**< Total captures per channel            */
} channel_map_t;

/**
 * Attack order: just the channel numbers, sorted by yield.
 * Returned by channel_map_get_attack_order().
 */
typedef struct {
    int channels[CMAP_MAX_CHANNELS];     /**< Channel numbers, sorted      */
    int listen_ms[CMAP_MAX_CHANNELS];    /**< Per-channel listen window    */
    int count;                            /**< Number of channels to visit  */
    int skipped;                          /**< Channels skipped (0 uncaptured) */
} channel_attack_order_t;

/* ── API ─────────────────────────────────────────────────────── */

/**
 * Initialize a channel map (zero everything).
 * Call once at brain startup.
 */
void channel_map_init(channel_map_t *cm);

/**
 * Build the channel map from current scan data.
 *
 * Iterates all visible APs, groups by channel, computes yield scores,
 * and sorts by expected_yield descending.
 *
 * @param cm           Channel map to populate
 * @param ap_channels  Array of channel numbers for each AP (indexed 0..ap_count-1)
 * @param ap_clients   Array of client counts for each AP
 * @param ap_captured  Array of booleans: true if AP has a full handshake
 * @param ap_rssi      Array of RSSI values for each AP
 * @param ap_count     Number of APs in the arrays
 * @param cb           Optional: channel_bandit for Thompson scores (NULL = skip)
 */
void channel_map_build(channel_map_t *cm,
                       const int *ap_channels,
                       const int *ap_clients,
                       const bool *ap_captured,
                       const int *ap_rssi,
                       int ap_count,
                       const void *cb);  /* cb_bandit_t* — void to avoid circular include */

/**
 * Get the sorted attack order, skipping channels with 0 uncaptured APs.
 *
 * @param cm    Built channel map
 * @param order Output: sorted channel list + listen windows
 */
void channel_map_get_attack_order(const channel_map_t *cm,
                                  channel_attack_order_t *order);

/**
 * Look up a specific channel's entry.
 *
 * @param cm      Built channel map
 * @param channel Channel number to find
 * @return        Pointer to entry, or NULL if channel not in map
 */
const channel_map_entry_t *channel_map_get_entry(const channel_map_t *cm,
                                                  int channel);

/**
 * Record a handshake capture on a channel.
 * Updates the productive timestamp for future yield scoring.
 *
 * @param cm      Channel map
 * @param channel Channel where capture occurred
 */
void channel_map_record_capture(channel_map_t *cm, int channel);

/**
 * Get recommended listen time for a channel.
 * Productive channels get longer listen windows.
 *
 * @param cm      Channel map
 * @param channel Channel number
 * @return        Listen time in milliseconds
 */
int channel_map_get_listen_ms(const channel_map_t *cm, int channel);

/**
 * Log the current channel map to stderr for debugging.
 *
 * @param cm Channel map to dump
 */
void channel_map_dump(const channel_map_t *cm);

/**
 * Destroy / reset a channel map.
 * No heap to free, just zeroes persistent stats.
 */
void channel_map_destroy(channel_map_t *cm);

#endif /* CHANNEL_MAP_H */
