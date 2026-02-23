/*
 * stationary_mode.c — Stationary Mode Pipeline (Phase 2C)
 *
 * Four-phase attack cycle with diminishing returns detection
 * and per-AP attack type rotation.
 */

#include "stationary_mode.h"
#include <string.h>
#include <stdio.h>

/* ── Internal helpers ────────────────────────────────────────────── */

static stat_ap_tracker_t *find_or_create_tracker(stat_ctx_t *ctx,
                                                  const char *bssid)
{
    /* Search existing */
    for (int i = 0; i < ctx->ap_tracker_count; i++) {
        if (strcasecmp(ctx->ap_trackers[i].bssid, bssid) == 0)
            return &ctx->ap_trackers[i];
    }

    /* Create new if space */
    if (ctx->ap_tracker_count >= STAT_MAX_TRACKED_APS) return NULL;

    stat_ap_tracker_t *t = &ctx->ap_trackers[ctx->ap_tracker_count++];
    memset(t, 0, sizeof(*t));
    strncpy(t->bssid, bssid, 17);
    t->bssid[17] = '\0';
    t->preferred = STAT_ATK_DEAUTH_BIDI;  /* Start with deauth */
    return t;
}

/* ── Init / Session ──────────────────────────────────────────────── */

void stat_init(stat_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

void stat_session_start(stat_ctx_t *ctx)
{
    ctx->session_start = time(NULL);
    ctx->active = true;
    ctx->permanent_soak = false;
    ctx->consecutive_stale = 0;
    ctx->ap_tracker_count = 0;
    ctx->circle_back_count = 0;

    /* Start first cycle */
    stat_new_cycle(ctx);

    /* Begin with scan phase */
    ctx->phase = STAT_PHASE_SCAN;
    ctx->phase_start = time(NULL);
    ctx->phase_duration_sec = STAT_SCAN_SEC;

    fprintf(stderr, "[stationary] session started\n");
}

void stat_session_end(stat_ctx_t *ctx)
{
    ctx->active = false;
    time_t elapsed = time(NULL) - ctx->session_start;
    if (elapsed < 1) elapsed = 1;

    fprintf(stderr, "[stationary] session ended: %us, %d cycles, "
                    "%d captures (%.1f/min)\n",
            (unsigned)elapsed, ctx->cycle.cycle_num,
            ctx->cycle.total_captures_session,
            (float)ctx->cycle.total_captures_session * 60.0f / (float)elapsed);
}

/* ── Phase management ────────────────────────────────────────────── */

const char *stat_phase_name(stat_phase_t phase)
{
    switch (phase) {
    case STAT_PHASE_SCAN:   return "SCAN";
    case STAT_PHASE_BURST:  return "BURST";
    case STAT_PHASE_CIRCLE: return "CIRCLE-BACK";
    case STAT_PHASE_SOAK:   return "SOAK";
    default:                return "UNKNOWN";
    }
}

stat_phase_t stat_advance_phase(stat_ctx_t *ctx)
{
    /* If in permanent soak, stay there */
    if (ctx->permanent_soak) {
        ctx->phase = STAT_PHASE_SOAK;
        ctx->phase_start = time(NULL);
        ctx->phase_duration_sec = STAT_SOAK_SEC * 2;  /* Extended soak */
        return ctx->phase;
    }

    /* Normal phase progression */
    switch (ctx->phase) {
    case STAT_PHASE_SCAN:
        ctx->phase = STAT_PHASE_BURST;
        ctx->phase_duration_sec = stat_get_burst_duration(ctx);
        break;

    case STAT_PHASE_BURST:
        /* Check if there are circle-back candidates */
        if (stat_get_circle_back_count(ctx) > 0) {
            ctx->phase = STAT_PHASE_CIRCLE;
            ctx->phase_duration_sec = STAT_CIRCLE_MAX_SEC;
        } else {
            ctx->phase = STAT_PHASE_SOAK;
            ctx->phase_duration_sec = STAT_SOAK_SEC;
        }
        break;

    case STAT_PHASE_CIRCLE:
        ctx->phase = STAT_PHASE_SOAK;
        ctx->phase_duration_sec = STAT_SOAK_SEC;
        break;

    case STAT_PHASE_SOAK:
        /* End of cycle — check diminishing returns */
        stat_new_cycle(ctx);
        ctx->phase = STAT_PHASE_SCAN;
        ctx->phase_duration_sec = STAT_SCAN_SEC;
        break;

    default:
        ctx->phase = STAT_PHASE_SCAN;
        ctx->phase_duration_sec = STAT_SCAN_SEC;
        break;
    }

    ctx->phase_start = time(NULL);
    fprintf(stderr, "[stationary] phase -> %s (%ds)\n",
            stat_phase_name(ctx->phase), ctx->phase_duration_sec);
    return ctx->phase;
}

bool stat_phase_expired(stat_ctx_t *ctx)
{
    return difftime(time(NULL), ctx->phase_start)
           >= (double)ctx->phase_duration_sec;
}

/* ── Attack type rotation ────────────────────────────────────────── */

stat_attack_type_t stat_get_attack_type(stat_ctx_t *ctx, const char *bssid)
{
    stat_ap_tracker_t *t = find_or_create_tracker(ctx, bssid);
    if (!t) return STAT_ATK_PMKID_ASSOC;  /* Fallback */
    return t->preferred;
}

void stat_record_attack(stat_ctx_t *ctx, const char *bssid,
                        stat_attack_type_t type, bool success)
{
    stat_ap_tracker_t *t = find_or_create_tracker(ctx, bssid);
    if (!t) return;

    t->total_attacks++;
    t->last_attacked = time(NULL);
    ctx->cycle.attacks_this_cycle++;

    if (success) {
        /* Reset fail counter on success */
        t->fail_count[type] = 0;
    } else {
        t->fail_count[type]++;

        /* Rotate if too many consecutive failures */
        if (t->fail_count[type] >= STAT_MAX_FAIL_BEFORE_ROTATE) {
            /* Try next attack type */
            stat_attack_type_t next = (stat_attack_type_t)
                ((int)t->preferred + 1) % STAT_ATK_COUNT;
            t->preferred = next;
            t->rotations++;

            fprintf(stderr, "[stationary] %s: attack rotated to %s "
                            "(fail_count=%d)\n",
                    bssid,
                    next == STAT_ATK_DEAUTH_BIDI ? "DEAUTH_BIDI" :
                    next == STAT_ATK_CSA_BEACON  ? "CSA" :
                    next == STAT_ATK_PMKID_ASSOC ? "PMKID" :
                    next == STAT_ATK_DEAUTH_BCAST ? "DEAUTH_BCAST" :
                    "DISASSOC",
                    t->fail_count[type]);
        }
    }
}

/* ── EAPOL state tracking ────────────────────────────────────────── */

void stat_record_eapol_state(stat_ctx_t *ctx, const char *bssid,
                             bool has_m1, bool has_m2)
{
    stat_ap_tracker_t *t = find_or_create_tracker(ctx, bssid);
    if (!t) return;

    bool was_circle = (t->has_m1 && !t->has_m2);
    t->has_m1 = has_m1;
    t->has_m2 = has_m2;

    if (has_m2) t->captured = true;

    /* Update circle-back count */
    bool is_circle = (t->has_m1 && !t->has_m2);
    if (is_circle && !was_circle)
        ctx->circle_back_count++;
    else if (!is_circle && was_circle && ctx->circle_back_count > 0)
        ctx->circle_back_count--;
}

void stat_record_capture(stat_ctx_t *ctx)
{
    ctx->cycle.captures_this_cycle++;
    ctx->cycle.total_captures_session++;

    /* Update capture rate */
    time_t elapsed = time(NULL) - ctx->cycle.cycle_start;
    if (elapsed > 0) {
        ctx->cycle.capture_rate =
            (float)ctx->cycle.captures_this_cycle * 60.0f / (float)elapsed;
    }

    fprintf(stderr, "[stationary] capture! cycle=%d total=%d rate=%.1f/min\n",
            ctx->cycle.captures_this_cycle,
            ctx->cycle.total_captures_session,
            ctx->cycle.capture_rate);
}

/* ── Diminishing returns ─────────────────────────────────────────── */

bool stat_should_permanent_soak(stat_ctx_t *ctx)
{
    return ctx->permanent_soak;
}

void stat_new_cycle(stat_ctx_t *ctx)
{
    /* Check if previous cycle was stale */
    if (ctx->cycle.cycle_num > 0) {
        if (ctx->cycle.captures_this_cycle == 0) {
            ctx->consecutive_stale++;
            ctx->cycle.stale_cycles = ctx->consecutive_stale;
            fprintf(stderr, "[stationary] stale cycle #%d "
                            "(0 captures, %d consecutive)\n",
                    ctx->cycle.cycle_num, ctx->consecutive_stale);

            if (ctx->consecutive_stale >= STAT_PERM_SOAK_AFTER) {
                ctx->permanent_soak = true;
                fprintf(stderr, "[stationary] PERMANENT SOAK: "
                                "%d consecutive stale cycles\n",
                        ctx->consecutive_stale);
            }
        } else {
            ctx->consecutive_stale = 0;
        }
    }

    ctx->cycle.cycle_num++;
    ctx->cycle.captures_this_cycle = 0;
    ctx->cycle.attacks_this_cycle = 0;
    ctx->cycle.capture_rate = 0.0f;
    ctx->cycle.cycle_start = time(NULL);
}

/* ── Circle-back ─────────────────────────────────────────────────── */

int stat_get_circle_back_count(stat_ctx_t *ctx)
{
    /* Recount from trackers for accuracy */
    int count = 0;
    for (int i = 0; i < ctx->ap_tracker_count; i++) {
        if (ctx->ap_trackers[i].has_m1 && !ctx->ap_trackers[i].has_m2)
            count++;
    }
    ctx->circle_back_count = count;
    return count;
}

bool stat_is_circle_back(stat_ctx_t *ctx, const char *bssid)
{
    for (int i = 0; i < ctx->ap_tracker_count; i++) {
        if (strcasecmp(ctx->ap_trackers[i].bssid, bssid) == 0) {
            return (ctx->ap_trackers[i].has_m1 &&
                    !ctx->ap_trackers[i].has_m2);
        }
    }
    return false;
}

/* ── Burst duration (adaptive) ───────────────────────────────────── */

int stat_get_burst_duration(stat_ctx_t *ctx)
{
    /* If capture rate is high, extend burst */
    if (ctx->cycle.capture_rate >= STAT_AGGRESSIVE_RATE) {
        return STAT_BURST_MAX_SEC;
    }

    /* If stale, shorten burst (spend more time soaking) */
    if (ctx->consecutive_stale > 0) {
        int reduced = STAT_BURST_MIN_SEC -
                      (ctx->consecutive_stale * 5);
        if (reduced < 15) reduced = 15;
        return reduced;
    }

    /* Default: middle ground */
    return (STAT_BURST_MIN_SEC + STAT_BURST_MAX_SEC) / 2;
}

/* ── Diagnostics ─────────────────────────────────────────────────── */

void stat_dump_stats(stat_ctx_t *ctx)
{
    time_t elapsed = time(NULL) - ctx->session_start;
    if (elapsed < 1) elapsed = 1;

    fprintf(stderr, "[stationary] stats: %us, cycle=%d, "
                    "captures=%d (%.1f/min), phase=%s",
            (unsigned)elapsed,
            ctx->cycle.cycle_num,
            ctx->cycle.total_captures_session,
            (float)ctx->cycle.total_captures_session * 60.0f / (float)elapsed,
            stat_phase_name(ctx->phase));

    if (ctx->permanent_soak)
        fprintf(stderr, " [PERM_SOAK]");

    int cb = stat_get_circle_back_count(ctx);
    if (cb > 0)
        fprintf(stderr, " circle_back=%d", cb);

    /* Show rotation stats */
    int rotated = 0;
    for (int i = 0; i < ctx->ap_tracker_count; i++) {
        if (ctx->ap_trackers[i].rotations > 0) rotated++;
    }
    if (rotated > 0)
        fprintf(stderr, " rotated=%d/%d", rotated, ctx->ap_tracker_count);

    fprintf(stderr, "\n");
}
