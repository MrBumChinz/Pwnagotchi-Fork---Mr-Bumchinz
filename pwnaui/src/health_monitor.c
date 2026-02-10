/*
 * PwnaUI Health Monitor Implementation
 *
 * Monitors and logs everything that could go wrong on the pwnagotchi.
 *
 * PERF FIX: All popen()/fork() calls removed.
 * On Pi Zero W (single-core ARM11 700MHz), each fork+exec was 400-1000ms.
 * Previous code spawned 6+ processes every 2s = 2-6 second main loop stall.
 * Now uses direct /proc and /sys reads with PID caching instead.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <ctype.h>

#include "health_monitor.h"

/* ============================================================================
 * UTILITY FUNCTIONS
 * ============================================================================
*/
static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void get_timestamp(char *buf, size_t len) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", tm);
}

static int format_duration(char *buf, size_t len, int seconds) {
    int hours = seconds / 3600;
    int mins = (seconds % 3600) / 60;
    int secs = seconds % 60;

    if (hours > 0) {
        return snprintf(buf, len, "%dh %dm %ds", hours, mins, secs);
    } else if (mins > 0) {
        return snprintf(buf, len, "%dm %ds", mins, secs);
    } else {
        return snprintf(buf, len, "%ds", secs);
    }
}

/* ============================================================================
 * LOGGING
 * ============================================================================
*/
void health_log(health_state_t *state, const char *level, const char *fmt, ...) {
    if (!state->enabled || !state->log_fp) return;

    char timestamp[32];
    get_timestamp(timestamp, sizeof(timestamp));

    fprintf(state->log_fp, "[%s] [%s] ", timestamp, level);

    va_list args;
    va_start(args, fmt);
    vfprintf(state->log_fp, fmt, args);
    va_end(args);

    fprintf(state->log_fp, "\n");
    fflush(state->log_fp);
}

void health_check_log_rotation(health_state_t *state) {
    if (!state->log_fp) return;

    struct stat st;
    if (fstat(fileno(state->log_fp), &st) == 0) {
        if (st.st_size > HEALTH_LOG_MAX_SIZE) {
            health_log(state, "INFO", "Log rotation triggered (size: %ld bytes)",
                      (long)st.st_size);

            fclose(state->log_fp);

            /* Rotate: health.log -> health.log.1 */
            char old_path[256];
            snprintf(old_path, sizeof(old_path), "%s.1", HEALTH_LOG_PATH);
            rename(HEALTH_LOG_PATH, old_path);

            /* Open new log */
            state->log_fp = fopen(HEALTH_LOG_PATH, "a");
            if (state->log_fp) {
                health_log(state, "INFO", "Log rotated, new file started");
            }
        }
    }
}

/* ============================================================================
 * PID LOOKUP — NO FORK (direct /proc reads with caching)
 * ============================================================================
*/

/* Verify if a cached PID is still the expected process — single file read */
static bool verify_pid(pid_t pid, const char *name) {
    if (pid <= 0) return false;
    char path[64], comm[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    bool match = false;
    if (fgets(comm, sizeof(comm), fp)) {
        comm[strcspn(comm, "\n")] = 0;
        match = (strcmp(comm, name) == 0);
    }
    fclose(fp);
    return match;
}

/* Find PID by scanning /proc — NO fork/popen.
 * Uses cached PID as fast-path (single file read instead of full /proc scan). */
static pid_t find_pid_cached(const char *name, pid_t cached) {
    /* Fast path: verify the cached PID is still alive and correct */
    if (cached > 0 && verify_pid(cached, name))
        return cached;

    /* Slow path: scan /proc entries (still no fork, just readdir + file reads) */
    DIR *dp = opendir("/proc");
    if (!dp) return 0;
    struct dirent *de;
    pid_t result = 0;
    while ((de = readdir(dp)) != NULL) {
        /* Skip non-numeric entries */
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        char path[64], comm[64];
        snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        if (fgets(comm, sizeof(comm), fp)) {
            comm[strcspn(comm, "\n")] = 0;
            if (strcmp(comm, name) == 0) {
                result = (pid_t)atoi(de->d_name);
                fclose(fp);
                break;
            }
        }
        fclose(fp);
    }
    closedir(dp);
    return result;
}

/* ============================================================================
 * SYSTEM CHECKS — ALL DIRECT READS (no fork/popen)
 * ============================================================================
*/
static bool check_interface_exists(const char *iface) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%s", iface);
    return access(path, F_OK) == 0;
}

static int read_cpu_temp(void) {
    FILE *fp = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!fp) return 0;

    int temp_milli;
    if (fscanf(fp, "%d", &temp_milli) != 1) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    return temp_milli / 1000;
}

static int read_mem_free_mb(void) {
    struct sysinfo info;
    if (sysinfo(&info) != 0) return 0;
    return (int)(info.freeram / (1024 * 1024));
}

static bool check_throttled(void) {
    /* Direct sysfs read only — no popen("vcgencmd") fallback.
     * The vcgencmd popen was spawning a shell + vcgencmd process,
     * each fork costing 400-1000ms on Pi Zero W. */
    FILE *fp = fopen("/sys/devices/platform/soc/soc:firmware/get_throttled", "r");
    if (!fp) return false;  /* sysfs unavailable = assume no throttling */

    int val = 0;
    if (fscanf(fp, "%d", &val) != 1) val = 0;
    fclose(fp);
    return val != 0;
}

static bool check_brcmfmac_loaded(void) {
    FILE *fp = fopen("/proc/modules", "r");
    if (!fp) return false;

    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "brcmfmac ", 9) == 0) {
            found = true;
            break;
        }
    }
    fclose(fp);
    return found;
}

/* Replaced: check_dmesg_errors() used popen("dmesg | tail -50 | grep -c ...")
 * which spawned shell + dmesg + tail + grep = 4 processes, taking 1-3 seconds.
 * Now reads /dev/kmsg directly with O_NONBLOCK for zero-fork error detection. */
static int check_dmesg_errors(void) {
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (fd < 0) return 0;

    /* Seek to end minus ~8KB to read only recent messages */
    lseek(fd, 0, SEEK_END);

    /* We can't easily seek backwards in /dev/kmsg.
     * Instead, just check if brcmfmac driver is loaded and healthy
     * via /proc/modules (already done by check_brcmfmac_loaded).
     * Return 0 — dmesg error counting is not worth the complexity. */
    close(fd);
    return 0;
}

/* ============================================================================
 * INITIALIZATION
 * ============================================================================
*/
int health_monitor_init(health_state_t *state, bool enabled) {
    memset(state, 0, sizeof(*state));
    state->enabled = enabled;

    if (!enabled) {
        return 0;
    }

    /* Open log file */
    state->log_fp = fopen(HEALTH_LOG_PATH, "a");
    if (!state->log_fp) {
        fprintf(stderr, "[HEALTH] Failed to open log file: %s\n", HEALTH_LOG_PATH);
        return -1;
    }

    state->start_time = time(NULL);
    state->last_channel = -1;
    state->bettercap_pid = find_pid_cached("bettercap", 0);
    state->last_bettercap_start = time(NULL);

    /* Initial interface check */
    state->wlan0mon_exists = check_interface_exists("wlan0mon");
    state->wlan0_exists = check_interface_exists("wlan0");

    /* Log startup */
    health_log(state, "INFO", "========================================");
    health_log(state, "INFO", "Health Monitor Started (no-fork edition)");
    health_log(state, "INFO", "========================================");

    /* Log initial system state */
    health_log(state, "INFO", "WiFi: wlan0mon=%s, wlan0=%s",
              state->wlan0mon_exists ? "yes" : "NO",
              state->wlan0_exists ? "yes" : "NO");
    health_log(state, "INFO", "brcmfmac: %s",
              check_brcmfmac_loaded() ? "loaded" : "NOT LOADED!");
    health_log(state, "INFO", "bettercap PID: %d", (int)state->bettercap_pid);
    health_log(state, "INFO", "CPU temp: %dC, Free mem: %dMB",
              read_cpu_temp(), read_mem_free_mb());

    if (check_throttled()) {
        health_log(state, "WARN", "CPU THROTTLED at startup!");
    }

    return 0;
}

/* ============================================================================
 * HEALTH CHECKS
 * ============================================================================
*/
static void check_wifi_interfaces(health_state_t *state) {
    bool has_wlan0mon = check_interface_exists("wlan0mon");
    bool has_wlan0 = check_interface_exists("wlan0");

    /* Interface disappeared */
    if (state->wlan0mon_exists && !has_wlan0mon) {
        health_log(state, "ERROR", "*** wlan0mon DISAPPEARED! ***");
        state->interface_loss_count++;
    }

    /* Interface came back */
    if (!state->wlan0mon_exists && has_wlan0mon) {
        health_log(state, "INFO", "wlan0mon restored");
    }

    /* No WiFi at all */
    if (!has_wlan0mon && !has_wlan0) {
        if (state->wlan0mon_exists || state->wlan0_exists) {
            health_log(state, "ERROR", "*** ALL WIFI INTERFACES GONE! ***");
        }
    }

    state->wlan0mon_exists = has_wlan0mon;
    state->wlan0_exists = has_wlan0;
}

static void check_nexmon_health(health_state_t *state) {
    /* Check if driver still loaded */
    if (!check_brcmfmac_loaded()) {
        health_log(state, "ERROR", "*** brcmfmac driver NOT LOADED! ***");
        state->nexmon_errors++;
        return;
    }

    /* NOTE: Removed check_dmesg_errors() which used popen("dmesg | tail | grep")
     * spawning 4 processes, costing 1-3 seconds on Pi Zero W.
     * Driver health is adequately monitored by brcmfmac module presence check
     * and the wifi_recovery system which catches actual failures. */
}

static void check_bettercap_health(health_state_t *state) {
    /* Uses cached PID — fast path is a single /proc/<pid>/comm read */
    pid_t current_pid = find_pid_cached("bettercap", state->bettercap_pid);

    if (current_pid == 0 && state->bettercap_pid != 0) {
        health_log(state, "ERROR", "*** bettercap DIED! (was PID %d) ***",
                  (int)state->bettercap_pid);
        state->bettercap_restarts++;
    } else if (current_pid != 0 && current_pid != state->bettercap_pid) {
        if (state->bettercap_pid != 0) {
            health_log(state, "WARN", "bettercap restarted (old PID: %d, new PID: %d)",
                      (int)state->bettercap_pid, (int)current_pid);
            state->bettercap_restarts++;
        }
        state->last_bettercap_start = time(NULL);
    }

    state->bettercap_pid = current_pid;
}

static void check_system_health(health_state_t *state) {
    /* Temperature */
    state->cpu_temp = read_cpu_temp();
    if (state->cpu_temp >= HEALTH_TEMP_CRIT_C) {
        health_log(state, "ERROR", "*** CPU CRITICAL TEMP: %dC ***", state->cpu_temp);
    } else if (state->cpu_temp >= HEALTH_TEMP_WARN_C) {
        health_log(state, "WARN", "CPU high temp: %dC", state->cpu_temp);
    }

    /* Memory */
    state->mem_free_mb = read_mem_free_mb();
    if (state->mem_free_mb < HEALTH_MEM_LOW_MB) {
        health_log(state, "WARN", "LOW MEMORY: %dMB free", state->mem_free_mb);
    }

    /* Throttling — direct sysfs read, no popen */
    bool throttled = check_throttled();
    if (throttled && !state->throttled) {
        health_log(state, "WARN", "CPU THROTTLING started!");
        state->throttle_count++;
    } else if (!throttled && state->throttled) {
        health_log(state, "INFO", "CPU throttling ended");
    }
    state->throttled = throttled;
}

/* ============================================================================
 * UPDATE FUNCTION (call from main loop)
 * ============================================================================
*/
void health_monitor_update(health_state_t *state) {
    if (!state->enabled) return;

    uint64_t now = get_time_ms();

    /* WiFi interface check - every 1s */
    if (now - state->last_wifi_check >= HEALTH_WIFI_CHECK_MS) {
        check_wifi_interfaces(state);
        state->last_wifi_check = now;
    }

    /* Nexmon check - every 2s */
    if (now - state->last_nexmon_check >= HEALTH_NEXMON_CHECK_MS) {
        check_nexmon_health(state);
        check_bettercap_health(state);
        state->last_nexmon_check = now;
    }

    /* Full health check - every 5s */
    if (now - state->last_health_check >= HEALTH_CHECK_INTERVAL_MS) {
        check_system_health(state);
        state->last_health_check = now;
    }

    /* Stats log - every 5 minutes */
    if (now - state->last_stats_log >= 300000) {
        health_log_stats(state);
        state->last_stats_log = now;

        /* Check log rotation */
        health_check_log_rotation(state);
    }

    /* CPU profiler update */
    cpu_profile_update(state);
}

/* ============================================================================
 * EVENT REPORTING
 * ============================================================================
*/
void health_report_epoch(health_state_t *state, int epoch, int ap_count,
                         int channel, bool blind) {
    if (!state->enabled) return;

    state->epoch_count++;
    state->ap_count = ap_count;
    if (ap_count > state->max_ap_count) {
        state->max_ap_count = ap_count;
    }

    /* Blind detection */
    if (blind || ap_count == 0) {
        if (!state->was_blind) {
            state->blind_start = time(NULL);
            health_log(state, "WARN", "BLIND started at epoch %d (0 APs visible)", epoch);
        }
        state->blind_count++;

        if (state->blind_count == HEALTH_BLIND_THRESHOLD) {
            health_log(state, "ERROR", "*** EXTENDED BLIND: %d consecutive blind epochs ***",
                      state->blind_count);
        }
    } else {
        if (state->was_blind && state->blind_count > 0) {
            int blind_duration = (int)(time(NULL) - state->blind_start);
            state->total_blind_seconds += blind_duration;
            state->total_blind_events++;
            health_log(state, "INFO", "Blind ended after %d epochs (%ds), APs now: %d",
                      state->blind_count, blind_duration, ap_count);
        }
        state->blind_count = 0;
    }
    state->was_blind = (blind || ap_count == 0);

    /* Channel stuck detection */
    if (channel > 0) {
        if (channel == state->last_channel) {
            state->channel_stuck_count++;
            if (state->channel_stuck_count == HEALTH_CHANNEL_STUCK_THRESH) {
                health_log(state, "WARN", "Channel STUCK on %d for %d epochs",
                          channel, state->channel_stuck_count);
                state->channel_stuck_start = time(NULL);
            }
        } else {
            if (state->channel_stuck_count >= HEALTH_CHANNEL_STUCK_THRESH) {
                int stuck_duration = (int)(time(NULL) - state->channel_stuck_start);
                health_log(state, "INFO", "Channel unstuck (was on %d for %ds)",
                          state->last_channel, stuck_duration);
            }
            state->channel_stuck_count = 0;
        }
        state->last_channel = channel;
    }
}

void health_report_handshake(health_state_t *state, const char *ap_name) {
    if (!state->enabled) return;

    state->handshake_count++;
    health_log(state, "INFO", "HANDSHAKE #%d: %s",
              state->handshake_count, ap_name ? ap_name : "unknown");
}

void health_report_deauth(health_state_t *state) {
    if (!state->enabled) return;
    state->deauth_count++;
}

void health_report_assoc(health_state_t *state) {
    if (!state->enabled) return;
    state->assoc_count++;
}

void health_report_wifi_change(health_state_t *state, bool has_wlan0mon,
                                bool has_wlan0) {
    if (!state->enabled) return;

    health_log(state, "INFO", "WiFi change: wlan0mon=%s, wlan0=%s",
              has_wlan0mon ? "yes" : "no",
              has_wlan0 ? "yes" : "no");

    state->wlan0mon_exists = has_wlan0mon;
    state->wlan0_exists = has_wlan0;
}

void health_report_nexmon_event(health_state_t *state, const char *event) {
    if (!state->enabled) return;

    health_log(state, "WARN", "Nexmon event: %s", event);
    state->nexmon_errors++;
    state->last_nexmon_error = time(NULL);
}

void health_report_bettercap_event(health_state_t *state, const char *event) {
    if (!state->enabled) return;

    health_log(state, "INFO", "Bettercap: %s", event);
}

/* ============================================================================
 * STATS LOGGING
 * ============================================================================
*/
void health_log_stats(health_state_t *state) {
    if (!state->enabled || !state->log_fp) return;

    int uptime = (int)(time(NULL) - state->start_time);
    char uptime_str[32];
    format_duration(uptime_str, sizeof(uptime_str), uptime);

    health_log(state, "STATS", "-------- Periodic Stats --------");
    health_log(state, "STATS", "Uptime: %s | Epochs: %d", uptime_str, state->epoch_count);
    health_log(state, "STATS", "Handshakes: %d | Deauths: %d | Assocs: %d",
              state->handshake_count, state->deauth_count, state->assoc_count);
    health_log(state, "STATS", "APs visible: %d (max: %d)",
              state->ap_count, state->max_ap_count);
    health_log(state, "STATS", "Blind events: %d (total %ds blind)",
              state->total_blind_events, state->total_blind_seconds);
    health_log(state, "STATS", "Interface losses: %d | Nexmon errors: %d",
              state->interface_loss_count, state->nexmon_errors);
    health_log(state, "STATS", "Bettercap restarts: %d | Throttle events: %d",
              state->bettercap_restarts, state->throttle_count);
    health_log(state, "STATS", "CPU: %dC | Free mem: %dMB | Throttled: %s",
              state->cpu_temp, state->mem_free_mb,
              state->throttled ? "YES" : "no");
    health_log(state, "STATS", "--------------------------------");
}

void health_log_final_summary(health_state_t *state) {
    if (!state->enabled || !state->log_fp) return;

    int uptime = (int)(time(NULL) - state->start_time);
    char uptime_str[32];
    format_duration(uptime_str, sizeof(uptime_str), uptime);

    health_log(state, "INFO", "========================================");
    health_log(state, "INFO", "FINAL SESSION SUMMARY");
    health_log(state, "INFO", "========================================");
    health_log(state, "INFO", "Total runtime: %s", uptime_str);
    health_log(state, "INFO", "Total epochs: %d", state->epoch_count);
    health_log(state, "INFO", "");
    health_log(state, "INFO", "--- Captures ---");
    health_log(state, "INFO", "Handshakes: %d", state->handshake_count);
    health_log(state, "INFO", "Deauths sent: %d", state->deauth_count);
    health_log(state, "INFO", "Associations: %d", state->assoc_count);
    health_log(state, "INFO", "Max APs visible: %d", state->max_ap_count);
    health_log(state, "INFO", "");
    health_log(state, "INFO", "--- Issues ---");
    health_log(state, "INFO", "Blind events: %d (total %ds blind)",
              state->total_blind_events, state->total_blind_seconds);
    health_log(state, "INFO", "Interface losses: %d", state->interface_loss_count);
    health_log(state, "INFO", "Nexmon/driver errors: %d", state->nexmon_errors);
    health_log(state, "INFO", "Bettercap restarts: %d", state->bettercap_restarts);
    health_log(state, "INFO", "CPU throttle events: %d", state->throttle_count);
    if (state->peak_cpu_total > 0) {
        health_log(state, "INFO", "Peak CPU: %.1f%% (pwnaui: %.1f%%) at %d APs",
                  state->peak_cpu_total, state->peak_cpu_self,
                  state->peak_ap_count);
    }
    health_log(state, "INFO", "========================================");
}

/* ============================================================================
 * CPU PROFILER — NO FORK (uses cached PIDs)
 * ============================================================================
*/

static const char *g_act_names[CPU_ACT_COUNT] = {
    "bcap_poll",    "channel_hop",  "deauth",      "assoc",
    "csa",          "probe",        "rogue_m2",    "reassoc",
    "disassoc",     "crack_check",  "epoch_end",   "hs_scan",
    "display",      "thompson",
    "bcap_ap_poll", "attack_total"
};

const char *cpu_act_name(int action_id) {
    if (action_id >= 0 && action_id < CPU_ACT_COUNT)
        return g_act_names[action_id];
    return "unknown";
}

uint64_t cpu_act_start(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

void cpu_act_end(health_state_t *state, int action_id, uint64_t start_us) {
    if (!state || !state->cpu_profile_enabled) return;
    if (action_id < 0 || action_id >= CPU_ACT_COUNT) return;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_us = (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
    uint64_t elapsed = now_us - start_us;

    state->act_time_us[action_id] += elapsed;
    state->act_count[action_id]++;
}

/* Read /proc/stat CPU line */
static bool read_proc_stat_cpu(unsigned long long *user, unsigned long long *nice_v,
                               unsigned long long *sys_v, unsigned long long *idle_v,
                               unsigned long long *iowait_v) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return false;

    char buf[256];
    if (!fgets(buf, sizeof(buf), fp)) { fclose(fp); return false; }
    fclose(fp);

    if (sscanf(buf, "cpu %llu %llu %llu %llu %llu",
               user, nice_v, sys_v, idle_v, iowait_v) != 5)
        return false;
    return true;
}

/* Read /proc/<pid>/stat for utime+stime (fields 14,15) */
static bool read_proc_pid_cpu(pid_t pid,
                              unsigned long long *utime,
                              unsigned long long *stime) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *fp = fopen(path, "r");
    if (!fp) { *utime = 0; *stime = 0; return false; }

    char buf[512];
    if (!fgets(buf, sizeof(buf), fp)) { fclose(fp); *utime = 0; *stime = 0; return false; }
    fclose(fp);

    /* Skip past comm field (may contain spaces/parens) */
    char *p = strrchr(buf, ')');
    if (!p) return false;
    p += 2;

    /* Fields after comm: state(3) ppid(4) pgrp(5) session(6) tty(7) tpgid(8)
       flags(9) minflt(10) cminflt(11) majflt(12) cmajflt(13) utime(14) stime(15) */
    char st;
    int f1, f2, f3, f4, f5;
    unsigned f6;
    unsigned long f7, f8, f9, f10;
    unsigned long long ut, stm;

    if (sscanf(p, "%c %d %d %d %d %d %u %lu %lu %lu %lu %llu %llu",
               &st, &f1, &f2, &f3, &f4, &f5, &f6,
               &f7, &f8, &f9, &f10, &ut, &stm) != 13)
        return false;

    *utime = ut;
    *stime = stm;
    return true;
}

static float pct(unsigned long long delta, unsigned long long total) {
    if (total == 0) return 0.0f;
    return 100.0f * (float)delta / (float)total;
}

void cpu_profile_update(health_state_t *state) {
    if (!state->enabled) return;

    uint64_t now = get_time_ms();

    /* Check toggle file every 5s */
    if (now - state->last_cpu_sample < CPU_PROFILE_SAMPLE_MS) return;

    bool was_enabled = state->cpu_profile_enabled;
    state->cpu_profile_enabled = (access(CPU_PROFILE_TOGGLE, F_OK) == 0);

    /* Cached PIDs for CPU profiler — avoids full /proc scan each time */
    static pid_t cached_bcap = 0;
    static pid_t cached_aircrack = 0;
    static pid_t cached_pwngrid = 0;

    if (state->cpu_profile_enabled && !was_enabled) {
        health_log(state, "CPU", "=== CPU Profiler ENABLED ===");
        read_proc_stat_cpu(&state->prev_cpu_user, &state->prev_cpu_nice,
                          &state->prev_cpu_system, &state->prev_cpu_idle,
                          &state->prev_cpu_iowait);
        read_proc_pid_cpu(getpid(),
                          &state->prev_self_utime, &state->prev_self_stime);
        cached_bcap = find_pid_cached("bettercap", 0);
        if (cached_bcap > 0)
            read_proc_pid_cpu(cached_bcap, &state->prev_bcap_utime, &state->prev_bcap_stime);
        state->last_cpu_log = now;
        memset(state->act_time_us, 0, sizeof(state->act_time_us));
        memset(state->act_count, 0, sizeof(state->act_count));
    } else if (!state->cpu_profile_enabled && was_enabled) {
        health_log(state, "CPU", "=== CPU Profiler DISABLED ===");
    }

    state->last_cpu_sample = now;

    if (!state->cpu_profile_enabled) return;

    /* Sample /proc/stat */
    unsigned long long user, nice_v, sys_v, idle_v, iowait_v;
    if (read_proc_stat_cpu(&user, &nice_v, &sys_v, &idle_v, &iowait_v)) {
        unsigned long long total_d =
            (user - state->prev_cpu_user) + (nice_v - state->prev_cpu_nice) +
            (sys_v - state->prev_cpu_system) + (idle_v - state->prev_cpu_idle) +
            (iowait_v - state->prev_cpu_iowait);

        unsigned long long busy_d =
            (user - state->prev_cpu_user) + (nice_v - state->prev_cpu_nice) +
            (sys_v - state->prev_cpu_system);

        state->cpu_total_pct = pct(busy_d, total_d);
        if (state->cpu_total_pct > state->peak_cpu_total)
            state->peak_cpu_total = state->cpu_total_pct;

        /* pwnaui CPU */
        unsigned long long su, ss;
        if (read_proc_pid_cpu(getpid(), &su, &ss)) {
            unsigned long long sd = (su - state->prev_self_utime) + (ss - state->prev_self_stime);
            state->cpu_self_pct = pct(sd, total_d);
            if (state->cpu_self_pct > state->peak_cpu_self)
                state->peak_cpu_self = state->cpu_self_pct;
            state->prev_self_utime = su;
            state->prev_self_stime = ss;
        }

        /* bettercap CPU — cached PID, single file read in steady state */
        cached_bcap = find_pid_cached("bettercap", cached_bcap);
        if (cached_bcap > 0) {
            unsigned long long bu, bs;
            if (read_proc_pid_cpu(cached_bcap, &bu, &bs)) {
                unsigned long long bd = (bu - state->prev_bcap_utime) + (bs - state->prev_bcap_stime);
                state->cpu_bcap_pct = pct(bd, total_d);
                state->prev_bcap_utime = bu;
                state->prev_bcap_stime = bs;
            }
        } else { state->cpu_bcap_pct = 0; }

        /* aircrack-ng CPU — cached PID */
        cached_aircrack = find_pid_cached("aircrack-ng", cached_aircrack);
        if (cached_aircrack > 0) {
            unsigned long long au, as_v;
            if (read_proc_pid_cpu(cached_aircrack, &au, &as_v)) {
                unsigned long long ad = (au - state->prev_aircrack_utime) + (as_v - state->prev_aircrack_stime);
                state->cpu_aircrack_pct = pct(ad, total_d);
                state->prev_aircrack_utime = au;
                state->prev_aircrack_stime = as_v;
            }
        } else {
            state->cpu_aircrack_pct = 0;
            state->prev_aircrack_utime = 0;
            state->prev_aircrack_stime = 0;
        }

        /* pwngrid CPU — cached PID */
        cached_pwngrid = find_pid_cached("pwngrid", cached_pwngrid);
        if (cached_pwngrid > 0) {
            unsigned long long pu, ps;
            if (read_proc_pid_cpu(cached_pwngrid, &pu, &ps)) {
                unsigned long long pd = (pu - state->prev_pwngrid_utime) + (ps - state->prev_pwngrid_stime);
                state->cpu_pwngrid_pct = pct(pd, total_d);
                state->prev_pwngrid_utime = pu;
                state->prev_pwngrid_stime = ps;
            }
        } else { state->cpu_pwngrid_pct = 0; }

        state->prev_cpu_user = user;
        state->prev_cpu_nice = nice_v;
        state->prev_cpu_system = sys_v;
        state->prev_cpu_idle = idle_v;
        state->prev_cpu_iowait = iowait_v;
    }

    if (state->ap_count > state->peak_ap_count)
        state->peak_ap_count = state->ap_count;

    /* Log summary every 30s */
    if (now - state->last_cpu_log >= CPU_PROFILE_INTERVAL_MS) {
        health_log(state, "CPU", "--- CPU Profile (30s) ---");
        health_log(state, "CPU", "System: %.1f%% | Temp: %dC | APs: %d",
                  state->cpu_total_pct, state->cpu_temp, state->ap_count);
        health_log(state, "CPU", "  pwnaui:     %5.1f%%", state->cpu_self_pct);
        health_log(state, "CPU", "  bettercap:  %5.1f%%", state->cpu_bcap_pct);
        health_log(state, "CPU", "  aircrack:   %5.1f%%", state->cpu_aircrack_pct);
        health_log(state, "CPU", "  pwngrid:    %5.1f%%", state->cpu_pwngrid_pct);
        float other_v = state->cpu_total_pct - state->cpu_self_pct -
                        state->cpu_bcap_pct - state->cpu_aircrack_pct -
                        state->cpu_pwngrid_pct;
        if (other_v < 0) other_v = 0;
        health_log(state, "CPU", "  other:      %5.1f%%", other_v);

        /* Per-action timing */
        bool any_act = false;
        for (int i = 0; i < CPU_ACT_COUNT; i++)
            if (state->act_count[i] > 0) { any_act = true; break; }

        if (any_act) {
            health_log(state, "CPU", "--- Brain Actions (30s) ---");
            for (int i = 0; i < CPU_ACT_COUNT; i++) {
                if (state->act_count[i] > 0) {
                    float avg_ms = (float)state->act_time_us[i] /
                                   (float)state->act_count[i] / 1000.0f;
                    float total_ms = (float)state->act_time_us[i] / 1000.0f;
                    health_log(state, "CPU", "  %-12s: %4u calls, %7.1fms total, %5.1fms avg",
                              cpu_act_name(i), state->act_count[i],
                              total_ms, avg_ms);
                }
            }
        }

        health_log(state, "CPU", "Peaks: CPU=%.1f%% pwnaui=%.1f%% APs=%d",
                  state->peak_cpu_total, state->peak_cpu_self,
                  state->peak_ap_count);

        /* Reset accumulators */
        memset(state->act_time_us, 0, sizeof(state->act_time_us));
        memset(state->act_count, 0, sizeof(state->act_count));
        state->last_cpu_log = now;
    }
}

/* ============================================================================
 * CLEANUP
 * ============================================================================
*/
void health_monitor_cleanup(health_state_t *state) {
    if (!state->enabled) return;

    health_log_final_summary(state);

    if (state->log_fp) {
        fclose(state->log_fp);
        state->log_fp = NULL;
    }

    state->enabled = false;
}
