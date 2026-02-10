/*
 * PwnaUI Health Monitor Plugin
 * 
 * Monitors system health and logs issues for post-walk review.
 * Toggle via config: main.plugins.health_monitor.enabled = true
 * 
 * Logs to: /tmp/pwnagotchi_health.log
 * 
 * What it tracks:
 * - Blind epochs (WiFi sees 0 APs)
 * - Channel stuck (not hopping)
 * - WiFi interface disappearing (wlan0mon gone)
 * - Nexmon driver crashes (brcmfmac errors)
 * - Service crashes (bettercap/pwnaui restarts)
 * - Memory low
 * - CPU temp/throttling
 * - Handshake counts
 * - Deauth/assoc counts
 * - Uptime
 */

#ifndef PWNAUI_HEALTH_MONITOR_H
#define PWNAUI_HEALTH_MONITOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

/* Log file path */
#define HEALTH_LOG_PATH "/tmp/pwnagotchi_health.log"
#define HEALTH_LOG_MAX_SIZE (5 * 1024 * 1024)  /* 5MB max before rotation */

/* CPU profiler toggle: touch /tmp/cpu_profile to enable */
#define CPU_PROFILE_TOGGLE "/tmp/cpu_profile"
#define CPU_PROFILE_INTERVAL_MS 30000  /* Log every 30s when enabled */
#define CPU_PROFILE_SAMPLE_MS  5000   /* Sample /proc/stat every 5s */

/* Brain action IDs for profiling */
#define CPU_ACT_BCAP_POLL    0
#define CPU_ACT_CHANNEL_HOP  1
#define CPU_ACT_DEAUTH       2
#define CPU_ACT_ASSOC        3
#define CPU_ACT_CSA          4
#define CPU_ACT_PROBE        5
#define CPU_ACT_ROGUE_M2     6
#define CPU_ACT_REASSOC      7
#define CPU_ACT_DISASSOC     8
#define CPU_ACT_CRACK_CHECK  9
#define CPU_ACT_EPOCH_END    10
#define CPU_ACT_HS_SCAN      11
#define CPU_ACT_DISPLAY      12
#define CPU_ACT_THOMPSON     13
#define CPU_ACT_BCAP_POLL_APS 14
#define CPU_ACT_ATTACK       15
#define CPU_ACT_COUNT        16

/* Check intervals */
#define HEALTH_CHECK_INTERVAL_MS    5000   /* Full health check every 5s */
#define HEALTH_WIFI_CHECK_MS        1000   /* WiFi check every 1s */
#define HEALTH_NEXMON_CHECK_MS      2000   /* Nexmon check every 2s */

/* Thresholds */
#define HEALTH_BLIND_THRESHOLD      5      /* 5 consecutive blind = alert */
#define HEALTH_CHANNEL_STUCK_THRESH 10     /* 10 epochs same channel = stuck */
#define HEALTH_MEM_LOW_MB           50     /* Warn when < 50MB free */
#define HEALTH_TEMP_WARN_C          65     /* Warn at 65C */
#define HEALTH_TEMP_CRIT_C          75     /* Critical at 75C */

/* Health state tracking */
typedef struct {
    /* Enable flag */
    bool enabled;
    
    /* Log file */
    FILE *log_fp;
    time_t start_time;
    
    /* WiFi/Blind tracking */
    int blind_count;           /* Consecutive blind epochs */
    bool was_blind;            /* Previous state */
    time_t blind_start;        /* When blind started */
    int total_blind_events;    /* Total blind events this session */
    int total_blind_seconds;   /* Total seconds blind */
    
    /* Channel tracking */
    int last_channel;
    int channel_stuck_count;
    time_t channel_stuck_start;
    
    /* Interface tracking */
    bool wlan0mon_exists;
    bool wlan0_exists;
    int interface_loss_count;
    
    /* Nexmon/driver tracking */
    int nexmon_errors;
    int brcmfmac_reloads;
    time_t last_nexmon_error;
    
    /* Service tracking */
    int bettercap_restarts;
    time_t last_bettercap_start;
    pid_t bettercap_pid;
    
    /* Stats */
    int handshake_count;
    int deauth_count;
    int assoc_count;
    int epoch_count;
    int ap_count;              /* Current visible APs */
    int max_ap_count;          /* Max APs seen at once */
    
    /* System */
    int mem_free_mb;
    int cpu_temp;
    bool throttled;
    int throttle_count;
    
    /* Timestamps for periodic checks */
    uint64_t last_health_check;
    uint64_t last_wifi_check;
    uint64_t last_nexmon_check;
    uint64_t last_stats_log;
    
    /* CPU Profiler state */
    bool cpu_profile_enabled;
    uint64_t last_cpu_sample;
    uint64_t last_cpu_log;

    /* /proc/stat previous values for delta calculation */
    unsigned long long prev_cpu_user;
    unsigned long long prev_cpu_nice;
    unsigned long long prev_cpu_system;
    unsigned long long prev_cpu_idle;
    unsigned long long prev_cpu_iowait;

    /* Per-process previous CPU ticks */
    unsigned long long prev_self_utime;
    unsigned long long prev_self_stime;
    unsigned long long prev_bcap_utime;
    unsigned long long prev_bcap_stime;
    unsigned long long prev_aircrack_utime;
    unsigned long long prev_aircrack_stime;
    unsigned long long prev_pwngrid_utime;
    unsigned long long prev_pwngrid_stime;

    /* Latest computed percentages */
    float cpu_total_pct;
    float cpu_self_pct;
    float cpu_bcap_pct;
    float cpu_aircrack_pct;
    float cpu_pwngrid_pct;

    /* Per-action timing (cumulative microseconds per interval) */
    uint64_t act_time_us[CPU_ACT_COUNT];
    uint32_t act_count[CPU_ACT_COUNT];

    /* Peak tracking */
    float peak_cpu_total;
    float peak_cpu_self;
    int   peak_ap_count;

} health_state_t;

/*
 * Initialize health monitor
 * Opens log file, initializes state
 * Returns 0 on success, -1 on error
 */
int health_monitor_init(health_state_t *state, bool enabled);

/*
 * Log a message with timestamp
 */
void health_log(health_state_t *state, const char *level, const char *fmt, ...);

/*
 * Update health monitor - call from main loop
 * Performs periodic checks based on elapsed time
 */
void health_monitor_update(health_state_t *state);

/*
 * Report epoch data (call after each epoch)
 */
void health_report_epoch(health_state_t *state, int epoch, int ap_count, 
                         int channel, bool blind);

/*
 * Report handshake captured
 */
void health_report_handshake(health_state_t *state, const char *ap_name);

/*
 * Report deauth sent
 */
void health_report_deauth(health_state_t *state);

/*
 * Report association
 */
void health_report_assoc(health_state_t *state);

/*
 * Report WiFi interface change
 */
void health_report_wifi_change(health_state_t *state, bool has_wlan0mon, 
                                bool has_wlan0);

/*
 * Report nexmon/driver event
 */
void health_report_nexmon_event(health_state_t *state, const char *event);

/*
 * Report bettercap event
 */
void health_report_bettercap_event(health_state_t *state, const char *event);

/*
 * Write periodic stats summary to log
 */
void health_log_stats(health_state_t *state);

/*
 * Write final summary on shutdown
 */
void health_log_final_summary(health_state_t *state);

/*
 * Cleanup - close log, write final stats
 */
void health_monitor_cleanup(health_state_t *state);

/*
 * Check if log needs rotation
 */
void health_check_log_rotation(health_state_t *state);

/*
 * CPU Profiler functions.
 * Toggle: touch /tmp/cpu_profile to enable, rm to disable.
 */
void cpu_profile_update(health_state_t *state);
uint64_t cpu_act_start(void);
void cpu_act_end(health_state_t *state, int action_id, uint64_t start_us);
const char *cpu_act_name(int action_id);

#endif /* PWNAUI_HEALTH_MONITOR_H */
