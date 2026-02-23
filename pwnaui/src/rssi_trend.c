/**
 * rssi_trend.c - RSSI Trend Tracking (Phase 1E)
 *
 * Ring-buffer RSSI history with linear trend classification.
 * Used in WALKING mode to time attacks for peak RSSI proximity.
 *
 * Zero heap.  All state in rssi_trend_tracker_t.
 *
 * Copyright (c) 2025 PwnaUI Project
 */

#include "rssi_trend.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ── Helpers ─────────────────────────────────────────────────── */

/**
 * Count valid readings (not older than RSSI_MAX_AGE seconds).
 * Returns them in chronological order via out[] (oldest first).
 */
static int get_valid_readings(const rssi_trend_tracker_t *tracker,
                               rssi_reading_t *out, int max_out,
                               time_t now)
{
    int n = 0;
    int total = (tracker->count < RSSI_HISTORY_SIZE)
                ? tracker->count : RSSI_HISTORY_SIZE;

    /* Walk ring buffer from oldest to newest */
    int start;
    if (tracker->count < RSSI_HISTORY_SIZE)
        start = 0;
    else
        start = tracker->write_idx;  /* oldest entry */

    for (int i = 0; i < total && n < max_out; i++) {
        int idx = (start + i) % RSSI_HISTORY_SIZE;
        const rssi_reading_t *r = &tracker->readings[idx];

        /* Skip readings older than RSSI_MAX_AGE */
        if (now > 0 && r->timestamp > 0) {
            double age = difftime(now, r->timestamp);
            if (age > (double)RSSI_MAX_AGE)
                continue;
        }

        out[n++] = *r;
    }
    return n;
}

/**
 * Compute linear slope from ordered readings (dB per reading).
 * Uses simple (newest - oldest) / (count - 1).
 */
static float compute_slope(const rssi_reading_t *readings, int count)
{
    if (count < 2)
        return 0.0f;

    float newest = (float)readings[count - 1].rssi;
    float oldest = (float)readings[0].rssi;

    return (newest - oldest) / (float)(count - 1);
}

/* ── Public API ──────────────────────────────────────────────── */

void rssi_trend_init(rssi_trend_tracker_t *tracker)
{
    if (!tracker) return;
    memset(tracker, 0, sizeof(*tracker));
    tracker->peak_rssi = -127;  /* Start at minimum */
}

void rssi_trend_record(rssi_trend_tracker_t *tracker, int8_t rssi)
{
    if (!tracker) return;

    time_t now = time(NULL);

    /* Write to ring buffer */
    tracker->readings[tracker->write_idx].rssi = rssi;
    tracker->readings[tracker->write_idx].timestamp = now;
    tracker->write_idx = (tracker->write_idx + 1) % RSSI_HISTORY_SIZE;
    tracker->count++;

    /* Track peak */
    if (rssi > tracker->peak_rssi) {
        tracker->peak_rssi = rssi;
        tracker->peak_time = now;
    }
}

rssi_trend_t rssi_trend_classify(const rssi_trend_tracker_t *tracker,
                                  rssi_trend_info_t *info)
{
    rssi_trend_info_t local_info;
    if (!info) info = &local_info;

    memset(info, 0, sizeof(*info));
    info->trend = RSSI_TREND_UNKNOWN;
    info->priority = RSSI_PRI_UNKNOWN;
    info->peak_rssi = tracker ? tracker->peak_rssi : -127;

    if (!tracker || tracker->count == 0) {
        return RSSI_TREND_UNKNOWN;
    }

    time_t now = time(NULL);

    /* Get valid (recent) readings in chronological order */
    rssi_reading_t valid[RSSI_HISTORY_SIZE];
    int n = get_valid_readings(tracker, valid, RSSI_HISTORY_SIZE, now);
    info->valid_readings = n;

    if (n < RSSI_MIN_READINGS) {
        return RSSI_TREND_UNKNOWN;
    }

    /* Compute slope */
    float slope = compute_slope(valid, n);
    info->slope = slope;

    /* Check if current reading is at or near peak */
    int8_t current = valid[n - 1].rssi;
    info->at_peak = (current >= tracker->peak_rssi - 1);

    /* Classify */
    if (slope > RSSI_TREND_APPROACH) {
        info->trend = RSSI_TREND_APPROACHING;
        info->priority = RSSI_PRI_APPROACHING;
    } else if (slope < RSSI_TREND_DEPART) {
        info->trend = RSSI_TREND_DEPARTING;
        info->priority = RSSI_PRI_DEPARTING;

        /* Last-chance boost: if departing but still very strong, attack now */
        if (current > -55) {
            info->priority = 1.2f;
        }
    } else {
        info->trend = RSSI_TREND_PEAK;
        info->priority = RSSI_PRI_PEAK;

        /* Extra boost if at historical peak */
        if (info->at_peak) {
            info->priority = 2.0f;
        }
    }

    return info->trend;
}

float rssi_trend_priority(const rssi_trend_tracker_t *tracker)
{
    rssi_trend_info_t info;
    rssi_trend_classify(tracker, &info);
    return info.priority;
}

bool rssi_trend_should_delay(const rssi_trend_tracker_t *tracker, bool has_hs)
{
    if (!tracker) return false;

    /* If we already have a partial handshake, don't delay — fire now */
    if (has_hs) return false;

    rssi_trend_info_t info;
    rssi_trend_classify(tracker, &info);

    /* Only delay when APPROACHING and signal is still weak-to-moderate */
    if (info.trend == RSSI_TREND_APPROACHING) {
        int8_t current = -127;
        if (info.valid_readings > 0) {
            /* Get most recent reading */
            int newest_idx = (tracker->write_idx - 1 + RSSI_HISTORY_SIZE) % RSSI_HISTORY_SIZE;
            current = tracker->readings[newest_idx].rssi;
        }

        /* Don't delay if signal is already very strong — attack now */
        if (current > -50)
            return false;

        return true;  /* Still approaching, wait for peak */
    }

    return false;
}

const char *rssi_trend_name(rssi_trend_t trend)
{
    switch (trend) {
    case RSSI_TREND_UNKNOWN:     return "UNKNOWN";
    case RSSI_TREND_APPROACHING: return "APPROACHING";
    case RSSI_TREND_PEAK:        return "PEAK";
    case RSSI_TREND_DEPARTING:   return "DEPARTING";
    default:                     return "???";
    }
}

void rssi_trend_reset(rssi_trend_tracker_t *tracker)
{
    if (!tracker) return;
    memset(tracker, 0, sizeof(*tracker));
    tracker->peak_rssi = -127;
}
