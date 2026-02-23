/* ==========================================================================
 * walking_mode.c — Phase 2D: Walking mode pipeline implementation
 *
 * Opportunistic hunter: fast 1/6/11 scan, RSSI-peak attacks,
 * proximity alerts, GPS breadcrumbs.
 * ========================================================================== */
#include "walking_mode.h"
#include "rssi_trend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static void timespec_now(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

/* ── Init / session lifecycle ─────────────────────────────────────────────── */

void walk_init(walk_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

void walk_session_start(walk_ctx_t *ctx)
{
    /* Keep learned secondaries across sessions, reset the rest */
    uint8_t saved_sec[WALK_SECONDARY_MAX];
    int saved_sec_count = ctx->secondary_count;
    if (saved_sec_count > 0) {
        memcpy(saved_sec, ctx->secondary_channels,
               (size_t)saved_sec_count * sizeof(uint8_t));
    }

    memset(&ctx->targets, 0, sizeof(ctx->targets));
    ctx->target_count = 0;
    memset(&ctx->prox_alerts, 0, sizeof(ctx->prox_alerts));
    ctx->prox_alert_count = 0;
    ctx->prox_alert_active = false;
    ctx->prox_alert_expires = 0;
    ctx->prox_alert_text[0] = '\0';
    memset(&ctx->breadcrumbs, 0, sizeof(ctx->breadcrumbs));
    ctx->breadcrumb_head = 0;
    ctx->breadcrumb_count = 0;
    memset(&ctx->stats, 0, sizeof(ctx->stats));
    ctx->stats.session_start = time(NULL);
    ctx->cycle_num = 0;
    timespec_now(&ctx->cycle_start);

    /* Restore secondary channels */
    if (saved_sec_count > 0) {
        memcpy(ctx->secondary_channels, saved_sec,
               (size_t)saved_sec_count * sizeof(uint8_t));
        ctx->secondary_count = saved_sec_count;
    }

    ctx->active = true;
    fprintf(stderr, "[walk] Session started\n");
}

void walk_session_end(walk_ctx_t *ctx)
{
    if (!ctx->active) return;
    ctx->active = false;
    walk_dump_stats(ctx);
    fprintf(stderr, "[walk] Session ended\n");
}

/* ── Target tracking ──────────────────────────────────────────────────────── */

static walk_target_t *find_target(walk_ctx_t *ctx, const char *mac)
{
    for (int i = 0; i < ctx->target_count; i++) {
        if (strcasecmp(ctx->targets[i].mac, mac) == 0)
            return &ctx->targets[i];
    }
    return NULL;
}

walk_target_t *walk_update_target(walk_ctx_t *ctx, const char *mac,
                                  const char *ssid, int8_t rssi,
                                  uint8_t channel, uint8_t clients,
                                  bool has_handshake)
{
    walk_target_t *t = find_target(ctx, mac);

    if (!t) {
        /* New AP — track it if we have room */
        if (ctx->target_count >= WALK_MAX_TRACKED) {
            /* Evict weakest target */
            int weakest = 0;
            for (int i = 1; i < ctx->target_count; i++) {
                if (ctx->targets[i].rssi < ctx->targets[weakest].rssi)
                    weakest = i;
            }
            /* Only evict if new AP is stronger */
            if (rssi <= ctx->targets[weakest].rssi)
                return NULL;
            t = &ctx->targets[weakest];
        } else {
            t = &ctx->targets[ctx->target_count++];
        }

        memset(t, 0, sizeof(*t));
        strncpy(t->mac, mac, WALK_TARGET_MAC_LEN - 1);
        ctx->stats.unique_aps_seen++;
    }

    /* Update fields */
    if (ssid && ssid[0])
        strncpy(t->ssid, ssid, WALK_TARGET_SSID_LEN - 1);
    t->rssi = rssi;
    if (rssi > t->peak_rssi || t->peak_rssi == 0)
        t->peak_rssi = rssi;
    t->channel = channel;
    t->clients = clients;
    t->has_handshake = has_handshake;
    t->last_seen = time(NULL);

    return t;
}

/* ── Attack strategy from RSSI trend ──────────────────────────────────────── */

walk_attack_strategy_t walk_get_strategy(int rssi_trend_enum)
{
    switch (rssi_trend_enum) {
    case RSSI_TREND_APPROACHING:
        return WALK_ATTACK_PMKID_ONLY;
    case RSSI_TREND_PEAK:
        return WALK_ATTACK_FULL_BURST;
    case RSSI_TREND_DEPARTING:
        return WALK_ATTACK_FINAL_SHOT;
    default:
        return WALK_ATTACK_PMKID_ONLY;  /* UNKNOWN → safe default */
    }
}

/* ── Target selection (top N by score) ────────────────────────────────────── */

/* Scoring: strong RSSI + clients + triage + not-yet-captured */
static float score_target(const walk_target_t *t)
{
    if (t->has_handshake) return -1.0f;  /* Already captured */
    if (t->rssi < -85) return -1.0f;      /* Too weak to bother */

    float s = 0.0f;

    /* RSSI component: 0-40 points, -30dBm=40, -85dBm=0 */
    float rssi_norm = (float)(t->rssi + 85) / 55.0f;
    if (rssi_norm > 1.0f) rssi_norm = 1.0f;
    if (rssi_norm < 0.0f) rssi_norm = 0.0f;
    s += rssi_norm * 40.0f;

    /* Client boost: 0-20 points */
    if (t->clients > 0) {
        float cli = (float)t->clients;
        if (cli > 5.0f) cli = 5.0f;
        s += (cli / 5.0f) * 20.0f;
    }

    /* Triage score pass-through: 0-20 points */
    s += t->triage_score * 20.0f;

    /* Freshness: recently seen = more points */
    time_t age = time(NULL) - t->last_seen;
    if (age < 5) s += 15.0f;        /* Just now */
    else if (age < 30) s += 10.0f;   /* Recent */
    else if (age < 120) s += 5.0f;   /* A couple minutes ago */

    /* Penalize if already attacked at peak */
    if (t->attacked_at_peak) s -= 10.0f;
    if (t->final_shot_done) s -= 20.0f;

    return s;
}

int walk_select_targets(walk_ctx_t *ctx, walk_target_t *out, int max)
{
    /* Score all targets */
    typedef struct { int idx; float score; } scored_t;
    scored_t scored[WALK_MAX_TRACKED];
    int valid = 0;

    for (int i = 0; i < ctx->target_count; i++) {
        float s = score_target(&ctx->targets[i]);
        if (s > 0.0f) {
            scored[valid].idx = i;
            scored[valid].score = s;
            valid++;
        }
    }

    /* Sort (simple selection sort — small N) */
    for (int i = 0; i < valid - 1; i++) {
        int best = i;
        for (int j = i + 1; j < valid; j++) {
            if (scored[j].score > scored[best].score)
                best = j;
        }
        if (best != i) {
            scored_t tmp = scored[i];
            scored[i] = scored[best];
            scored[best] = tmp;
        }
    }

    /* Copy top N */
    int n = valid < max ? valid : max;
    for (int i = 0; i < n; i++) {
        out[i] = ctx->targets[scored[i].idx];
    }

    return n;
}

/* ── Proximity alert system ───────────────────────────────────────────────── */

static bool prox_recently_alerted(const walk_ctx_t *ctx, const char *mac)
{
    time_t now = time(NULL);
    for (int i = 0; i < ctx->prox_alert_count; i++) {
        if (strcasecmp(ctx->prox_alerts[i].mac, mac) == 0) {
            if (now - ctx->prox_alerts[i].triggered_at < WALK_PROX_COOLDOWN_S)
                return true;
        }
    }
    return false;
}

bool walk_check_proximity(walk_ctx_t *ctx, const char *mac,
                          const char *ssid, int8_t rssi, uint8_t clients)
{
    /* Must meet threshold */
    if (rssi < WALK_PROX_RSSI_MIN) return false;
    if (clients < WALK_PROX_MIN_CLIENTS) return false;

    /* Cooldown check */
    if (prox_recently_alerted(ctx, mac)) return false;

    /* Must be a new/unknown AP (not already tracked with handshake) */
    walk_target_t *t = find_target(ctx, mac);
    if (t && t->has_handshake) return false;

    /* Fire alert! */
    walk_proximity_alert_t *a = NULL;

    /* Reuse existing slot or add new */
    for (int i = 0; i < ctx->prox_alert_count; i++) {
        if (strcasecmp(ctx->prox_alerts[i].mac, mac) == 0) {
            a = &ctx->prox_alerts[i];
            break;
        }
    }
    if (!a) {
        if (ctx->prox_alert_count < WALK_PROX_MAX_ALERTS) {
            a = &ctx->prox_alerts[ctx->prox_alert_count++];
        } else {
            /* Overwrite oldest */
            time_t oldest = ctx->prox_alerts[0].triggered_at;
            int oldest_idx = 0;
            for (int i = 1; i < ctx->prox_alert_count; i++) {
                if (ctx->prox_alerts[i].triggered_at < oldest) {
                    oldest = ctx->prox_alerts[i].triggered_at;
                    oldest_idx = i;
                }
            }
            a = &ctx->prox_alerts[oldest_idx];
        }
    }

    strncpy(a->mac, mac, WALK_TARGET_MAC_LEN - 1);
    a->mac[WALK_TARGET_MAC_LEN - 1] = '\0';
    if (ssid && ssid[0])
        strncpy(a->ssid, ssid, WALK_TARGET_SSID_LEN - 1);
    a->ssid[WALK_TARGET_SSID_LEN - 1] = '\0';
    a->rssi = rssi;
    a->clients = clients;
    a->triggered_at = time(NULL);
    a->active = true;

    /* Build display text */
    snprintf(ctx->prox_alert_text, sizeof(ctx->prox_alert_text),
             "[!] JACKPOT: %s %ddBm %dc",
             (ssid && ssid[0]) ? ssid : "???",
             (int)rssi, (int)clients);

    ctx->prox_alert_active = true;
    ctx->prox_alert_expires = time(NULL) + (WALK_PROX_DISPLAY_MS / 1000);
    ctx->stats.proximity_alerts++;

    fprintf(stderr, "[walk] PROXIMITY ALERT: %s (%s) RSSI=%d clients=%d\n",
            ssid ? ssid : "???", mac, (int)rssi, (int)clients);

    return true;
}

const char *walk_get_proximity_text(const walk_ctx_t *ctx)
{
    if (!ctx->prox_alert_active) return NULL;
    return ctx->prox_alert_text;
}

bool walk_proximity_tick(walk_ctx_t *ctx)
{
    if (!ctx->prox_alert_active) return false;

    if (time(NULL) >= ctx->prox_alert_expires) {
        ctx->prox_alert_active = false;
        ctx->prox_alert_text[0] = '\0';
        return false;
    }
    return true;
}

/* ── GPS breadcrumbs ──────────────────────────────────────────────────────── */

void walk_breadcrumb(walk_ctx_t *ctx, double lat, double lon,
                     uint16_t ap_count)
{
    if (lat == 0.0 && lon == 0.0) return;  /* No fix */

    walk_breadcrumb_t *b = &ctx->breadcrumbs[ctx->breadcrumb_head];
    b->lat = lat;
    b->lon = lon;
    b->ap_count = ap_count;
    b->captures = (uint16_t)ctx->stats.handshakes;
    b->timestamp = time(NULL);

    ctx->breadcrumb_head = (ctx->breadcrumb_head + 1) % WALK_BREADCRUMB_MAX;
    if (ctx->breadcrumb_count < WALK_BREADCRUMB_MAX)
        ctx->breadcrumb_count++;
}

/* ── Recording attacks / captures ─────────────────────────────────────────── */

void walk_record_attack(walk_ctx_t *ctx, const char *mac,
                        walk_attack_strategy_t strategy)
{
    ctx->stats.attacks_sent++;

    walk_target_t *t = find_target(ctx, mac);
    if (!t) return;

    t->last_attacked = time(NULL);

    switch (strategy) {
    case WALK_ATTACK_PMKID_ONLY:
        t->pmkid_attempted = true;
        ctx->stats.pmkid_attempts++;
        break;
    case WALK_ATTACK_FULL_BURST:
        t->attacked_at_peak = true;
        ctx->stats.peak_attacks++;
        break;
    case WALK_ATTACK_FINAL_SHOT:
        t->final_shot_done = true;
        ctx->stats.final_shots++;
        break;
    }
}

void walk_record_capture(walk_ctx_t *ctx, const char *mac)
{
    ctx->stats.handshakes++;

    walk_target_t *t = find_target(ctx, mac);
    if (t) {
        t->has_handshake = true;
    }
}

/* ── Secondary channel learning ───────────────────────────────────────────── */

void walk_learn_secondary_channels(walk_ctx_t *ctx,
                                   const uint8_t *channels,
                                   const float *scores,
                                   int count)
{
    ctx->secondary_count = 0;

    for (int i = 0; i < count && ctx->secondary_count < WALK_SECONDARY_MAX; i++) {
        /* Skip primary channels */
        if (channels[i] == WALK_PRIMARY_CH_1 ||
            channels[i] == WALK_PRIMARY_CH_6 ||
            channels[i] == WALK_PRIMARY_CH_11)
            continue;

        /* Only add if Thompson score > threshold */
        if (scores[i] >= WALK_SECONDARY_THRESHOLD) {
            ctx->secondary_channels[ctx->secondary_count++] = channels[i];
        }
    }

    if (ctx->secondary_count > 0) {
        fprintf(stderr, "[walk] Learned %d secondary channels:",
                ctx->secondary_count);
        for (int i = 0; i < ctx->secondary_count; i++) {
            fprintf(stderr, " %d", ctx->secondary_channels[i]);
        }
        fprintf(stderr, "\n");
    }
}

int walk_get_scan_channels(const walk_ctx_t *ctx, uint8_t *out, int max)
{
    int n = 0;

    /* Always scan primary channels first */
    if (n < max) out[n++] = WALK_PRIMARY_CH_1;
    if (n < max) out[n++] = WALK_PRIMARY_CH_6;
    if (n < max) out[n++] = WALK_PRIMARY_CH_11;

    /* Then productive secondaries */
    for (int i = 0; i < ctx->secondary_count && n < max; i++) {
        out[n++] = ctx->secondary_channels[i];
    }

    return n;
}

/* ── Diagnostics ──────────────────────────────────────────────────────────── */

void walk_dump_stats(const walk_ctx_t *ctx)
{
    time_t elapsed = time(NULL) - ctx->stats.session_start;
    if (elapsed < 1) elapsed = 1;

    float rate_per_min = (float)ctx->stats.handshakes / ((float)elapsed / 60.0f);

    fprintf(stderr,
        "[walk] ── Walking Session Stats ──\n"
        "[walk]  Duration:    %ld min %ld sec\n"
        "[walk]  Epochs:      %u\n"
        "[walk]  Scans:       %u\n"
        "[walk]  Unique APs:  %u\n"
        "[walk]  Attacks:     %u (peak=%u, final=%u, pmkid=%u)\n"
        "[walk]  Captures:    %u (%.1f/min)\n"
        "[walk]  Prox alerts: %u\n"
        "[walk]  Breadcrumbs: %d\n"
        "[walk] ─────────────────────────────\n",
        (long)(elapsed / 60), (long)(elapsed % 60),
        ctx->stats.epochs,
        ctx->stats.scans,
        ctx->stats.unique_aps_seen,
        ctx->stats.attacks_sent, ctx->stats.peak_attacks,
        ctx->stats.final_shots, ctx->stats.pmkid_attempts,
        ctx->stats.handshakes, rate_per_min,
        ctx->stats.proximity_alerts,
        ctx->breadcrumb_count);
}
