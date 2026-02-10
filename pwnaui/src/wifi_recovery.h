/*
 * wifi_recovery.h - Automatic WiFi Recovery Module
 * 
 * Inspired by jayofelony's fix_services.py plugin.
 * Detects brcmfmac driver failures and attempts automatic recovery
 * without requiring a full system reboot.
 * 
 * Detection patterns:
 *   - brcmf_cfg80211_nexmon_set_channel: Set Channel failed (-110 ETIMEDOUT)
 *   - Firmware has halted or crashed
 *   - wlan0mon interface DOWN or missing
 *   - Zero APs detected for extended period
 * 
 * Recovery sequence:
 *   1. Stop wifi.recon in bettercap
 *   2. monstop (delete wlan0mon)
 *   3. modprobe -r brcmfmac
 *   4. modprobe brcmfmac
 *   5. monstart (recreate wlan0mon)
 *   6. Restart bettercap wifi.recon
 *   7. If all else fails, trigger reboot
 */

#ifndef WIFI_RECOVERY_H
#define WIFI_RECOVERY_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Recovery configuration */
typedef struct {
    int blind_threshold_secs;       /* Seconds with 0 APs before recovery (default: 120) */
    int recovery_cooldown_secs;     /* Minimum time between recovery attempts (default: 180) */
    int max_recovery_attempts;      /* Max attempts before reboot (default: 3) */
    bool enabled;                   /* Enable/disable auto-recovery */
    bool check_interface_state;     /* Check wlan0mon UP/DOWN state */
    bool check_dmesg_errors;        /* Check dmesg for brcmfmac errors */
} wifi_recovery_config_t;

/* Recovery state */
typedef struct {
    wifi_recovery_config_t config;
    
    /* Timing */
    time_t last_recovery_time;      /* When we last attempted recovery */
    time_t last_ap_seen_time;       /* When we last saw APs > 0 */
    time_t started_at;              /* When module started */
    
    /* Counters */
    int consecutive_zero_ap_polls;  /* Number of consecutive polls with 0 APs */
    int recovery_attempts;          /* Recovery attempts since last success */
    int total_recoveries;           /* Total successful recoveries */
    int total_failures;             /* Total failed recoveries */
    
    /* State flags */
    bool is_recovering;             /* Currently in recovery process */
    bool interface_was_down;        /* Interface was down last check */
    bool needs_recovery;            /* Recovery triggered but not yet performed */
    
    /* Interface name */
    char mon_interface[32];         /* Monitor interface name (e.g., wlan0mon) */
    char phy_interface[32];         /* Physical interface name (e.g., wlan0) */
    
} wifi_recovery_ctx_t;

/* Recovery result */
typedef enum {
    WIFI_RECOVERY_OK = 0,           /* No recovery needed */
    WIFI_RECOVERY_SUCCESS,          /* Recovery succeeded */
    WIFI_RECOVERY_FAILED,           /* Recovery failed */
    WIFI_RECOVERY_COOLDOWN,         /* In cooldown period */
    WIFI_RECOVERY_DISABLED,         /* Recovery disabled */
    WIFI_RECOVERY_MAX_ATTEMPTS,     /* Max attempts reached, reboot needed */
    WIFI_RECOVERY_IN_PROGRESS       /* Already recovering */
} wifi_recovery_result_t;

/* Interface state */
typedef enum {
    IFACE_STATE_UNKNOWN = 0,
    IFACE_STATE_UP,
    IFACE_STATE_DOWN,
    IFACE_STATE_MISSING
} iface_state_t;

/*
 * Create wifi recovery context
 * config: Optional configuration (NULL for defaults)
 * mon_iface: Monitor interface name (default: "wlan0mon")
 * phy_iface: Physical interface name (default: "wlan0")
 */
wifi_recovery_ctx_t *wifi_recovery_create(const wifi_recovery_config_t *config,
                                          const char *mon_iface,
                                          const char *phy_iface);

/*
 * Destroy wifi recovery context
 */
void wifi_recovery_destroy(wifi_recovery_ctx_t *ctx);

/*
 * Check if recovery is needed based on current AP count
 * Call this every poll cycle with the current AP count
 * Returns: true if recovery was triggered
 */
bool wifi_recovery_check(wifi_recovery_ctx_t *ctx, int ap_count);

/*
 * Check monitor interface state directly
 * Returns: IFACE_STATE_UP, IFACE_STATE_DOWN, or IFACE_STATE_MISSING
 */
iface_state_t wifi_recovery_get_iface_state(wifi_recovery_ctx_t *ctx);

/*
 * Check dmesg for brcmfmac errors
 * Returns: true if errors detected
 */
bool wifi_recovery_check_dmesg(wifi_recovery_ctx_t *ctx);

/*
 * Perform recovery
 * bcap_run: Function to execute bettercap commands (can be NULL)
 * Returns: Recovery result code
 */
wifi_recovery_result_t wifi_recovery_perform(wifi_recovery_ctx_t *ctx,
                                             bool (*bcap_run)(const char *cmd));

/*
 * Force immediate recovery attempt (ignores cooldown)
 */
wifi_recovery_result_t wifi_recovery_force(wifi_recovery_ctx_t *ctx,
                                           bool (*bcap_run)(const char *cmd));

/*
 * Reset recovery state (after successful operation)
 */
void wifi_recovery_reset(wifi_recovery_ctx_t *ctx);

/*
 * Get recovery statistics string
 * buf: Output buffer
 * len: Buffer length
 */
void wifi_recovery_stats(wifi_recovery_ctx_t *ctx, char *buf, size_t len);

/*
 * Check if we should reboot (max attempts exceeded)
 */
bool wifi_recovery_should_reboot(wifi_recovery_ctx_t *ctx);

/*
 * Trigger system reboot
 */
void wifi_recovery_reboot(wifi_recovery_ctx_t *ctx);

#endif /* WIFI_RECOVERY_H */
