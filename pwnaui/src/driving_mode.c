/*
 * driving_mode.c — Driving Mode Pipeline (Phase 2B)
 *
 * PMKID spray + Map Blitz for in-vehicle operation.
 * 50ms channel hops, one association per channel per sweep,
 * breadcrumb trail for every AP sighted.
 */

#include "driving_mode.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Init / Session ──────────────────────────────────────────────── */

void drv_init(drv_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

void drv_session_start(drv_ctx_t *ctx)
{
    /* Preserve lifetime stats, reset session stats */
    ctx->session_start = time(NULL);
    ctx->total_sweeps = 0;
    ctx->total_associations = 0;
    ctx->total_pmkid_captures = 0;
    ctx->total_aps_logged = 0;
    ctx->total_new_aps = 0;
    ctx->sweeps_this_cycle = 0;
    ctx->pmkid_this_cycle = 0;
    ctx->cycle_start = time(NULL);
    ctx->sweep_rate = 0.0f;
    ctx->pmkid_rate = 0.0f;
    ctx->breadcrumb_count = 0;
    ctx->breadcrumb_write_idx = 0;
    ctx->active = true;

    /* Reset per-channel state */
    for (int i = 0; i <= DRV_MAX_CHANNELS; i++) {
        ctx->channels[i].associated = false;
        ctx->channels[i].assoc_bssid[0] = '\0';
        ctx->channels[i].strongest_rssi = -100;
        ctx->channels[i].ap_count = 0;
        /* Keep lifetime totals: total_associations, pmkid_captures */
    }

    fprintf(stderr, "[driving] session started\n");
}

void drv_session_end(drv_ctx_t *ctx)
{
    if (!ctx->active) return;  /* Not started — don't print bogus stats */
    ctx->active = false;
    time_t elapsed = time(NULL) - ctx->session_start;
    if (elapsed < 1) elapsed = 1;

    fprintf(stderr, "[driving] session ended: %us, %u sweeps, "
                    "%u assocs, %u PMKIDs, %u APs logged\n",
            (unsigned)elapsed, ctx->total_sweeps,
            ctx->total_associations, ctx->total_pmkid_captures,
            ctx->total_aps_logged);
}

/* ── Sweep management ────────────────────────────────────────────── */

void drv_sweep_begin(drv_ctx_t *ctx)
{
    ctx->sweep.sweep_num++;
    ctx->sweep.channel_idx = 0;
    ctx->sweep.sweep_start = time(NULL);
    ctx->sweep.assoc_this_sweep = 0;
    ctx->sweep.aps_this_sweep = 0;

    /* Reset per-channel per-sweep state */
    for (int i = 0; i <= DRV_MAX_CHANNELS; i++) {
        ctx->channels[i].associated = false;
        ctx->channels[i].assoc_bssid[0] = '\0';
        ctx->channels[i].strongest_rssi = -100;
        ctx->channels[i].ap_count = 0;
    }
}

int drv_enter_channel(drv_ctx_t *ctx, int channel)
{
    if (channel < 1 || channel > DRV_MAX_CHANNELS) return DRV_HOP_MS;

    ctx->sweep.channel_idx++;

    /* If we previously associated on this channel in an earlier sweep,
     * give it extra listen time for PMKID harvesting */
    if (drv_should_harvest(ctx, channel)) {
        return DRV_HOP_MS + DRV_PMKID_LISTEN_MS;
    }

    return DRV_HOP_MS;
}

void drv_sweep_end(drv_ctx_t *ctx)
{
    ctx->total_sweeps++;
    ctx->sweeps_this_cycle++;
    ctx->last_sweep_time = time(NULL);

    /* Update rolling sweep rate */
    time_t elapsed = time(NULL) - ctx->session_start;
    if (elapsed > 0) {
        ctx->sweep_rate = (float)ctx->total_sweeps / (float)elapsed;
    }
}

/* ── Channel operations ──────────────────────────────────────────── */

bool drv_should_associate(drv_ctx_t *ctx, int channel,
                          int8_t rssi, bool has_handshake,
                          bool pmkid_available)
{
    if (channel < 1 || channel > DRV_MAX_CHANNELS) return false;

    /* Already associated on this channel this sweep */
    if (ctx->channels[channel].associated) return false;

    /* Already captured — don't waste time */
    if (has_handshake) return false;

    /* Already have PMKID — skip unless signal is very strong */
    if (pmkid_available && rssi < -45) return false;

    /* Too weak to bother while driving */
    if (rssi < -75) return false;

    /* Track strongest AP seen on channel */
    if (rssi > ctx->channels[channel].strongest_rssi) {
        ctx->channels[channel].strongest_rssi = rssi;
    }

    return true;
}

void drv_record_association(drv_ctx_t *ctx, int channel,
                            const char *bssid, int8_t rssi)
{
    if (channel < 1 || channel > DRV_MAX_CHANNELS) return;

    ctx->channels[channel].associated = true;
    strncpy(ctx->channels[channel].assoc_bssid, bssid,
            DRV_BSSID_LEN - 1);
    ctx->channels[channel].assoc_bssid[DRV_BSSID_LEN - 1] = '\0';
    ctx->channels[channel].strongest_rssi = rssi;
    ctx->channels[channel].total_associations++;

    ctx->sweep.assoc_this_sweep++;
    ctx->total_associations++;
}

void drv_record_pmkid(drv_ctx_t *ctx, int channel)
{
    if (channel >= 1 && channel <= DRV_MAX_CHANNELS) {
        ctx->channels[channel].pmkid_captures++;
    }
    ctx->total_pmkid_captures++;
    ctx->pmkid_this_cycle++;

    /* Update PMKID rate */
    time_t elapsed = time(NULL) - ctx->session_start;
    if (elapsed > 0) {
        ctx->pmkid_rate = (float)ctx->total_pmkid_captures * 60.0f
                          / (float)elapsed;
    }

    fprintf(stderr, "[driving] PMKID captured on ch%d "
                    "(total: %u, rate: %.1f/min)\n",
            channel, ctx->total_pmkid_captures, ctx->pmkid_rate);
}

/* ── Breadcrumb trail ────────────────────────────────────────────── */

void drv_add_breadcrumb(drv_ctx_t *ctx,
                        const char *bssid, const char *ssid,
                        int8_t rssi, uint8_t channel,
                        double lat, double lon,
                        bool associated)
{
    int idx = ctx->breadcrumb_write_idx;
    drv_breadcrumb_t *bc = &ctx->breadcrumbs[idx];

    strncpy(bc->bssid, bssid, DRV_BSSID_LEN - 1);
    bc->bssid[DRV_BSSID_LEN - 1] = '\0';
    strncpy(bc->ssid, ssid, 32);
    bc->ssid[32] = '\0';
    bc->rssi = rssi;
    bc->channel = channel;
    bc->lat = lat;
    bc->lon = lon;
    bc->timestamp = time(NULL);
    bc->associated = associated;
    bc->pmkid_captured = false;

    ctx->breadcrumb_write_idx =
        (ctx->breadcrumb_write_idx + 1) % DRV_BREADCRUMB_MAX;
    if (ctx->breadcrumb_count < DRV_BREADCRUMB_MAX)
        ctx->breadcrumb_count++;

    ctx->total_aps_logged++;
    ctx->sweep.aps_this_sweep++;
    if (channel >= 1 && channel <= DRV_MAX_CHANNELS)
        ctx->channels[channel].ap_count++;
}

/* ── Cycle management ────────────────────────────────────────────── */

bool drv_check_cycle(drv_ctx_t *ctx)
{
    time_t now = time(NULL);
    if (difftime(now, ctx->cycle_start) >= (double)DRV_CYCLE_SEC) {
        if (ctx->sweeps_this_cycle > 0) {
            fprintf(stderr, "[driving] cycle: %d sweeps, %d PMKIDs "
                            "(target: %d sweeps)\n",
                    ctx->sweeps_this_cycle, ctx->pmkid_this_cycle,
                    DRV_SWEEPS_PER_CYCLE);
        }
        ctx->sweeps_this_cycle = 0;
        ctx->pmkid_this_cycle = 0;
        ctx->cycle_start = now;
        return true;
    }
    return false;
}

/* ── Channel ordering ────────────────────────────────────────────── */

void drv_get_channel_order(drv_ctx_t *ctx, int *channels, int *count)
{
    (void)ctx;

    /* Start with standard 2.4GHz channel order */
    int order[DRV_MAX_CHANNELS] = {1, 6, 11, 2, 7, 12, 3, 8, 13, 4, 9, 14, 5, 10};
    *count = DRV_MAX_CHANNELS;

    /* Non-overlapping channels first (1, 6, 11), then fill in.
     * This maximizes coverage per unit time. */
    for (int i = 0; i < DRV_MAX_CHANNELS; i++) {
        channels[i] = order[i];
    }

    /* Simple shuffle of the secondary channels (indices 3-13)
     * to avoid predictable patterns that could trigger WIDS.
     * Use sweep number as seed for deterministic but varying order. */
    unsigned int seed = (unsigned int)(ctx->sweep.sweep_num + 42);
    for (int i = DRV_MAX_CHANNELS - 1; i > 3; i--) {
        seed = seed * 1103515245 + 12345;
        int j = 3 + (int)((seed >> 16) % (unsigned int)(i - 2));
        int tmp = channels[i];
        channels[i] = channels[j];
        channels[j] = tmp;
    }
}

/* ── PMKID harvesting ────────────────────────────────────────────── */

bool drv_should_harvest(drv_ctx_t *ctx, int channel)
{
    if (channel < 1 || channel > DRV_MAX_CHANNELS) return false;

    /* If we've ever associated on this channel (in any sweep)
     * and we haven't captured the PMKID yet, listen longer */
    return (ctx->channels[channel].total_associations > 0 &&
            ctx->channels[channel].pmkid_captures == 0);
}

/* ── Diagnostics ─────────────────────────────────────────────────── */

void drv_dump_stats(drv_ctx_t *ctx)
{
    time_t elapsed = time(NULL) - ctx->session_start;
    if (elapsed < 1) elapsed = 1;

    fprintf(stderr, "[driving] stats: %us elapsed, %u sweeps (%.1f/s), "
                    "%u assocs, %u PMKIDs (%.1f/min), %u APs logged\n",
            (unsigned)elapsed,
            ctx->total_sweeps, ctx->sweep_rate,
            ctx->total_associations,
            ctx->total_pmkid_captures, ctx->pmkid_rate,
            ctx->total_aps_logged);

    /* Per-channel summary */
    fprintf(stderr, "[driving] channels:");
    for (int ch = 1; ch <= DRV_MAX_CHANNELS; ch++) {
        if (ctx->channels[ch].total_associations > 0 ||
            ctx->channels[ch].pmkid_captures > 0) {
            fprintf(stderr, " ch%d(%ua/%up)",
                    ch,
                    ctx->channels[ch].total_associations,
                    ctx->channels[ch].pmkid_captures);
        }
    }
    fprintf(stderr, "\n");
}
