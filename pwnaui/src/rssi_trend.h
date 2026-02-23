/**
 * rssi_trend.h - RSSI Trend Tracking (Phase 1E)
 *
 * Tracks RSSI history per-AP and classifies the trend as
 * APPROACHING, PEAK, or DEPARTING.  In WALKING mode the brain
 * uses this to delay attacks until RSSI peaks, then fires the
 * full attack battery for maximum handshake probability.
 *
 * Design:
 *   - Ring buffer of 5 RSSI readings with timestamps per AP
 *   - Trend = (newest - oldest) / num_readings  (dB per reading)
 *   - Classification thresholds: ±2 dB/reading
 *   - Priority multiplier returned for use in candidate scoring
 *
 * Only active in WALKING mode.  STATIONARY has flat trends.
 * DRIVING moves too fast for meaningful RSSI tracking.
 *
 * Zero heap allocation.  Tracker storage lives in brain_attack_tracker_t.
 *
 * Copyright (c) 2025 PwnaUI Project
 */

#ifndef RSSI_TREND_H
#define RSSI_TREND_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* ── Constants ───────────────────────────────────────────────── */

/** Number of RSSI readings in the history ring buffer. */
#define RSSI_HISTORY_SIZE    5

/** Trend threshold: above this = APPROACHING (dB per reading). */
#define RSSI_TREND_APPROACH  2.0f

/** Trend threshold: below this = DEPARTING (dB per reading). */
#define RSSI_TREND_DEPART   -2.0f

/** Priority multiplier for APPROACHING targets (wait for peak). */
#define RSSI_PRI_APPROACHING 0.3f

/** Priority multiplier for PEAK targets (maximum priority). */
#define RSSI_PRI_PEAK        1.5f

/** Priority multiplier for DEPARTING targets (last chance). */
#define RSSI_PRI_DEPARTING   0.6f

/** Priority for targets with insufficient data. */
#define RSSI_PRI_UNKNOWN     1.0f

/** Minimum readings before trend is considered valid. */
#define RSSI_MIN_READINGS    3

/** Maximum age of readings for trend to be valid (seconds). */
#define RSSI_MAX_AGE         60

/* ── Types ───────────────────────────────────────────────────── */

/** RSSI trend classification. */
typedef enum {
    RSSI_TREND_UNKNOWN    = 0,  /**< Not enough data                      */
    RSSI_TREND_APPROACHING,     /**< Getting closer (RSSI rising)          */
    RSSI_TREND_PEAK,            /**< At closest point (RSSI stable/max)    */
    RSSI_TREND_DEPARTING        /**< Moving away (RSSI falling)            */
} rssi_trend_t;

/** Single RSSI reading with timestamp. */
typedef struct {
    int8_t   rssi;              /**< RSSI value in dBm                    */
    time_t   timestamp;         /**< When this reading was taken           */
} rssi_reading_t;

/**
 * Per-AP RSSI trend tracker.
 * Embedded in brain_attack_tracker_t — one per tracked AP.
 */
typedef struct {
    rssi_reading_t readings[RSSI_HISTORY_SIZE]; /**< Ring buffer           */
    int            count;       /**< Total readings inserted (wraps ring)   */
    int            write_idx;   /**< Next write position in ring            */
    int8_t         peak_rssi;   /**< Highest RSSI ever seen for this AP     */
    time_t         peak_time;   /**< When peak RSSI was observed            */
    rssi_trend_t   last_trend;  /**< Most recent classification             */
    float          last_slope;  /**< Most recent slope (dB/reading)         */
} rssi_trend_tracker_t;

/** Summary info returned by rssi_trend_classify(). */
typedef struct {
    rssi_trend_t trend;         /**< Current trend classification           */
    float        slope;         /**< Trend slope (dB/reading, + = stronger) */
    float        priority;      /**< Priority multiplier for scoring        */
    int8_t       peak_rssi;     /**< Highest observed RSSI                  */
    bool         at_peak;       /**< True if current reading >= peak        */
    int          valid_readings;/**< Number of recent valid readings         */
} rssi_trend_info_t;

/* ── API ─────────────────────────────────────────────────────── */

/**
 * Initialize a trend tracker (zero everything).
 */
void rssi_trend_init(rssi_trend_tracker_t *tracker);

/**
 * Record a new RSSI reading for an AP.
 * Call once per epoch when the AP is visible.
 *
 * @param tracker  Per-AP trend tracker
 * @param rssi     RSSI reading in dBm
 */
void rssi_trend_record(rssi_trend_tracker_t *tracker, int8_t rssi);

/**
 * Classify the current RSSI trend.
 *
 * @param tracker  Per-AP trend tracker
 * @param info     Output: trend classification + priority multiplier
 * @return         The trend enum value
 */
rssi_trend_t rssi_trend_classify(const rssi_trend_tracker_t *tracker,
                                  rssi_trend_info_t *info);

/**
 * Get the priority multiplier for target scoring.
 * Shortcut for rssi_trend_classify() → info.priority.
 *
 * @param tracker  Per-AP trend tracker
 * @return         Priority multiplier (0.3 approaching, 1.5 peak, 0.6 departing)
 */
float rssi_trend_priority(const rssi_trend_tracker_t *tracker);

/**
 * Check if we should delay attack on this AP (APPROACHING, wait for peak).
 * Only returns true in WALKING mode contexts.
 *
 * @param tracker   Per-AP trend tracker
 * @param has_hs    True if we already have a partial handshake
 * @return          True if brain should wait (PMKID only), false if attack now
 */
bool rssi_trend_should_delay(const rssi_trend_tracker_t *tracker, bool has_hs);

/**
 * Get human-readable trend name.
 */
const char *rssi_trend_name(rssi_trend_t trend);

/**
 * Reset a trend tracker (e.g., when AP disappears and reappears).
 */
void rssi_trend_reset(rssi_trend_tracker_t *tracker);

#endif /* RSSI_TREND_H */
