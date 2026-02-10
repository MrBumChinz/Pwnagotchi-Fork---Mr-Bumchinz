/*
 * wifi_recovery.c - Automatic WiFi Recovery Module
 * 
 * Implements automatic detection and recovery from brcmfmac driver failures.
 * Based on jayofelony's fix_services.py plugin patterns.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>

#include "wifi_recovery.h"

/* Logging helper */
#define LOG_PREFIX "[wifi_recovery] "
#define LOG_INFO(fmt, ...)  fprintf(stderr, LOG_PREFIX fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, LOG_PREFIX "WARN: " fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...)   fprintf(stderr, LOG_PREFIX "ERROR: " fmt "\n", ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) fprintf(stderr, LOG_PREFIX "DEBUG: " fmt "\n", ##__VA_ARGS__)

/* Default configuration */
#define DEFAULT_BLIND_THRESHOLD_SECS    120   /* 2 minutes with no APs - reduced false triggers */
#define DEFAULT_RECOVERY_COOLDOWN_SECS  120   /* 2 minutes between attempts */
#define DEFAULT_MAX_RECOVERY_ATTEMPTS   3     /* Then reboot (lowered from 5: fail fast, reboot clean) */
#define DEFAULT_ZERO_AP_THRESHOLD       5     /* Consecutive 0-AP polls */
#define STARTUP_GRACE_SECS              180   /* 3 min grace period at boot for bettercap init */

/* dmesg error patterns to detect */
static const char *DMESG_ERROR_PATTERNS[] = {
    "brcmf_cfg80211_nexmon_set_channel: Set Channel failed",
    "Firmware has halted or crashed",
    "brcmf_run_escan: error (-110)",
    "_brcmf_set_multicast_list: Setting allmulti failed, -110",
    "brcmf_cfg80211_add_iface: iface validation failed: err=-95",
    "BRCMF_C_SET_MONITOR error",
    "Failed to initialize a non-removable card",
    "error -22 whilst initialising SDIO card",
    NULL
};

/*
 * Execute a shell command and return exit status
 */
static int exec_cmd(const char *cmd) {
    LOG_DEBUG("exec: %s", cmd);
    int ret = system(cmd);
    if (ret == -1) {
        LOG_ERR("system() failed: %s", strerror(errno));
        return -1;
    }
    return WEXITSTATUS(ret);
}

/*
 * Execute a command and capture output
 */
static int exec_cmd_output(const char *cmd, char *output, size_t output_len) {
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        LOG_ERR("popen() failed: %s", strerror(errno));
        return -1;
    }
    
    if (output && output_len > 0) {
        output[0] = '\0';
        char *p = output;
        size_t remaining = output_len - 1;
        
        while (remaining > 0 && fgets(p, remaining, fp)) {
            size_t len = strlen(p);
            p += len;
            remaining -= len;
        }
    }
    
    int status = pclose(fp);
    return WEXITSTATUS(status);
}

/*
 * Check if interface exists and get its operstate
 */
static iface_state_t get_interface_state(const char *iface) {
    char path[256];
    char state[32];
    FILE *fp;
    
    /* Check if interface exists */
    snprintf(path, sizeof(path), "/sys/class/net/%s", iface);
    if (access(path, F_OK) != 0) {
        return IFACE_STATE_MISSING;
    }
    
    /* Read operstate */
    snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", iface);
    fp = fopen(path, "r");
    if (!fp) {
        return IFACE_STATE_UNKNOWN;
    }
    
    if (fgets(state, sizeof(state), fp)) {
        /* Strip newline */
        char *nl = strchr(state, '\n');
        if (nl) *nl = '\0';
        
        fclose(fp);
        
        if (strcmp(state, "up") == 0 || strcmp(state, "unknown") == 0) {
            return IFACE_STATE_UP;
        } else if (strcmp(state, "down") == 0) {
            return IFACE_STATE_DOWN;
        }
    }
    
    fclose(fp);
    return IFACE_STATE_UNKNOWN;
}

/*
 * Check dmesg for brcmfmac errors (recent entries only)
 * Uses /dev/kmsg read with timeout to avoid zombie popen processes
 * on the resource-constrained RPi Zero W.
 */
static bool check_dmesg_for_errors(void) {
    char output[8192];
    
    /* Read dmesg directly from /dev/kmsg is not seekable for tail,
     * so use a simple popen with a very short timeout to prevent zombies.
     * We read the FULL dmesg into a ring buffer, checking only recent lines. */
    FILE *fp = popen("timeout 3 dmesg 2>/dev/null | tail -100", "r");
    if (!fp) {
        LOG_ERR("popen(dmesg) failed: %s", strerror(errno));
        return false;
    }
    
    output[0] = '\0';
    char *p = output;
    size_t remaining = sizeof(output) - 1;
    
    while (remaining > 0 && fgets(p, remaining, fp)) {
        size_t len = strlen(p);
        p += len;
        remaining -= len;
    }
    
    int status = pclose(fp);
    (void)status;  /* Don't care about exit code - timeout returns 124 */
    
    /* Check for error patterns */
    for (int i = 0; DMESG_ERROR_PATTERNS[i]; i++) {
        if (strstr(output, DMESG_ERROR_PATTERNS[i])) {
            LOG_WARN("dmesg error detected: %s", DMESG_ERROR_PATTERNS[i]);
            return true;
        }
    }
    
    return false;
}

/*
 * Stop monitor mode and bettercap wifi
 */
static bool stop_wifi(bool (*bcap_run)(const char *cmd)) {
    bool ok = true;
    
    /* Stop bettercap wifi.recon */
    if (bcap_run) {
        LOG_INFO("Stopping wifi.recon...");
        if (!bcap_run("wifi.recon off")) {
            LOG_WARN("wifi.recon off may have failed");
        }
        usleep(500000); /* 500ms */
    }
    
    /* Run monstop if available */
    LOG_INFO("Running monstop...");
    if (exec_cmd("which monstop >/dev/null 2>&1") == 0) {
        if (exec_cmd("monstop") != 0) {
            LOG_WARN("monstop failed");
            ok = false;
        }
    } else {
        /* Manual interface deletion */
        LOG_INFO("monstop not found, manual interface removal...");
        exec_cmd("ip link set wlan0mon down 2>/dev/null");
        exec_cmd("iw dev wlan0mon del 2>/dev/null");
    }
    
    usleep(500000); /* 500ms */
    return ok;
}

/*
 * Reset the SDIO bus controller (Pi Zero W: mmc1 via mmc-bcm2835 driver).
 * When the SDIO bus itself crashes (mmc1: error -22, "Failed to initialize
 * a non-removable card"), simply reloading brcmfmac won't help because the
 * underlying bus is dead.  We must unbind/rebind the platform driver to
 * power-cycle the SDIO controller, which re-enumerates the WiFi chip.
 */
static bool reset_sdio_bus(void) {
    /* The Pi Zero W SDIO WiFi controller is at 20300000.mmcnr,
     * managed by the mmc-bcm2835 platform driver. */
    const char *driver_path = "/sys/bus/platform/drivers/mmc-bcm2835";
    const char *device_id   = "20300000.mmcnr";

    /* Check the driver path exists */
    char check_path[256];
    snprintf(check_path, sizeof(check_path), "%s/%s", driver_path, device_id);
    if (access(check_path, F_OK) != 0) {
        LOG_WARN("SDIO device path %s not found, skipping bus reset", check_path);
        return false;
    }

    LOG_INFO("Resetting SDIO bus (unbind %s)...", device_id);

    /* Unbind: removes the mmc host entirely, killing the SDIO bus */
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "echo '%s' > %s/unbind 2>/dev/null", device_id, driver_path);
    exec_cmd(cmd);

    /* Give hardware time to fully power down */
    sleep(2);

    /* Rebind: re-probes the SDIO controller, re-enumerates the bus */
    LOG_INFO("Rebinding SDIO bus (bind %s)...", device_id);
    snprintf(cmd, sizeof(cmd),
             "echo '%s' > %s/bind 2>/dev/null", device_id, driver_path);
    exec_cmd(cmd);

    /* Wait for SDIO enumeration + firmware load */
    LOG_INFO("Waiting for SDIO re-enumeration...");
    sleep(5);

    return true;
}

/*
 * Unload and reload brcmfmac driver, with SDIO bus reset fallback
 */
static bool reload_driver(void) {
    LOG_INFO("Unloading brcmfmac...");
    
    /* Unload driver */
    if (exec_cmd("modprobe -r brcmfmac") != 0) {
        LOG_WARN("modprobe -r brcmfmac failed (may already be unloaded)");
    }
    
    /* Wait for unload */
    sleep(3);
    
    /* Reload driver */
    LOG_INFO("Loading brcmfmac...");
    if (exec_cmd("modprobe brcmfmac") != 0) {
        LOG_ERR("modprobe brcmfmac failed!");
        return false;
    }
    
    /* Wait for driver to initialize */
    LOG_INFO("Waiting for driver initialization...");
    sleep(5);
    
    /* Check if wlan0 came up */
    iface_state_t state = get_interface_state("wlan0");
    if (state == IFACE_STATE_MISSING) {
        LOG_WARN("wlan0 missing after modprobe — SDIO bus likely crashed");

        /* ---- SDIO bus reset fallback ---- */
        LOG_INFO("Attempting SDIO bus reset...");

        /* Make sure driver is unloaded before bus reset */
        exec_cmd("modprobe -r brcmfmac 2>/dev/null");
        sleep(1);

        if (!reset_sdio_bus()) {
            LOG_ERR("SDIO bus reset failed!");
            return false;
        }

        /* Reload driver after bus reset */
        LOG_INFO("Reloading brcmfmac after SDIO reset...");
        if (exec_cmd("modprobe brcmfmac") != 0) {
            LOG_ERR("modprobe brcmfmac failed after SDIO reset!");
            return false;
        }

        /* Longer wait — the bus needs time to re-enumerate + firmware load */
        LOG_INFO("Waiting for driver init after SDIO reset...");
        for (int i = 0; i < 10; i++) {
            sleep(2);
            state = get_interface_state("wlan0");
            if (state != IFACE_STATE_MISSING) {
                LOG_INFO("wlan0 appeared after SDIO reset (%d sec)", (i + 1) * 2);
                break;
            }
        }

        state = get_interface_state("wlan0");
        if (state == IFACE_STATE_MISSING) {
            LOG_ERR("wlan0 STILL missing after SDIO bus reset — hardware may be dead");
            return false;
        }
    }
    
    LOG_INFO("wlan0 is present (state: %s)", 
             state == IFACE_STATE_UP ? "up" : 
             state == IFACE_STATE_DOWN ? "down" : "unknown");
    
    return true;
}

/*
 * Start monitor mode
 */
static bool start_wifi(bool (*bcap_run)(const char *cmd)) {
    /* Run monstart if available */
    LOG_INFO("Running monstart...");
    if (exec_cmd("which monstart >/dev/null 2>&1") == 0) {
        if (exec_cmd("monstart") != 0) {
            LOG_ERR("monstart failed!");
            return false;
        }
    } else {
        /* Manual monitor mode setup */
        LOG_INFO("monstart not found, manual setup...");
        exec_cmd("ip link set wlan0 down");
        exec_cmd("iw dev wlan0 interface add wlan0mon type monitor");
        exec_cmd("ip link set wlan0mon up");
    }
    
    /* Wait for interface */
    sleep(2);
    
    /* Check wlan0mon state */
    iface_state_t state = get_interface_state("wlan0mon");
    if (state != IFACE_STATE_UP && state != IFACE_STATE_UNKNOWN) {
        LOG_ERR("wlan0mon is not up after monstart (state: %d)", state);
        return false;
    }
    
    LOG_INFO("wlan0mon is up");
    
    /* CRITICAL FIX: After driver reload, bettercap's old process still holds
     * a dead file descriptor to the previous (destroyed) wlan0mon interface.
     * Simply telling it "wifi.recon on" won't help — it reads from the dead
     * fd and sees 0 APs forever, creating a permanent blind loop.
     * We MUST restart the bettercap service so it opens a fresh pcap handle
     * to the newly created wlan0mon. */
    LOG_INFO("Restarting bettercap service to bind to new wlan0mon...");
    if (exec_cmd("systemctl restart bettercap") != 0) {
        LOG_WARN("bettercap restart returned error, trying stop+start...");
        exec_cmd("systemctl stop bettercap");
        sleep(3);
        if (exec_cmd("systemctl start bettercap") != 0) {
            LOG_ERR("bettercap service failed to start!");
            return false;
        }
    }
    
    /* Wait for bettercap to initialize — Pi Zero W is slow, bettercap needs
     * time to load caplets and open the pcap handle on wlan0mon. */
    LOG_INFO("Waiting for bettercap API to come up...");
    int api_ready = 0;
    for (int wait = 0; wait < 30; wait += 3) {
        sleep(3);
        if (exec_cmd("curl -sf -o /dev/null --max-time 2 "
                      "http://pwnagotchi:pwnagotchi@127.0.0.1:8081/api/session") == 0) {
            api_ready = 1;
            LOG_INFO("Bettercap API responsive after ~%d seconds", wait + 3);
            break;
        }
    }
    if (!api_ready) {
        LOG_WARN("Bettercap API did not respond within 30s, continuing anyway");
    }
    
    /* Now configure bettercap via its API */
    if (bcap_run) {
        LOG_INFO("Setting bettercap wifi.interface...");
        if (!bcap_run("set wifi.interface wlan0mon")) {
            LOG_WARN("Failed to set wifi.interface");
        }
        usleep(500000);
        
        LOG_INFO("Starting wifi.recon...");
        if (!bcap_run("wifi.clear; wifi.recon on")) {
            LOG_WARN("wifi.recon on may have failed");
        }
    }
    
    return true;
}

/*
 * Create wifi recovery context
 */
wifi_recovery_ctx_t *wifi_recovery_create(const wifi_recovery_config_t *config,
                                          const char *mon_iface,
                                          const char *phy_iface) {
    wifi_recovery_ctx_t *ctx = calloc(1, sizeof(wifi_recovery_ctx_t));
    if (!ctx) return NULL;
    
    /* Set configuration */
    if (config) {
        ctx->config = *config;
    } else {
        /* Defaults */
        ctx->config.blind_threshold_secs = DEFAULT_BLIND_THRESHOLD_SECS;
        ctx->config.recovery_cooldown_secs = DEFAULT_RECOVERY_COOLDOWN_SECS;
        ctx->config.max_recovery_attempts = DEFAULT_MAX_RECOVERY_ATTEMPTS;
        ctx->config.enabled = true;
        ctx->config.check_interface_state = true;
        ctx->config.check_dmesg_errors = true;
    }
    
    /* Set interface names */
    strncpy(ctx->mon_interface, mon_iface ? mon_iface : "wlan0mon", 
            sizeof(ctx->mon_interface) - 1);
    strncpy(ctx->phy_interface, phy_iface ? phy_iface : "wlan0",
            sizeof(ctx->phy_interface) - 1);
    
    /* Initialize timing - offset last_ap_seen by grace period so blind timer
     * doesn't fire before bettercap has finished initializing wifi.recon */
    ctx->started_at = time(NULL);
    ctx->last_ap_seen_time = ctx->started_at + STARTUP_GRACE_SECS;
    ctx->last_recovery_time = 0;
    
    LOG_INFO("WiFi recovery initialized (mon=%s, phy=%s, blind_threshold=%ds, startup_grace=%ds)",
             ctx->mon_interface, ctx->phy_interface, 
             ctx->config.blind_threshold_secs, STARTUP_GRACE_SECS);
    
    return ctx;
}

/*
 * Destroy context
 */
void wifi_recovery_destroy(wifi_recovery_ctx_t *ctx) {
    if (ctx) {
        LOG_INFO("WiFi recovery shutdown (total_recoveries=%d, total_failures=%d)",
                 ctx->total_recoveries, ctx->total_failures);
        free(ctx);
    }
}

/*
 * Check if recovery is needed
 */
bool wifi_recovery_check(wifi_recovery_ctx_t *ctx, int ap_count) {
    if (!ctx || !ctx->config.enabled) return false;
    if (ctx->is_recovering) return false;
    
    time_t now = time(NULL);
    
    /* Startup grace period: don't trigger recovery while bettercap is still
     * initializing. The grace offset on last_ap_seen_time handles most of it,
     * but also explicitly skip if we're within the grace window. */
    int uptime = (int)(now - ctx->started_at);
    if (uptime < STARTUP_GRACE_SECS) {
        /* Still in startup grace period - just track APs without triggering */
        if (ap_count > 0) {
            ctx->last_ap_seen_time = now;
            ctx->consecutive_zero_ap_polls = 0;
        }
        return false;
    }
    
    /* Update AP tracking */
    if (ap_count > 0) {
        ctx->last_ap_seen_time = now;
        ctx->consecutive_zero_ap_polls = 0;
        return false;
    }
    
    /* Count zero-AP polls */
    ctx->consecutive_zero_ap_polls++;
    
    /* Check if we've been blind too long */
    int blind_duration = (int)(now - ctx->last_ap_seen_time);
    
    if (blind_duration >= ctx->config.blind_threshold_secs) {
        LOG_WARN("Blind for %d seconds (threshold: %d), checking interface...",
                 blind_duration, ctx->config.blind_threshold_secs);
        
        /* Check interface state */
        if (ctx->config.check_interface_state) {
            iface_state_t state = get_interface_state(ctx->mon_interface);
            if (state == IFACE_STATE_DOWN || state == IFACE_STATE_MISSING) {
                LOG_WARN("%s is %s - recovery needed!", 
                         ctx->mon_interface,
                         state == IFACE_STATE_DOWN ? "DOWN" : "MISSING");
                ctx->needs_recovery = true;
                ctx->interface_was_down = true;
                return true;
            }
        }
        
        /* Check dmesg for driver errors */
        if (ctx->config.check_dmesg_errors && check_dmesg_for_errors()) {
            LOG_WARN("brcmfmac errors in dmesg - recovery needed!");
            ctx->needs_recovery = true;
            return true;
        }
        
        /* Even if interface looks OK, if we're blind for 2x threshold, try recovery */
        if (blind_duration >= ctx->config.blind_threshold_secs * 2) {
            LOG_WARN("Extended blindness (%d sec) - forcing recovery", blind_duration);
            ctx->needs_recovery = true;
            return true;
        }
    }
    
    return false;
}

/*
 * Get interface state
 */
iface_state_t wifi_recovery_get_iface_state(wifi_recovery_ctx_t *ctx) {
    if (!ctx) return IFACE_STATE_UNKNOWN;
    return get_interface_state(ctx->mon_interface);
}

/*
 * Check dmesg
 */
bool wifi_recovery_check_dmesg(wifi_recovery_ctx_t *ctx) {
    (void)ctx;
    return check_dmesg_for_errors();
}

/*
 * Perform recovery
 */
wifi_recovery_result_t wifi_recovery_perform(wifi_recovery_ctx_t *ctx,
                                             bool (*bcap_run)(const char *cmd)) {
    if (!ctx) return WIFI_RECOVERY_FAILED;
    if (!ctx->config.enabled) return WIFI_RECOVERY_DISABLED;
    if (ctx->is_recovering) return WIFI_RECOVERY_IN_PROGRESS;
    
    time_t now = time(NULL);
    
    /* Check cooldown */
    if (ctx->last_recovery_time > 0) {
        int elapsed = (int)(now - ctx->last_recovery_time);
        if (elapsed < ctx->config.recovery_cooldown_secs) {
            LOG_INFO("In cooldown period (%d/%d sec elapsed)",
                     elapsed, ctx->config.recovery_cooldown_secs);
            return WIFI_RECOVERY_COOLDOWN;
        }
    }
    
    /* Check max attempts */
    if (ctx->recovery_attempts >= ctx->config.max_recovery_attempts) {
        LOG_ERR("Max recovery attempts (%d) reached - reboot required!",
                ctx->config.max_recovery_attempts);
        return WIFI_RECOVERY_MAX_ATTEMPTS;
    }
    
    /* Start recovery */
    ctx->is_recovering = true;
    ctx->recovery_attempts++;
    ctx->last_recovery_time = now;
    
    LOG_INFO("=== Starting WiFi recovery (attempt %d/%d) ===",
             ctx->recovery_attempts, ctx->config.max_recovery_attempts);
    
    bool success = true;
    
    /* Step 1: Stop WiFi */
    LOG_INFO("Step 1/3: Stopping WiFi...");
    if (!stop_wifi(bcap_run)) {
        LOG_WARN("Stop WiFi had issues, continuing...");
    }
    
    /* Step 2: Reload driver */
    LOG_INFO("Step 2/3: Reloading brcmfmac driver...");
    if (!reload_driver()) {
        LOG_ERR("Driver reload failed!");
        success = false;
    }
    
    /* Step 3: Start WiFi (if driver reload succeeded) */
    if (success) {
        LOG_INFO("Step 3/3: Starting WiFi...");
        if (!start_wifi(bcap_run)) {
            LOG_ERR("Start WiFi failed!");
            success = false;
        }
    }
    
    ctx->is_recovering = false;
    ctx->needs_recovery = false;
    
    if (success) {
        LOG_INFO("=== WiFi recovery SUCCESSFUL ===");
        ctx->total_recoveries++;
        ctx->recovery_attempts = 0;  /* Reset on success */
        ctx->last_ap_seen_time = now; /* Give it time to find APs */
        ctx->interface_was_down = false;
        return WIFI_RECOVERY_SUCCESS;
    } else {
        LOG_ERR("=== WiFi recovery FAILED ===");
        ctx->total_failures++;
        return WIFI_RECOVERY_FAILED;
    }
}

/*
 * Force recovery (ignore cooldown)
 */
wifi_recovery_result_t wifi_recovery_force(wifi_recovery_ctx_t *ctx,
                                           bool (*bcap_run)(const char *cmd)) {
    if (!ctx) return WIFI_RECOVERY_FAILED;
    
    /* Clear cooldown */
    ctx->last_recovery_time = 0;
    ctx->needs_recovery = true;
    
    return wifi_recovery_perform(ctx, bcap_run);
}

/*
 * Reset state
 */
void wifi_recovery_reset(wifi_recovery_ctx_t *ctx) {
    if (!ctx) return;
    
    ctx->consecutive_zero_ap_polls = 0;
    ctx->recovery_attempts = 0;
    ctx->needs_recovery = false;
    ctx->interface_was_down = false;
    ctx->last_ap_seen_time = time(NULL);
    
    LOG_INFO("Recovery state reset");
}

/*
 * Get stats string
 */
void wifi_recovery_stats(wifi_recovery_ctx_t *ctx, char *buf, size_t len) {
    if (!ctx || !buf || len == 0) return;
    
    time_t now = time(NULL);
    int blind_duration = (int)(now - ctx->last_ap_seen_time);
    
    iface_state_t state = get_interface_state(ctx->mon_interface);
    const char *state_str = state == IFACE_STATE_UP ? "UP" :
                            state == IFACE_STATE_DOWN ? "DOWN" :
                            state == IFACE_STATE_MISSING ? "MISSING" : "?";
    
    snprintf(buf, len,
             "WiFi Recovery: enabled=%d, %s=%s, blind=%ds, "
             "attempts=%d/%d, total_ok=%d, total_fail=%d",
             ctx->config.enabled,
             ctx->mon_interface, state_str,
             blind_duration,
             ctx->recovery_attempts, ctx->config.max_recovery_attempts,
             ctx->total_recoveries, ctx->total_failures);
}

/*
 * Check if reboot needed
 */
bool wifi_recovery_should_reboot(wifi_recovery_ctx_t *ctx) {
    if (!ctx) return false;
    return ctx->recovery_attempts >= ctx->config.max_recovery_attempts;
}

/*
 * Trigger reboot
 */
void wifi_recovery_reboot(wifi_recovery_ctx_t *ctx) {
    if (!ctx) return;
    
    LOG_ERR("!!! TRIGGERING SYSTEM REBOOT !!!");
    LOG_ERR("Recovery failed %d times, no choice but to reboot",
            ctx->recovery_attempts);
    
    /* Sync filesystems */
    exec_cmd("sync");
    sleep(1);
    
    /* Reboot */
    exec_cmd("shutdown -r now");
}
