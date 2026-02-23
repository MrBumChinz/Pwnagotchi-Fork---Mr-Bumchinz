/*
 * ap_triage.c — AP Triage System (Phase 2A)
 *
 * Classifies access points into Gold/Silver/Bronze/Exploit/Skip tiers
 * and allocates attack time budgets accordingly.
 */

#include "ap_triage.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ── Internal helpers ────────────────────────────────────────────── */

/**
 * Check if encryption string indicates WPA2.
 */
static bool is_wpa2(const char *enc)
{
    if (!enc) return false;
    /* Match "WPA2", "WPA2-CCMP", "WPA2/WPA3", etc. */
    return (strstr(enc, "WPA2") != NULL);
}

/**
 * Check if encryption is open/unencrypted.
 */
static bool is_open(const char *enc)
{
    if (!enc || enc[0] == '\0') return true;
    return (strcmp(enc, "OPEN") == 0 || strcmp(enc, "OPN") == 0);
}

/**
 * Compute freshness factor (0.0 = stale, 1.0 = fresh/never attacked).
 * Linear decay over TRIAGE_FRESHNESS_WINDOW seconds.
 */
static float freshness_factor(time_t last_attacked, time_t now)
{
    if (last_attacked == 0) return 1.0f;  /* Never attacked = fresh */
    double elapsed = difftime(now, last_attacked);
    if (elapsed < 0) elapsed = 0;
    if (elapsed >= (double)TRIAGE_FRESHNESS_WINDOW) return 1.0f;
    return (float)(elapsed / (double)TRIAGE_FRESHNESS_WINDOW);
}

/**
 * Compute Thompson success probability estimate.
 * Returns alpha / (alpha + beta), clamped to [0, 1].
 */
static float thompson_success_rate(float alpha, float beta)
{
    float total = alpha + beta;
    if (total < 0.01f) return 0.5f;  /* No data = neutral */
    float rate = alpha / total;
    if (rate < 0.0f) rate = 0.0f;
    if (rate > 1.0f) rate = 1.0f;
    return rate;
}

/**
 * Normalize RSSI to 0.0..1.0 range.
 * -30 dBm = 1.0 (excellent), -90 dBm = 0.0 (terrible).
 */
static float normalize_rssi(int8_t rssi)
{
    float r = (float)(rssi + 90) / 60.0f;
    if (r < 0.0f) r = 0.0f;
    if (r > 1.0f) r = 1.0f;
    return r;
}

/**
 * Normalize client count to 0.0..1.0 range.
 * 0 = 0.0, 5+ = 1.0, linear between.
 */
static float normalize_clients(uint32_t count)
{
    if (count == 0) return 0.0f;
    if (count >= 5) return 1.0f;
    return (float)count / 5.0f;
}

/* ── Classification ──────────────────────────────────────────────── */

void ap_triage_classify(const ap_triage_input_t *input,
                        ap_triage_result_t *result)
{
    /* Default */
    result->tier = TRIAGE_BRONZE;
    result->priority_score = 0.0f;
    result->time_budget_pct = TRIAGE_BUDGET_BRONZE;
    result->reason = "default";

    /* ── SKIP tier: hard filters ─────────────────────────────────── */

    if (input->is_whitelisted) {
        result->tier = TRIAGE_SKIP;
        result->time_budget_pct = TRIAGE_BUDGET_SKIP;
        result->priority_score = 0.0f;
        result->reason = "whitelisted";
        return;
    }

    if (input->is_blacklisted) {
        result->tier = TRIAGE_SKIP;
        result->time_budget_pct = TRIAGE_BUDGET_SKIP;
        result->priority_score = 0.0f;
        result->reason = "blacklisted";
        return;
    }

    /* Already fully captured = skip */
    if (input->handshake_captured && input->got_handshake) {
        result->tier = TRIAGE_SKIP;
        result->time_budget_pct = TRIAGE_BUDGET_SKIP;
        result->priority_score = 0.0f;
        result->reason = "already captured";
        return;
    }

    /* Open/WEP = skip (no WPA handshake to capture) */
    if (is_open(input->encryption)) {
        result->tier = TRIAGE_SKIP;
        result->time_budget_pct = TRIAGE_BUDGET_SKIP;
        result->priority_score = 0.0f;
        result->reason = "open network";
        return;
    }

    /* Too weak to bother */
    if (input->rssi < TRIAGE_RSSI_BRONZE_MIN) {
        result->tier = TRIAGE_SKIP;
        result->time_budget_pct = TRIAGE_BUDGET_SKIP;
        result->priority_score = 0.0f;
        result->reason = "signal too weak";
        return;
    }

    /* ── EXPLOIT tier: PMKID available but no full 4-way ─────────── */

    if (input->pmkid_available && !input->handshake_captured) {
        result->tier = TRIAGE_EXPLOIT;
        result->time_budget_pct = TRIAGE_BUDGET_EXPLOIT;
        result->priority_score = ap_triage_score(input);
        result->reason = "PMKID opportunity";
        return;
    }

    /* ── GOLD tier ───────────────────────────────────────────────── */

    if (input->rssi > TRIAGE_RSSI_GOLD &&
        input->clients_count >= (uint32_t)TRIAGE_CLIENTS_MIN &&
        !input->is_wpa3 &&
        is_wpa2(input->encryption) &&
        !input->got_handshake) {

        result->tier = TRIAGE_GOLD;
        result->time_budget_pct = TRIAGE_BUDGET_GOLD;
        result->priority_score = ap_triage_score(input);
        result->reason = "strong WPA2, clients, uncaptured";
        return;
    }

    /* Near-gold: strong signal + clients but WPA2/WPA3 mixed */
    if (input->rssi > TRIAGE_RSSI_GOLD &&
        input->clients_count >= (uint32_t)TRIAGE_CLIENTS_MIN &&
        !input->got_handshake &&
        !input->is_wpa3) {

        result->tier = TRIAGE_GOLD;
        result->time_budget_pct = TRIAGE_BUDGET_GOLD;
        result->priority_score = ap_triage_score(input) * 0.9f;
        result->reason = "strong signal, clients, uncaptured";
        return;
    }

    /* ── SILVER tier ─────────────────────────────────────────────── */

    if (input->rssi >= TRIAGE_RSSI_SILVER_MIN &&
        input->clients_count >= (uint32_t)TRIAGE_CLIENTS_MIN &&
        !input->got_handshake) {

        result->tier = TRIAGE_SILVER;
        result->time_budget_pct = TRIAGE_BUDGET_SILVER;
        result->priority_score = ap_triage_score(input);
        result->reason = input->is_wpa3 ? "moderate WPA3 with clients"
                                        : "moderate WPA2 with clients";
        return;
    }

    /* Silver: decent signal, no clients but high Thompson success */
    if (input->rssi >= TRIAGE_RSSI_SILVER_MIN &&
        !input->got_handshake &&
        (input->thompson_alpha + input->thompson_beta) >= TRIAGE_THOMPSON_COLD &&
        thompson_success_rate(input->thompson_alpha, input->thompson_beta) > 0.4f) {

        result->tier = TRIAGE_SILVER;
        result->time_budget_pct = TRIAGE_BUDGET_SILVER;
        result->priority_score = ap_triage_score(input) * 0.85f;
        result->reason = "Thompson history suggests success";
        return;
    }

    /* ── BRONZE tier (default for everything else) ───────────────── */

    result->tier = TRIAGE_BRONZE;
    result->time_budget_pct = TRIAGE_BUDGET_BRONZE;
    result->priority_score = ap_triage_score(input);

    if (input->is_wpa3)
        result->reason = "WPA3 — low success probability";
    else if (input->clients_count == 0)
        result->reason = "no clients — deauth ineffective";
    else if (input->rssi < TRIAGE_RSSI_SILVER_MIN)
        result->reason = "weak signal";
    else if (input->got_handshake)
        result->reason = "partial capture exists";
    else
        result->reason = "default low priority";
}

/* ── Priority scoring ────────────────────────────────────────────── */

float ap_triage_score(const ap_triage_input_t *input)
{
    float rssi_norm   = normalize_rssi(input->rssi);
    float client_norm = normalize_clients(input->clients_count);
    float thompson_r  = thompson_success_rate(input->thompson_alpha,
                                              input->thompson_beta);
    float fresh       = freshness_factor(input->last_attacked, input->now);

    /* Weighted combination */
    float score = (TRIAGE_W_RSSI      * rssi_norm)
                + (TRIAGE_W_CLIENTS   * client_norm)
                + (TRIAGE_W_THOMPSON  * thompson_r)
                + (TRIAGE_W_FRESHNESS * fresh);

    /* Bonus for PMKID availability (quick capture) */
    if (input->pmkid_available && !input->handshake_captured)
        score += 0.15f;

    /* Penalty for high deauth count (diminishing returns) */
    if (input->deauth_count > 10)
        score *= 0.85f;
    else if (input->deauth_count > 20)
        score *= 0.70f;

    /* Clamp */
    if (score < 0.0f) score = 0.0f;
    if (score > 1.0f) score = 1.0f;

    return score;
}

/* ── Budget allocation ───────────────────────────────────────────── */

float ap_triage_budget(ap_triage_tier_t tier)
{
    switch (tier) {
    case TRIAGE_GOLD:    return TRIAGE_BUDGET_GOLD;
    case TRIAGE_SILVER:  return TRIAGE_BUDGET_SILVER;
    case TRIAGE_BRONZE:  return TRIAGE_BUDGET_BRONZE;
    case TRIAGE_EXPLOIT: return TRIAGE_BUDGET_EXPLOIT;
    case TRIAGE_SKIP:    return TRIAGE_BUDGET_SKIP;
    default:             return 0.0f;
    }
}

float ap_triage_allocate_time(ap_triage_tier_t tier,
                              int tier_rank,
                              int tier_count,
                              float total_budget_s)
{
    if (tier == TRIAGE_SKIP || tier_count <= 0)
        return 0.0f;

    float tier_budget_s = total_budget_s * ap_triage_budget(tier);
    float per_ap = tier_budget_s / (float)tier_count;

    /* Front-load: higher-ranked APs get more time */
    /* Linear taper: rank 0 gets 1.5x, last rank gets 0.5x */
    float rank_factor = 1.0f;
    if (tier_count > 1) {
        float pos = (float)tier_rank / (float)(tier_count - 1);
        rank_factor = 1.5f - pos;  /* 1.5 at rank 0, 0.5 at last */
    }

    float allocated = per_ap * rank_factor;

    /* Minimum 1 second if allocated at all */
    if (allocated < 1.0f && allocated > 0.0f)
        allocated = 1.0f;

    return allocated;
}

/* ── Cooldown ────────────────────────────────────────────────────── */

bool ap_triage_in_cooldown(time_t last_attacked, time_t now)
{
    if (last_attacked == 0) return false;
    return difftime(now, last_attacked) < (double)TRIAGE_COOLDOWN_SEC;
}

/* ── Tier name ───────────────────────────────────────────────────── */

const char *ap_triage_tier_name(ap_triage_tier_t tier)
{
    switch (tier) {
    case TRIAGE_GOLD:    return "GOLD";
    case TRIAGE_SILVER:  return "SILVER";
    case TRIAGE_BRONZE:  return "BRONZE";
    case TRIAGE_EXPLOIT: return "EXPLOIT";
    case TRIAGE_SKIP:    return "SKIP";
    default:             return "UNKNOWN";
    }
}

/* ── Summary tracking ────────────────────────────────────────────── */

void ap_triage_summary_init(ap_triage_summary_t *s)
{
    memset(s, 0, sizeof(*s));
}

void ap_triage_summary_add(ap_triage_summary_t *s,
                           const ap_triage_result_t *result,
                           int8_t rssi)
{
    s->total++;

    switch (result->tier) {
    case TRIAGE_GOLD:
        s->gold++;
        s->gold_rssi_sum += rssi;
        break;
    case TRIAGE_SILVER:
        s->silver++;
        s->silver_rssi_sum += rssi;
        break;
    case TRIAGE_BRONZE:
        s->bronze++;
        break;
    case TRIAGE_EXPLOIT:
        s->exploit++;
        break;
    case TRIAGE_SKIP:
        s->skip++;
        break;
    }
}

void ap_triage_summary_finalize(ap_triage_summary_t *s)
{
    if (s->gold > 0)
        s->avg_gold_rssi = (float)s->gold_rssi_sum / (float)s->gold;
    else
        s->avg_gold_rssi = 0.0f;

    if (s->silver > 0)
        s->avg_silver_rssi = (float)s->silver_rssi_sum / (float)s->silver;
    else
        s->avg_silver_rssi = 0.0f;
}

void ap_triage_summary_dump(const ap_triage_summary_t *s)
{
    fprintf(stderr, "[triage] G:%d S:%d B:%d X:%d skip:%d total:%d",
            s->gold, s->silver, s->bronze, s->exploit, s->skip, s->total);

    if (s->gold > 0)
        fprintf(stderr, " avg_gold_rssi:%.0f", s->avg_gold_rssi);
    if (s->silver > 0)
        fprintf(stderr, " avg_silver_rssi:%.0f", s->avg_silver_rssi);

    fprintf(stderr, "\n");
}

/* ── Batch operations ────────────────────────────────────────────── */

void ap_triage_batch_init(ap_triage_batch_t *batch)
{
    memset(batch, 0, sizeof(*batch));
    batch->build_time = time(NULL);
    ap_triage_summary_init(&batch->summary);
}
