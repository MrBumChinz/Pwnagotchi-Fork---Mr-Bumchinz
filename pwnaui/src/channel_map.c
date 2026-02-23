/**
 * channel_map.c - Channel-Grouped Attack Batching (Phase 1D)
 *
 * Builds a yield-scored channel map so the attack loop visits
 * the most productive channels first and skips fully-captured
 * ones entirely.  Computes per-channel listen windows so recently
 * productive channels get longer post-attack dwell times.
 *
 * Zero heap allocation.  All state lives in channel_map_t.
 *
 * Copyright (c) 2025 PwnaUI Project
 */

#include "channel_map.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ── Forward declaration for Thompson score ─────────────────── */
/* We accept cb_bandit_t* as void* to avoid a circular include.
 * We only need the alpha/beta from its channel array.          */
typedef struct {
    float alpha;
    float beta;
    /* remaining fields omitted — we only read alpha/beta */
} cb_channel_stub_t;

typedef struct {
    cb_channel_stub_t channels[166];  /* index 1-165 */
    /* remaining fields don't matter */
} cb_bandit_stub_t;

/* ts_beta_sample is defined in thompson.c, declared in channel_bandit.h */
extern float ts_beta_sample(float alpha, float beta);

/* ── Helpers ─────────────────────────────────────────────────── */

/**
 * Compute Thompson score for a channel from the bandit's Beta distribution.
 * Returns 0.5 (neutral) if bandit is NULL.
 */
static float thompson_channel_score(const void *cb, int channel)
{
    if (!cb || channel < 1 || channel > 165)
        return 0.5f;

    const cb_bandit_stub_t *bandit = (const cb_bandit_stub_t *)cb;
    float a = bandit->channels[channel].alpha;
    float b = bandit->channels[channel].beta;

    /* Sanity: uninitialised bandit entry → neutral */
    if (a < 0.01f && b < 0.01f)
        return 0.5f;

    return ts_beta_sample(a, b);
}

/**
 * Compute recency bonus: 1.0 if a capture happened in the last
 * CMAP_RECENCY_WINDOW seconds, decaying linearly to 0.0 after that.
 */
static float recency_bonus(time_t last_productive, time_t now)
{
    if (last_productive == 0)
        return 0.0f;

    double age = difftime(now, last_productive);
    if (age <= 0.0)
        return 1.0f;
    if (age >= (double)CMAP_RECENCY_WINDOW)
        return 0.0f;

    return 1.0f - (float)(age / (double)CMAP_RECENCY_WINDOW);
}

/**
 * Compute per-channel listen window.
 */
static int compute_listen_ms(time_t last_productive, time_t now)
{
    if (last_productive == 0)
        return CMAP_LISTEN_BASE_MS;

    double age = difftime(now, last_productive);
    if (age < (double)CMAP_RECENCY_WINDOW) {
        /* Recently productive — longer listen */
        float decay = 1.0f - (float)(age / (double)CMAP_RECENCY_WINDOW);
        return CMAP_LISTEN_BASE_MS + (int)(CMAP_LISTEN_BONUS_MS * decay);
    }

    return CMAP_LISTEN_BASE_MS;
}

/* ── Public API ──────────────────────────────────────────────── */

void channel_map_init(channel_map_t *cm)
{
    if (!cm) return;
    memset(cm, 0, sizeof(*cm));
}

void channel_map_build(channel_map_t *cm,
                       const int *ap_channels,
                       const int *ap_clients,
                       const bool *ap_captured,
                       const int *ap_rssi,
                       int ap_count,
                       const void *cb)
{
    if (!cm) return;

    time_t now = time(NULL);
    cm->build_time = now;
    cm->count = 0;

    /* Temporary accumulators indexed by channel (1-165) */
    int   ch_ap_count[166]       = {0};
    int   ch_client_count[166]   = {0};
    int   ch_uncaptured[166]     = {0};
    int   ch_best_rssi[166];
    bool  ch_seen[166]           = {false};

    for (int i = 0; i < 166; i++)
        ch_best_rssi[i] = -100;

    /* First pass: accumulate per-channel stats */
    for (int i = 0; i < ap_count; i++) {
        int ch = ap_channels[i];
        if (ch < 1 || ch > 165) continue;

        ch_seen[ch] = true;
        ch_ap_count[ch]++;
        ch_client_count[ch] += ap_clients[i];

        if (!ap_captured[i])
            ch_uncaptured[ch]++;

        if (ap_rssi[i] > ch_best_rssi[ch])
            ch_best_rssi[ch] = ap_rssi[i];
    }

    /* Second pass: build AP index cache per channel */
    /* Temporary per-channel index counters */
    int ch_idx_count[166] = {0};

    /* Build entries for each seen channel */
    for (int ch = 1; ch <= 165; ch++) {
        if (!ch_seen[ch]) continue;
        if (cm->count >= CMAP_MAX_CHANNELS) break;

        channel_map_entry_t *e = &cm->entries[cm->count];
        memset(e, 0, sizeof(*e));

        e->channel          = ch;
        e->ap_count         = ch_ap_count[ch];
        e->client_count     = ch_client_count[ch];
        e->uncaptured_count = ch_uncaptured[ch];
        e->last_productive  = cm->productive_ts[ch];
        e->ap_index_count   = 0;

        /* Thompson score for this channel */
        e->thompson_score = thompson_channel_score(cb, ch);

        /* Recency bonus */
        float rec = recency_bonus(e->last_productive, now);

        /* Compute expected yield */
        e->expected_yield = (float)e->uncaptured_count * CMAP_W_UNCAPTURED
                          + (float)e->client_count     * CMAP_W_CLIENTS
                          + e->thompson_score          * CMAP_W_THOMPSON
                          + rec                        * CMAP_W_RECENCY;

        /* Small RSSI bonus: channels with strong APs are more likely to yield */
        if (ch_best_rssi[ch] > -60)
            e->expected_yield += 0.5f;

        /* Listen window */
        e->listen_ms = compute_listen_ms(e->last_productive, now);

        cm->count++;
    }

    /* Fill AP index caches (second pass through APs) */
    /* Reset temp counters */
    memset(ch_idx_count, 0, sizeof(ch_idx_count));
    for (int i = 0; i < ap_count; i++) {
        int ch = ap_channels[i];
        if (ch < 1 || ch > 165) continue;

        /* Find the entry for this channel */
        for (int j = 0; j < cm->count; j++) {
            if (cm->entries[j].channel == ch) {
                if (cm->entries[j].ap_index_count < CMAP_MAX_APS_PER_CH) {
                    cm->entries[j].ap_indices[cm->entries[j].ap_index_count++] = i;
                }
                break;
            }
        }
    }

    /* Sort by expected yield (descending) */
    if (cm->count > 1) {
        /* Simple insertion sort — max 32 entries, no need for qsort overhead */
        for (int i = 1; i < cm->count; i++) {
            channel_map_entry_t tmp = cm->entries[i];
            int j = i - 1;
            while (j >= 0 && cm->entries[j].expected_yield < tmp.expected_yield) {
                cm->entries[j + 1] = cm->entries[j];
                j--;
            }
            cm->entries[j + 1] = tmp;
        }
    }
}

void channel_map_get_attack_order(const channel_map_t *cm,
                                  channel_attack_order_t *order)
{
    if (!cm || !order) return;

    memset(order, 0, sizeof(*order));

    for (int i = 0; i < cm->count; i++) {
        const channel_map_entry_t *e = &cm->entries[i];

        if (e->uncaptured_count == 0) {
            /* Skip fully-captured channels */
            order->skipped++;
            continue;
        }

        if (order->count >= CMAP_MAX_CHANNELS)
            break;

        order->channels[order->count]  = e->channel;
        order->listen_ms[order->count] = e->listen_ms;
        order->count++;
    }
}

const channel_map_entry_t *channel_map_get_entry(const channel_map_t *cm,
                                                  int channel)
{
    if (!cm) return NULL;

    for (int i = 0; i < cm->count; i++) {
        if (cm->entries[i].channel == channel)
            return &cm->entries[i];
    }
    return NULL;
}

void channel_map_record_capture(channel_map_t *cm, int channel)
{
    if (!cm || channel < 1 || channel > 165) return;

    time_t now = time(NULL);
    cm->productive_ts[channel] = now;
    cm->epoch_captures[channel]++;

    /* Also update the entry if it exists in the current map */
    for (int i = 0; i < cm->count; i++) {
        if (cm->entries[i].channel == channel) {
            cm->entries[i].last_productive = now;
            /* Extend listen window immediately */
            cm->entries[i].listen_ms = CMAP_LISTEN_BASE_MS + CMAP_LISTEN_BONUS_MS;
            break;
        }
    }
}

int channel_map_get_listen_ms(const channel_map_t *cm, int channel)
{
    if (!cm) return CMAP_LISTEN_BASE_MS;

    const channel_map_entry_t *e = channel_map_get_entry(cm, channel);
    if (e)
        return e->listen_ms;

    return CMAP_LISTEN_BASE_MS;
}

void channel_map_dump(const channel_map_t *cm)
{
    if (!cm) return;

    fprintf(stderr, "[channel_map] %d channels (built at %ld):\n",
            cm->count, (long)cm->build_time);

    for (int i = 0; i < cm->count; i++) {
        const channel_map_entry_t *e = &cm->entries[i];
        fprintf(stderr, "  [%d] ch%-3d  aps=%-2d  clients=%-2d  uncap=%-2d  "
                        "yield=%.1f  thompson=%.2f  listen=%dms%s\n",
                i, e->channel, e->ap_count, e->client_count,
                e->uncaptured_count, e->expected_yield,
                e->thompson_score, e->listen_ms,
                e->uncaptured_count == 0 ? "  [SKIP]" : "");
    }
}

void channel_map_destroy(channel_map_t *cm)
{
    if (!cm) return;
    memset(cm, 0, sizeof(*cm));
}
