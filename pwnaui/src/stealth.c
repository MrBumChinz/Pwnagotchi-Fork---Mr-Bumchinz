/**
 * stealth.c - Neurolyzer-inspired Stealth & Evasion System
 *
 * Implementation of adaptive stealth, WIDS detection, MAC rotation,
 * and deauth throttling for the C brain.
 *
 * Inspired by: AlienMajik/pwnagotchi_plugins/neurolyzer.py
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <ctype.h>

#include "stealth.h"

/* ============================================================================
 * Realistic OUI List (from Neurolyzer)
 * ========================================================================== */

/* Common vendor OUIs for realistic MAC generation */
static const char *REALISTIC_OUIS[STEALTH_OUI_COUNT] = {
    "B8:27:EB",     /* Raspberry Pi */
    "DC:A6:32",     /* Raspberry Pi */
    "E4:5F:01",     /* Raspberry Pi */
    "00:14:22",     /* Dell */
    "34:AB:95",     /* Generic */
    "00:1A:11",     /* Google */
    "08:74:02",     /* Apple */
    "50:32:37",     /* Apple */
    "FC:45:96",     /* Apple */
    "00:E0:4C",     /* Realtek */
    "00:1E:06",     /* Wibrain */
    "00:26:BB",     /* Apple */
    "00:50:F2",     /* Microsoft */
    "00:0C:29",     /* VMware */
    "00:15:5D",     /* Hyper-V */
    "00:1C:42"      /* Parallels */
};

/* Default WIDS/WIPS SSID patterns */
static const char *DEFAULT_WIDS_PATTERNS[] = {
    "wids-guardian",
    "airdefense",
    "cisco-ips",
    "cisco-awips",
    "fortinet-wids",
    "aruba-widp",
    "aruba-ips",
    "kismet",
    "airmagnet",
    "airtight",
    "fluke-aircheck",
    "wireless-ids",
    NULL
};

/* ============================================================================
 * Utility Functions
 * ========================================================================== */

/* Random number in range [min, max] */
static int rand_range(int min, int max) {
    return min + (rand() % (max - min + 1));
}

/* Random float in range [0.0, 1.0] */
static float rand_float(void) {
    return (float)rand() / (float)RAND_MAX;
}

/* Case-insensitive substring search */
static bool str_contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;

    size_t needle_len = strlen(needle);
    size_t haystack_len = strlen(haystack);

    if (needle_len > haystack_len) return false;

    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (strncasecmp(haystack + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

/* Execute command and return success */
static int exec_cmd(const char *cmd) {
    int status = system(cmd);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* Get current MAC address from interface */
static int get_current_mac(const char *interface, char *mac_out) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", interface);

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    if (fgets(mac_out, STEALTH_MAC_STR_LEN, f) == NULL) {
        fclose(f);
        return -1;
    }
    fclose(f);

    /* Remove trailing newline */
    size_t len = strlen(mac_out);
    if (len > 0 && mac_out[len - 1] == '\n') {
        mac_out[len - 1] = '\0';
    }

    /* Convert to uppercase */
    for (int i = 0; mac_out[i]; i++) {
        mac_out[i] = toupper((unsigned char)mac_out[i]);
    }

    return 0;
}

/* ============================================================================
 * Configuration
 * ========================================================================== */

stealth_config_t stealth_config_default(void) {
    stealth_config_t config = {
        .mode = STEALTH_MODE_STEALTH,

        /* Whitelist - empty by default */
        .whitelist_count = 0,

        /* MAC rotation */
        .mac_rotation_enabled = true,
        .mac_rotation_interval = 1800,  /* 30 minutes default */
        .use_realistic_oui = true,

        /* Deauth throttling */
        .deauth_throttle = 0.5f,        /* 50% by default */
        .max_deauths_per_epoch = 20,

        /* WIDS detection */
        .wids_detection_enabled = true,
        .wids_pattern_count = 0,

        /* Adaptive behavior */
        .adaptive_stealth = true,
        .crowded_threshold = 20,
        .quiet_threshold = 5
    };

    /* Initialize WIDS patterns with defaults */
    for (int i = 0; DEFAULT_WIDS_PATTERNS[i] && i < STEALTH_MAX_WIDS_PATTERNS; i++) {
        strncpy(config.wids_patterns[i], DEFAULT_WIDS_PATTERNS[i],
                STEALTH_MAX_SSID_LEN - 1);
        config.wids_pattern_count++;
    }

    return config;
}

/* ============================================================================
 * Context Management
 * ========================================================================== */

stealth_ctx_t *stealth_create(const stealth_config_t *config, const char *interface) {
    stealth_ctx_t *ctx = calloc(1, sizeof(stealth_ctx_t));
    if (!ctx) return NULL;

    /* Copy configuration or use defaults */
    if (config) {
        ctx->config = *config;
    } else {
        /* Default configuration */
        ctx->config.mode = STEALTH_MODE_STEALTH;
        ctx->config.mac_rotation_enabled = false;  /* Disabled by default */
        ctx->config.mac_rotation_interval = 1800;  /* 30 minutes */
        ctx->config.deauth_throttle = 0.5f;
        ctx->config.wids_detection_enabled = true;
        ctx->config.whitelist_count = 0;
        ctx->config.adaptive_stealth = true;
        ctx->config.crowded_threshold = 20;
        ctx->config.quiet_threshold = 5;
        
        /* Add default WIDS patterns */
        const char *default_wids[] = {
            "wids", "airdefense", "kismet", "honeypot", "fortinet"
        };
        for (int i = 0; i < 5 && i < STEALTH_MAX_WIDS_PATTERNS; i++) {
            strncpy(ctx->config.wids_patterns[i], default_wids[i], 
                    STEALTH_MAX_SSID_LEN - 1);
            ctx->config.wids_pattern_count++;
        }
    }

    /* Initialize state */
    ctx->current_level = STEALTH_LEVEL_MEDIUM;
    ctx->last_mac_change = 0;
    ctx->last_wids_check = 0;
    ctx->deauths_this_epoch = 0;
    ctx->mac_changed = false;

    /* Store interface name */
    strncpy(ctx->interface, interface, sizeof(ctx->interface) - 1);

    /* Get original MAC */
    if (get_current_mac(interface, ctx->original_mac) == 0) {
        strncpy(ctx->current_mac, ctx->original_mac, STEALTH_MAC_STR_LEN);
    }

    /* Seed RNG */
    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

    fprintf(stderr, "[stealth] initialized: mode=%s, interface=%s, mac=%s\n",
            stealth_mode_name(ctx->config.mode), interface, ctx->original_mac);

    return ctx;
}

void stealth_destroy(stealth_ctx_t *ctx) {
    if (!ctx) return;

    /* Restore original MAC if changed */
    if (ctx->mac_changed) {
        stealth_restore_mac(ctx);
    }

    free(ctx);
}

/* ============================================================================
 * Whitelist Management
 * ========================================================================== */

bool stealth_is_whitelisted(stealth_ctx_t *ctx, const char *ssid) {
    if (!ctx || !ssid) return false;

    for (int i = 0; i < ctx->config.whitelist_count; i++) {
        if (strcasecmp(ctx->config.whitelist[i], ssid) == 0) {
            ctx->whitelisted_skips++;
            return true;
        }
    }
    return false;
}

int stealth_add_whitelist(stealth_ctx_t *ctx, const char *ssid) {
    if (!ctx || !ssid) return -1;

    if (ctx->config.whitelist_count >= STEALTH_MAX_WHITELIST) {
        return -1;  /* Whitelist full */
    }

    /* Check if already in list */
    for (int i = 0; i < ctx->config.whitelist_count; i++) {
        if (strcasecmp(ctx->config.whitelist[i], ssid) == 0) {
            return 0;  /* Already exists */
        }
    }

    strncpy(ctx->config.whitelist[ctx->config.whitelist_count],
            ssid, STEALTH_MAX_SSID_LEN - 1);
    ctx->config.whitelist_count++;

    fprintf(stderr, "[stealth] added to whitelist: %s\n", ssid);
    return 0;
}

/* ============================================================================
 * WIDS Detection
 * ========================================================================== */

wids_result_t stealth_check_wids(stealth_ctx_t *ctx, const char **ssids, int count) {
    wids_result_t result = {0};

    if (!ctx || !ctx->config.wids_detection_enabled) {
        return result;
    }

    time_t now = time(NULL);

    /* Only check every 5 minutes */
    if (now - ctx->last_wids_check < 300) {
        return result;
    }
    ctx->last_wids_check = now;

    for (int i = 0; i < count && ssids[i]; i++) {
        for (int p = 0; p < ctx->config.wids_pattern_count; p++) {
            if (str_contains_ci(ssids[i], ctx->config.wids_patterns[p])) {
                result.detected = true;
                strncpy(result.ssid, ssids[i], STEALTH_MAX_SSID_LEN - 1);
                result.risk_level = 8;  /* High risk */

                ctx->wids_detections++;

                fprintf(stderr, "[stealth] WIDS DETECTED: %s (pattern: %s)\n",
                        ssids[i], ctx->config.wids_patterns[p]);

                return result;
            }
        }
    }

    return result;
}

/* ============================================================================
 * Adaptive Stealth
 * ========================================================================== */



/* Check if single SSID matches WIDS patterns */
bool stealth_is_wids_ap(stealth_ctx_t *ctx, const char *ssid)
{
    if (!ctx || !ssid || !ctx->config.wids_detection_enabled) {
        return false;
    }
    
    /* Check against known WIDS patterns */
    for (int i = 0; i < ctx->config.wids_pattern_count; i++) {
        if (strcasestr(ssid, ctx->config.wids_patterns[i]) != NULL) {
            ctx->wids_detections++;
            return true;
        }
    }
    
    /* Check for common honeypot indicators */
    const char *honeypot_indicators[] = {
        "honeypot", "honey_pot", "fake_ap", "rogue_ap",
        "test_ap", "security_test", "pentest", NULL
    };
    
    for (int i = 0; honeypot_indicators[i] != NULL; i++) {
        if (strcasestr(ssid, honeypot_indicators[i]) != NULL) {
            ctx->wids_detections++;
            return true;
        }
    }
    
    return false;
}

void stealth_adapt_level(stealth_ctx_t *ctx, int ap_count) {
    if (!ctx || !ctx->config.adaptive_stealth) return;

    time_t now = time(NULL);

    /* Only adapt every 60 seconds */
    if (now - ctx->last_adaptation < 60) return;
    ctx->last_adaptation = now;

    stealth_level_t old_level = ctx->current_level;

    if (ap_count > ctx->config.crowded_threshold) {
        /* Crowded area: go stealthier */
        ctx->current_level = STEALTH_LEVEL_PASSIVE;
        ctx->config.deauth_throttle = 0.2f;  /* Only 20% of targets */
        ctx->config.mac_rotation_interval = rand_range(300, 600);  /* 5-10 min */
    } else if (ap_count > ctx->config.quiet_threshold) {
        /* Medium density */
        ctx->current_level = STEALTH_LEVEL_MEDIUM;
        ctx->config.deauth_throttle = 0.8f;  /* 80% of targets */
        ctx->config.mac_rotation_interval = rand_range(600, 1800);  /* 10-30 min */
    } else {
        /* Quiet area: can be more aggressive */
        ctx->current_level = STEALTH_LEVEL_AGGRESSIVE;
        ctx->config.deauth_throttle = 1.0f;  /* 100% of targets */
        ctx->config.mac_rotation_interval = rand_range(1800, 3600);  /* 30-60 min */
    }

    if (ctx->current_level != old_level) {
        fprintf(stderr, "[stealth] level adapted: %s -> %s (ap_count=%d)\n",
                stealth_level_name(old_level),
                stealth_level_name(ctx->current_level),
                ap_count);
    }
}

/* ============================================================================
 * Deauth Throttling
 * ========================================================================== */

bool stealth_should_throttle_deauth(stealth_ctx_t *ctx) {
    if (!ctx) return false;

    /* Check if we've hit the per-epoch limit */
    if (ctx->deauths_this_epoch >= ctx->config.max_deauths_per_epoch) {
        ctx->throttled_deauths++;
        return true;
    }

    /* Random throttle based on current throttle value */
    float r = rand_float();
    if (r > ctx->config.deauth_throttle) {
        ctx->throttled_deauths++;
        return true;
    }

    return false;
}

void stealth_record_deauth(stealth_ctx_t *ctx) {
    if (ctx) {
        ctx->deauths_this_epoch++;
    }
}

void stealth_epoch_reset(stealth_ctx_t *ctx) {
    if (ctx) {
        ctx->deauths_this_epoch = 0;
    }
}

/* ============================================================================
 * MAC Address Rotation
 * ========================================================================== */

bool stealth_should_rotate_mac(stealth_ctx_t *ctx) {
    if (!ctx || !ctx->config.mac_rotation_enabled) return false;
    if (ctx->config.mode == STEALTH_MODE_NORMAL) return false;

    time_t now = time(NULL);
    time_t elapsed = now - ctx->last_mac_change;

    /* Check minimum interval */
    if (elapsed < STEALTH_MIN_MAC_INTERVAL) return false;

    /* Check configured interval */
    return elapsed >= ctx->config.mac_rotation_interval;
}

int stealth_generate_mac(stealth_ctx_t *ctx, char *out_mac) {
    if (!ctx || !out_mac) return -1;

    if (ctx->config.use_realistic_oui) {
        /* Use a realistic OUI */
        const char *oui = REALISTIC_OUIS[rand() % STEALTH_OUI_COUNT];
        snprintf(out_mac, STEALTH_MAC_STR_LEN,
                 "%s:%02X:%02X:%02X",
                 oui,
                 rand() % 256,
                 rand() % 256,
                 rand() % 256);
    } else {
        /* Fully random MAC (locally administered) */
        snprintf(out_mac, STEALTH_MAC_STR_LEN,
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 (rand() % 256) | 0x02,  /* Set locally administered bit */
                 rand() % 256,
                 rand() % 256,
                 rand() % 256,
                 rand() % 256,
                 rand() % 256);
    }

    /* Convert to uppercase */
    for (int i = 0; out_mac[i]; i++) {
        out_mac[i] = toupper((unsigned char)out_mac[i]);
    }

    return 0;
}

int stealth_rotate_mac(stealth_ctx_t *ctx) {
    if (!ctx) return -1;

    /* Only in stealth or noided mode */
    if (ctx->config.mode == STEALTH_MODE_NORMAL) {
        return 0;
    }

    char new_mac[STEALTH_MAC_STR_LEN];
    if (stealth_generate_mac(ctx, new_mac) != 0) {
        return -1;
    }

    /* Build commands to change MAC */
    char cmd[256];

    /* Try to detect if we're using a monitor mode interface */
    bool is_mon = strstr(ctx->interface, "mon") != NULL;

    if (is_mon) {
        /* For nexmon/brcmfmac monitor interfaces (Pi Zero W):
         * wlan0mon is a radiotap interface - direct MAC change fails with
         * "Operation not supported". Instead, change MAC on the base interface
         * (e.g., wlan0) which the firmware uses for transmitted frames. */

        /* Extract base interface name by stripping "mon" suffix */
        char base_iface[64];
        strncpy(base_iface, ctx->interface, sizeof(base_iface) - 1);
        base_iface[sizeof(base_iface) - 1] = '\0';
        char *mon_suffix = strstr(base_iface, "mon");
        if (mon_suffix) {
            *mon_suffix = '\0';  /* "wlan0mon" -> "wlan0" */
        } else {
            /* No "mon" suffix found, can't determine base interface */
            fprintf(stderr, "[stealth] cannot determine base interface for %s\n", ctx->interface);
            return -1;
        }

        fprintf(stderr, "[stealth] monitor mode detected, changing MAC on base interface %s\n", base_iface);

        /* Bring base interface down, change MAC, bring back up */
        snprintf(cmd, sizeof(cmd), "ip link set %s down 2>/dev/null", base_iface);
        exec_cmd(cmd);

        snprintf(cmd, sizeof(cmd), "ip link set %s address %s 2>/dev/null", base_iface, new_mac);
        int ret = exec_cmd(cmd);

        if (ret != 0) {
            /* Try macchanger as fallback on base interface */
            snprintf(cmd, sizeof(cmd), "macchanger -m %s %s 2>/dev/null", new_mac, base_iface);
            ret = exec_cmd(cmd);
        }

        /* Bring base interface back (leave down since it was down) */
        /* On nexmon, wlan0 stays down while wlan0mon is active */

        if (ret != 0) {
            fprintf(stderr, "[stealth] MAC rotation failed on base interface %s\n", base_iface);
            return -1;
        }

        /* Verify using base interface MAC (wlan0mon always shows 00:00:00:00:00:00) */
        char verify_mac[STEALTH_MAC_STR_LEN];
        usleep(200000);  /* Wait 200ms */

        if (get_current_mac(base_iface, verify_mac) == 0) {
            if (strcasecmp(verify_mac, new_mac) == 0) {
                strncpy(ctx->current_mac, new_mac, STEALTH_MAC_STR_LEN);
                ctx->mac_changed = true;
                ctx->last_mac_change = time(NULL);
                ctx->total_mac_rotations++;

                fprintf(stderr, "[stealth] MAC rotated on %s: %s -> %s\n",
                        base_iface, ctx->original_mac, new_mac);
                return 0;
            }
        }

        fprintf(stderr, "[stealth] MAC verification failed on %s (expected %s, got %s)\n",
                base_iface, new_mac, verify_mac);
        return -1;
    } else {
        /* Regular interface */
        snprintf(cmd, sizeof(cmd), "ip link set %s down", ctx->interface);
        if (exec_cmd(cmd) != 0) {
            fprintf(stderr, "[stealth] failed to bring down %s\n", ctx->interface);
            return -1;
        }

        snprintf(cmd, sizeof(cmd), "ip link set %s address %s", ctx->interface, new_mac);
        if (exec_cmd(cmd) != 0) {
            /* Try macchanger as fallback */
            snprintf(cmd, sizeof(cmd), "macchanger -m %s %s 2>/dev/null", new_mac, ctx->interface);
            if (exec_cmd(cmd) != 0) {
                /* Bring interface back up even on failure */
                snprintf(cmd, sizeof(cmd), "ip link set %s up", ctx->interface);
                exec_cmd(cmd);
                fprintf(stderr, "[stealth] MAC change failed for %s\n", ctx->interface);
                return -1;
            }
        }

        snprintf(cmd, sizeof(cmd), "ip link set %s up", ctx->interface);
        exec_cmd(cmd);
    }

    /* Verify the change */
    char verify_mac[STEALTH_MAC_STR_LEN];
    usleep(500000);  /* Wait 500ms for interface to come up */

    if (get_current_mac(ctx->interface, verify_mac) == 0) {
        if (strcasecmp(verify_mac, new_mac) == 0) {
            strncpy(ctx->current_mac, new_mac, STEALTH_MAC_STR_LEN);
            ctx->mac_changed = true;
            ctx->last_mac_change = time(NULL);
            ctx->total_mac_rotations++;

            fprintf(stderr, "[stealth] MAC rotated: %s -> %s\n",
                    ctx->original_mac, new_mac);
            return 0;
        }
    }

    fprintf(stderr, "[stealth] MAC verification failed (expected %s, got %s)\n",
            new_mac, verify_mac);
    return -1;
}

int stealth_restore_mac(stealth_ctx_t *ctx) {
    if (!ctx || !ctx->mac_changed) return 0;

    char cmd[256];

    snprintf(cmd, sizeof(cmd), "ip link set %s down 2>/dev/null", ctx->interface);
    exec_cmd(cmd);

    snprintf(cmd, sizeof(cmd), "ip link set %s address %s 2>/dev/null",
             ctx->interface, ctx->original_mac);
    int ret = exec_cmd(cmd);

    if (ret != 0) {
        snprintf(cmd, sizeof(cmd), "macchanger -m %s %s 2>/dev/null",
                 ctx->original_mac, ctx->interface);
        ret = exec_cmd(cmd);
    }

    snprintf(cmd, sizeof(cmd), "ip link set %s up 2>/dev/null", ctx->interface);
    exec_cmd(cmd);

    if (ret == 0) {
        strncpy(ctx->current_mac, ctx->original_mac, STEALTH_MAC_STR_LEN);
        ctx->mac_changed = false;
        fprintf(stderr, "[stealth] MAC restored: %s\n", ctx->original_mac);
    }

    return ret;
}

/* ============================================================================
 * Getters
 * ========================================================================== */

stealth_level_t stealth_get_level(stealth_ctx_t *ctx) {
    return ctx ? ctx->current_level : STEALTH_LEVEL_MEDIUM;
}

float stealth_get_deauth_throttle(stealth_ctx_t *ctx) {
    if (!ctx) return 0.5f;

    switch (ctx->current_level) {
        case STEALTH_LEVEL_AGGRESSIVE:
            return 0.8f;
        case STEALTH_LEVEL_MEDIUM:
            return 0.5f;
        case STEALTH_LEVEL_PASSIVE:
            return 0.2f;
        default:
            return ctx->config.deauth_throttle;
    }
}

int stealth_get_mac_interval(stealth_ctx_t *ctx) {
    if (!ctx) return 1800;

    switch (ctx->current_level) {
        case STEALTH_LEVEL_AGGRESSIVE:
            return rand_range(1800, 3600);  /* 30-60 min */
        case STEALTH_LEVEL_MEDIUM:
            return rand_range(600, 1800);   /* 10-30 min */
        case STEALTH_LEVEL_PASSIVE:
            return rand_range(300, 600);    /* 5-10 min */
        default:
            return ctx->config.mac_rotation_interval;
    }
}

const char *stealth_level_name(stealth_level_t level) {
    switch (level) {
        case STEALTH_LEVEL_AGGRESSIVE: return "aggressive";
        case STEALTH_LEVEL_MEDIUM:     return "medium";
        case STEALTH_LEVEL_PASSIVE:    return "passive";
        default:                       return "unknown";
    }
}

const char *stealth_mode_name(stealth_mode_t mode) {
    switch (mode) {
        case STEALTH_MODE_NORMAL:  return "normal";
        case STEALTH_MODE_STEALTH: return "stealth";
        case STEALTH_MODE_NOIDED:  return "noided";
        default:                   return "unknown";
    }
}
