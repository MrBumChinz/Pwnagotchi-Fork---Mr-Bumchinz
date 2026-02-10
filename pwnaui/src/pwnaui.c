/*
 * PwnaUI - High-Performance C UI Renderer for Pwnagotchi
 * 
 * Main daemon that handles all UI rendering via UNIX socket IPC.
 * Replaces Python/PIL UI with native C for 10-30?? performance improvement.
 * 
 * Author: PwnaUI Project
 * License: MIT
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <dirent.h>
#include "pcap_check.h"
#include <syslog.h>
#include <time.h>
#include <stdarg.h>
#include <pthread.h>

#include "ipc.h"
#include "renderer.h"
#include "display.h"
#include "font.h"
#include "icons.h"
#include "plugins.h"
#include "themes.h"
#include "bcap_ws.h"
#include "brain.h"
#include "health_monitor.h"
#include "webserver.h"
#include "attack_log.h"
#include "pisugar.h"

/* Configuration */
#define SOCKET_PATH         "/var/run/pwnaui.sock"
#define PID_FILE            "/var/run/pwnaui.pid"
#define HEALTH_LOG_PATH     "/tmp/pwnagotchi_health.log"
#define MAX_CLIENTS         64      /* Handle burst connections - must be >= SOCKET_BACKLOG in ipc.c */
#define BUFFER_SIZE         1024
#define UPDATE_INTERVAL_MS  500     /* 2 Hz partial refresh - matches animation timing */

/* Global state */
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reload_config = 0;
static int g_daemon_mode = 0;
static int g_verbose = 0;
static int g_native_plugins = 0;    /* Enable native C plugins */

/* Static allocation, no malloc in hot paths */
static ui_state_t g_ui_state;
static uint8_t g_framebuffer[DISPLAY_MAX_WIDTH * DISPLAY_MAX_HEIGHT / 8];
static volatile int g_dirty = 0;
static uint64_t g_last_update_ms = 0;

/* Display thread state */
static pthread_t g_display_thread;
static pthread_mutex_t g_ui_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_display_cond = PTHREAD_COND_INITIALIZER;
static volatile int g_display_pending = 0;  /* Signal display thread to render */
static uint8_t g_display_fb[DISPLAY_MAX_WIDTH * DISPLAY_MAX_HEIGHT / 8];  /* Copy for display thread */

/* Native C plugins state */
static plugin_state_t g_plugins;
static uint64_t g_last_full_refresh_ms = 0;

/* GPS CNCplugin enabled flag (separate from native_plugins for future flexibility) */
static int g_gps_enabled = 0;

/* Bettercap WebSocket client state */
static int g_bcap_enabled = 0;
static bcap_ws_ctx_t *g_bcap_ctx = NULL;
static int g_bcap_ap_count = 0;
static int g_bcap_handshake_count = 0;
static int g_bcap_total_aps = 0;  /* Lifetime total APs seen */
static char g_seen_macs[512][18];  /* Track unique MACs we've seen */
static int g_seen_mac_count = 0;

/* Check if MAC was already seen */
static int mac_already_seen(const char *mac) {
    for (int i = 0; i < g_seen_mac_count; i++) {
        if (strcasecmp(g_seen_macs[i], mac) == 0) return 1;
    }
    return 0;
}

/* Add MAC to seen list */
static void add_seen_mac(const char *mac) {
    if (g_seen_mac_count < 512 && !mac_already_seen(mac)) {
        strncpy(g_seen_macs[g_seen_mac_count], mac, 17);
        g_seen_macs[g_seen_mac_count][17] = '\0';
        g_seen_mac_count++;
    }
}

/* Handshake dedup: track BSSIDs we've already shown handshake notification for */
static char g_hs_seen_macs[256][18];
static int g_hs_seen_count = 0;

static int hs_mac_already_seen(const char *mac) {
    for (int i = 0; i < g_hs_seen_count; i++) {
        if (strcasecmp(g_hs_seen_macs[i], mac) == 0) return 1;
    }
    return 0;
}

static void add_hs_seen_mac(const char *mac) {
    if (g_hs_seen_count < 256 && !hs_mac_already_seen(mac)) {
        strncpy(g_hs_seen_macs[g_hs_seen_count], mac, 17);
        g_hs_seen_macs[g_hs_seen_count][17] = '\0';
        g_hs_seen_count++;
    }
}

/* Thompson Sampling Brain (native replacement for Python pwnagotchi) */
static int g_brain_enabled = 0;
static brain_ctx_t *g_brain_ctx = NULL;
static int g_webserver_fd = -1;
static pisugar_ctx_t *g_pisugar = NULL;  /* PiSugar button handler */
static health_state_t g_health;

/* Uptime tracking (independent of brain epochs) */
static time_t g_start_time = 0;
static time_t g_last_uptime_update = 0;
static time_t g_last_stats_scan = 0;

/* Convert brain mood enum to string */
static const char *brain_mood_str(brain_mood_t mood) {
    static const char *mood_names[] = {
        "starting", "ready", "normal", "bored", "sad",
        "angry", "lonely", "excited", "grateful", "sleeping", "rebooting"
    };
    if (mood >= 0 && mood < MOOD_NUM_MOODS) return mood_names[mood];
    return "unknown";
}

/* Forward declarations */
static void trigger_display_update(void);
static void *display_thread_func(void *arg);

/*
 * Logging
 */
static void pwnaui_log(int priority, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    
    if (g_daemon_mode) {
        vsyslog(priority, fmt, ap);
    } else {
        FILE *out = (priority <= LOG_WARNING) ? stderr : stdout;
        vfprintf(out, fmt, ap);
        fprintf(out, "\n");
    }
    
    va_end(ap);
}

#define PWNAUI_LOG_INFO(...)    pwnaui_log(LOG_INFO, __VA_ARGS__)
#define PWNAUI_LOG_WARN(...)    pwnaui_log(LOG_WARNING, __VA_ARGS__)
#define PWNAUI_LOG_ERR(...)     pwnaui_log(LOG_ERR, __VA_ARGS__)
#define PWNAUI_LOG_DEBUG(...)   do { if (g_verbose) pwnaui_log(LOG_DEBUG, __VA_ARGS__); } while(0)



/* ==========================================================================
 * Face System - ASCII art faces for each mood/action
 * Match the original Python faces.py
 * ========================================================================== */


/* Face PNG state for each mood - DIRECT PNG MAPPING */
static const face_state_t MOOD_FACE_STATES[] = {
    FACE_EXCITED,       /* MOOD_STARTING - waking up! */
    FACE_COOL,          /* MOOD_READY - ready to play */
    FACE_LOOK_R,        /* MOOD_NORMAL - hunting */
    FACE_DEMOTIVATED,   /* MOOD_BORED - all APs pwned */
    FACE_SAD,           /* MOOD_SAD - nothing working */
    FACE_ANGRY,         /* MOOD_ANGRY - frustrated */
    FACE_LONELY,        /* MOOD_LONELY - blind */
    FACE_LOOK_R_HAPPY,  /* MOOD_EXCITED - on a roll! */
    FACE_FRIEND,        /* MOOD_GRATEFUL - friends! */
    FACE_SLEEP1,        /* MOOD_SLEEPING - zzz */
    FACE_BROKEN,        /* MOOD_REBOOTING - dying */
};

/* Get face state for mood - returns PNG enum directly */
static face_state_t get_face_state_for_mood(brain_mood_t mood) {
    if ((int)mood >= 0 && (int)mood < MOOD_NUM_MOODS) {
        return MOOD_FACE_STATES[mood];
    }
    return FACE_LOOK_R;
}

/* ==========================================================================
 * Voice System - Random status messages for each mood
 * ========================================================================== */

static const char *VOICE_STARTING[] = {
    "Coffee time! Wake up, wake up!",
    NULL
};

static const char *VOICE_READY[] = {
    "Ahhh... now we're ready to play.",
    NULL
};

static const char *VOICE_NORMAL[] = {
    "Ooo--what's over there?",
    NULL
};

static const char *VOICE_BORED[] = {
    "We've been here already... can we go for a walk?",
    NULL
};

static const char *VOICE_SAD[] = {
    "I can see them... but nothing's working. Why won't they share?",
    NULL
};

static const char *VOICE_SAD_NO_CLIENTS[] = {
    "They're all locked up tight... no one's coming or going.",
    NULL
};

static const char *VOICE_SAD_WPA3[] = {
    "WPA3 everywhere... they're too smart for my tricks.",
    NULL
};

static const char *VOICE_SAD_WEAK[] = {
    "I can barely hear them from here...",
    NULL
};

static const char *VOICE_SAD_DEAUTHS[] = {
    "I keep knocking but nobody answers...",
    NULL
};

static const char *VOICE_ANGRY[] = {
    "I've been trying forever and NOTHING is working! Ugh!",
    NULL
};

static const char *VOICE_ANGRY_NO_CLIENTS[] = {
    "Not a single client to kick off! Just locked doors everywhere! Ugh!",
    NULL
};

static const char *VOICE_ANGRY_WPA3[] = {
    "Stupid WPA3! My attacks just bounce right off! Ugh!",
    NULL
};

static const char *VOICE_ANGRY_WEAK[] = {
    "They're all so far away! I'm screaming but they can't hear me! Ugh!",
    NULL
};

static const char *VOICE_ANGRY_DEAUTHS[] = {
    "I've sent a million deauths and NOTHING came back! Ugh!",
    NULL
};

static const char *VOICE_LONELY[] = {
    "I can't see anything... hold me.",
    NULL
};

static const char *VOICE_EXCITED[] = {
    "We're on a roll! I'm doing so good!",
    NULL
};

static const char *VOICE_GRATEFUL[] = {
    "Friends!",
    NULL
};

static const char *VOICE_SLEEPING[] = {
    "Mmm... nap time. Wake me if something happens.",
    NULL
};

static const char *VOICE_REBOOTING[] = {
    "Uh-oh... I don't feel so good... I need a restart.",
    NULL
};

static const char **VOICE_MESSAGES[] = {
    VOICE_STARTING,
    VOICE_READY,
    VOICE_NORMAL,
    VOICE_BORED,
    VOICE_SAD,
    VOICE_ANGRY,
    VOICE_LONELY,
    VOICE_EXCITED,
    VOICE_GRATEFUL,
    VOICE_SLEEPING,
    VOICE_REBOOTING
};

/* Action-specific voices */
static const char *VOICE_DEAUTH[] = {
    "Booted that client right off~ No Wi-Fi for you!",
    NULL
};

static const char *VOICE_ASSOC[] = {
    "Snatching that juicy PMKID... mmm, tasty hash incoming~",
    NULL
};

static const char *VOICE_HANDSHAKE[] = {
    "Got it! I'm saving this little treasure!",
    NULL
};

/* Get context-aware voice for SAD/ANGRY based on frustration diagnosis */
static const char *get_frustration_voice(brain_mood_t mood, brain_frustration_t reason) {
    if (mood == MOOD_SAD) {
        switch (reason) {
            case FRUST_NO_CLIENTS:      return VOICE_SAD_NO_CLIENTS[0];
            case FRUST_WPA3:            return VOICE_SAD_WPA3[0];
            case FRUST_WEAK_SIGNAL:     return VOICE_SAD_WEAK[0];
            case FRUST_DEAUTHS_IGNORED: return VOICE_SAD_DEAUTHS[0];
            default:                    return VOICE_SAD[0];
        }
    } else { /* MOOD_ANGRY */
        switch (reason) {
            case FRUST_NO_CLIENTS:      return VOICE_ANGRY_NO_CLIENTS[0];
            case FRUST_WPA3:            return VOICE_ANGRY_WPA3[0];
            case FRUST_WEAK_SIGNAL:     return VOICE_ANGRY_WEAK[0];
            case FRUST_DEAUTHS_IGNORED: return VOICE_ANGRY_DEAUTHS[0];
            default:                    return VOICE_ANGRY[0];
        }
    }
}

/* Get random voice message for mood */
static const char *brain_get_voice(brain_mood_t mood) {
    if (mood < 0 || mood >= MOOD_NUM_MOODS) return "...";
    const char **messages = VOICE_MESSAGES[mood];
    int count = 0;
    while (messages[count] != NULL) count++;
    if (count == 0) return "...";
    return messages[rand() % count];
}

/* Get random voice message from array */
static const char *get_random_voice(const char **messages) {
    int count = 0;
    while (messages[count] != NULL) count++;
    if (count == 0) return "...";
    return messages[rand() % count];
}
/* ==========================================================================
 * Stats Scanner - Read handshake/crack stats from disk (with mtime cache)
 * ========================================================================== */


#define HANDSHAKES_DIR "/home/pi/handshakes"
#define POTFILE_PATH "/home/pi/handshakes/wpa-sec.cracked.potfile"
#define XP_FILE "/var/lib/pwnagotchi/pwnhub_xp.txt"
#define FOOD_FILE "/var/lib/pwnagotchi/pwnhub_food.txt"
#define FOOD_MAX 1000

/* Pcap cache - stores mtime and parsed result to avoid re-parsing unchanged files */
typedef struct {
    char filename[256];
    time_t mtime;
    int result;  /* 0=none, 1=partial, 2=full */
} pcap_cache_entry_t;

#define PCAP_CACHE_SIZE 64
static pcap_cache_entry_t g_pcap_cache[PCAP_CACHE_SIZE];
static int g_pcap_cache_count = 0;
static time_t g_potfile_mtime = 0;  /* tracks potfile changes for sync */

/* Count .pcap files in handshakes directory (for TCAPS display) */
static int count_pcap_files(void) {
    int count = 0;
    DIR *dir = opendir(HANDSHAKES_DIR);
    if (!dir) return 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            const char *name = entry->d_name;
            size_t len = strlen(name);
            if (len > 5 && strcmp(name + len - 5, ".pcap") == 0) {
                count++;
            }
        }
    }
    closedir(dir);
    return count;
}

/* Save XP state to disk with fsync for power-loss safety */
static void save_xp_state(int total_xp) {
    FILE *f = fopen(XP_FILE, "w");
    if (f) {
        fprintf(f, "%d\n", total_xp);
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
}

/* Save food state to disk with fsync for power-loss safety */
static void save_food_state(int food) {
    FILE *f = fopen(FOOD_FILE, "w");
    if (f) {
        fprintf(f, "%d\n", food);
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
}

/* Find cache entry for a pcap file */
static pcap_cache_entry_t* pcap_cache_find(const char *filename) {
    for (int i = 0; i < g_pcap_cache_count; i++) {
        if (strcmp(g_pcap_cache[i].filename, filename) == 0) {
            return &g_pcap_cache[i];
        }
    }
    return NULL;
}

/* Add new cache entry */
static pcap_cache_entry_t* pcap_cache_add(const char *filename, time_t mtime, int result) {
    pcap_cache_entry_t *e;
    if (g_pcap_cache_count >= PCAP_CACHE_SIZE) {
        memmove(&g_pcap_cache[0], &g_pcap_cache[1], 
                sizeof(pcap_cache_entry_t) * (PCAP_CACHE_SIZE - 1));
        e = &g_pcap_cache[PCAP_CACHE_SIZE - 1];
    } else {
        e = &g_pcap_cache[g_pcap_cache_count++];
    }
    strncpy(e->filename, filename, sizeof(e->filename) - 1);
    e->filename[sizeof(e->filename) - 1] = '\0';
    e->mtime = mtime;
    e->result = result;
    return e;
}

/* Get handshake result for pcap - uses cache if mtime unchanged */
static int get_pcap_result_cached(const char *filepath, const char *filename, time_t mtime) {
    pcap_cache_entry_t *cached = pcap_cache_find(filename);
    if (cached && cached->mtime == mtime) {
        return cached->result;
    }
    
    handshake_info_t hs_info;
    int result = pcap_check_handshake(filepath, &hs_info);
    
    if (cached) {
        cached->mtime = mtime;
        cached->result = result;
    } else {
        pcap_cache_add(filename, mtime, result);
    }
    
    PWNAUI_LOG_INFO("[stats] Parsed %s: result=%d (M1:%d M2:%d M3:%d M4:%d PMKID:%d)",
                    filename, result, hs_info.has_m1, hs_info.has_m2, 
                    hs_info.has_m3, hs_info.has_m4, hs_info.has_pmkid);
    return result;
}

/* Scan handshakes directory and update stats (with caching) */
static void scan_handshake_stats(void) {
    DIR *dir;
    struct dirent *entry;
    int fhs = 0, phs = 0, pwds = 0;
    int scanned = 0, cached = 0;

    dir = opendir(HANDSHAKES_DIR);
    if (dir) {
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG) {
                const char *name = entry->d_name;
                size_t len = strlen(name);

                if (len > 5 && strcmp(name + len - 5, ".pcap") == 0) {
                    char pcap_path[512];
                    snprintf(pcap_path, sizeof(pcap_path), "%s/%s", HANDSHAKES_DIR, name);
                    
                    struct stat st;
                    if (stat(pcap_path, &st) != 0) continue;
                    
                    pcap_cache_entry_t *c = pcap_cache_find(name);
                    int result;
                    if (c && c->mtime == st.st_mtime) {
                        result = c->result;
                        cached++;
                    } else {
                        result = get_pcap_result_cached(pcap_path, name, st.st_mtime);
                        scanned++;
                    }
                    
                    if (result == 2) fhs++;
                    else if (result == 1) phs++;
                }
                else if (len > 6 && strcmp(name + len - 6, ".22000") == 0) {
                    char pcap_name[256];
                    snprintf(pcap_name, sizeof(pcap_name), "%.*s.pcap", (int)(len - 6), name);
                    char pcap_path[512];
                    snprintf(pcap_path, sizeof(pcap_path), "%s/%s", HANDSHAKES_DIR, pcap_name);
                    struct stat st;
                    if (stat(pcap_path, &st) != 0) phs++;
                }
            }
        }
        closedir(dir);
    }

    /* --- Unified PWDS: single source of truth = .key files in /home/pi/cracked/ ---
     * If wpa-sec potfile has new entries, sync them into .key files first,
     * then count .key files as the ONE authoritative cracked-password count.
     */

    /* Sync potfile → .key files (only when potfile changes) */
    struct stat pot_st;
    if (stat(POTFILE_PATH, &pot_st) == 0) {
        if (pot_st.st_mtime != g_potfile_mtime) {
            g_potfile_mtime = pot_st.st_mtime;
            FILE *fp = fopen(POTFILE_PATH, "r");
            if (fp) {
                char line[512];
                while (fgets(line, sizeof(line), fp)) {
                    /* potfile format: MAC:SSID:PASSWORD */
                    if (line[0] == '\0' || line[0] == '\n') continue;
                    /* Remove trailing newline */
                    size_t ll = strlen(line);
                    if (ll > 0 && line[ll-1] == '\n') line[ll-1] = '\0';
                    /* Parse: find first ':', then second ':' */
                    char *first_colon = strchr(line, ':');
                    if (!first_colon) continue;
                    char *second_colon = strchr(first_colon + 1, ':');
                    if (!second_colon) continue;
                    /* Extract SSID (between first and second colon) */
                    *second_colon = '\0';
                    const char *ssid = first_colon + 1;
                    const char *password = second_colon + 1;
                    if (strlen(ssid) == 0 || strlen(password) == 0) continue;
                    /* Write .key file if it doesn't already exist */
                    char keypath[256];
                    snprintf(keypath, sizeof(keypath), "/home/pi/cracked/%s.key", ssid);
                    if (access(keypath, F_OK) != 0) {
                        /* Ensure /home/pi/cracked/ exists */
                        mkdir("/home/pi/cracked", 0755);
                        FILE *kf = fopen(keypath, "w");
                        if (kf) {
                            fprintf(kf, "%s\n", password);
                            fclose(kf);
                            PWNAUI_LOG_INFO("[stats] Synced potfile crack → %s.key", ssid);
                        }
                    }
                }
                fclose(fp);
            }
        }
    }

    /* Count .key files — THE single source of truth for PWDS */
    {
        DIR *cracked_dir = opendir("/home/pi/cracked");
        if (cracked_dir) {
            struct dirent *ce;
            while ((ce = readdir(cracked_dir)) != NULL) {
                if (ce->d_type == DT_REG) {
                    const char *cname = ce->d_name;
                    size_t clen = strlen(cname);
                    if (clen > 4 && strcmp(cname + clen - 4, ".key") == 0) {
                        pwds++;
                    }
                }
            }
            closedir(cracked_dir);
        }
    }

    pthread_mutex_lock(&g_ui_mutex);
    g_ui_state.pwds = pwds;
    g_ui_state.fhs = fhs;
    g_ui_state.phs = phs;
    snprintf(g_ui_state.shakes, sizeof(g_ui_state.shakes), "%d", fhs);
    g_dirty = 1;
    pthread_mutex_unlock(&g_ui_mutex);

    if (scanned > 0) {
        PWNAUI_LOG_INFO("[stats] PWDS:%d FHS:%d PHS:%d (scanned:%d cached:%d)", 
                        pwds, fhs, phs, scanned, cached);
    }
}

/* Update uptime display (called every second from main loop) */
static void update_uptime_display(void) {
    if (g_start_time == 0) return;
    
    time_t now = time(NULL);
    int uptime_secs = (int)(now - g_start_time);
    
    int days = uptime_secs / 86400;
    int hours = (uptime_secs % 86400) / 3600;
    int mins = (uptime_secs % 3600) / 60;
    int secs = uptime_secs % 60;
    
    pthread_mutex_lock(&g_ui_mutex);
    snprintf(g_ui_state.uptime, sizeof(g_ui_state.uptime), "%02d:%02d:%02d:%02d", days, hours, mins, secs);
    g_dirty = 1;
    pthread_mutex_unlock(&g_ui_mutex);
}

/* ==========================================================================
/* ==========================================================================
 * Brain UI Callbacks - update display when brain changes state
 * ========================================================================== */

/* Hold attack phase display for minimum duration before mood can overwrite */
static time_t g_attack_phase_hold_until = 0;

/* DOWNLOAD animation auto-stop timer (handshake celebration) */
static time_t g_download_start_time = 0;
#define DOWNLOAD_DISPLAY_SECS  5   /* Show DOWNLOAD animation for 5 seconds */

/* Attack phase UI callback - shows what attack the brain is running */
static void brain_attack_phase_callback(int phase, void *user_data) {
    (void)user_data;
    static const char *ATTACK_VOICES[] = {
        "Snatching that juicy PMKID... mmm, tasty hash incoming~",       /* 0 */
        "Channel switch! Come follow me, little clients... hehe~",       /* 1 */
        "Booted that client right off~ No Wi-Fi for you!",               /* 2 */
        "Sneaky anon reassoc~ Your fancy protection can't stop me!",     /* 3 */
        "Double disassoc chaos! Both sides disconnected~ Bye bye!",      /* 4 */
        "Pretending to be the AP... now hand over that M2 hash, pretty please~", /* 5 */
        "Probing probing probing~ Who's hiding their SSID from me?",     /* 6 */
        "Shhh... I'm listening very carefully.",                          /* 7 */
        "I feel sick...",                                                  /* 8 */
        "I feel like getting on the CRACK!",                               /* 9 - idle cracking */
        "Cracked it! Password FOUND!",                                     /* 10 - key found! */
    };

    /* HULK rage quotes — random pick when phase 11 fires */
    static const char *HULK_VOICES[] = {
        "HULK SMASH YOUR WIFI!",
        "YOUR ROUTER IS MY TOILET!",
        "HULK ANGRY! DEAUTHING EVERYTHING!",
        "NOTHING WORKED? FINE. HULK MODE!",
        "ALL YOUR PACKETS BELONG TO HULK!",
        "NUCLEAR OPTION ENGAGED! SMASHING ALL APs!",
        "HULK TIRED OF BEING NICE! SMASH TIME!",
        "LAST RESORT! MAXIMUM CARNAGE!",
    };
    static const int HULK_VOICE_COUNT = sizeof(HULK_VOICES) / sizeof(HULK_VOICES[0]);

    pthread_mutex_lock(&g_ui_mutex);
    if (phase >= 0 && phase <= 10) {
        strncpy(g_ui_state.status, ATTACK_VOICES[phase], sizeof(g_ui_state.status) - 1);
    } else if (phase == 11) {
        /* HULK MODE: pick random rage quote */
        int idx = rand() % HULK_VOICE_COUNT;
        strncpy(g_ui_state.status, HULK_VOICES[idx], sizeof(g_ui_state.status) - 1);
    }
    if (phase == 7) {
        /* Listen: smart/observing face */
        animation_stop();
        g_ui_state.face_enum = FACE_SMART;
        strncpy(g_ui_state.face, "SMART", sizeof(g_ui_state.face) - 1);
    } else if (phase == 8) {
        /* WiFi recovery: broken face */
        animation_stop();
        g_ui_state.face_enum = FACE_BROKEN;
        strncpy(g_ui_state.face, "BROKEN", sizeof(g_ui_state.face) - 1);
    } else if (phase == 9) {
        /* Idle cracking started: SMART face */
        animation_stop();
        g_ui_state.face_enum = FACE_SMART;
        strncpy(g_ui_state.face, "SMART", sizeof(g_ui_state.face) - 1);
    } else if (phase == 10) {
        /* KEY FOUND! Celebrate with DOWNLOAD animation */
        animation_start(ANIM_DOWNLOAD, 500);
        g_download_start_time = time(NULL);
    } else if (phase == 11) {
        /* HULK SMASH! INTENSE face + fast UPLOAD animation (rage effect) */
        animation_start(ANIM_UPLOAD, 500);
        g_ui_state.face_enum = FACE_INTENSE;
        strncpy(g_ui_state.face, "INTENSE", sizeof(g_ui_state.face) - 1);
    } else {
        /* Attack phases 0-6: upload animation (00->01->10->11 @ 1s/frame) */
        animation_start(ANIM_UPLOAD, 1000);
    }
    g_dirty = 1;
    /* Hold attack face display until next attack phase fires.
     * For LISTEN (phase 7): brain sleeps 10s (recon_time), then mood fires --
     * we need the hold to outlast that sleep so mood doesn't overwrite SMART face.
     * 20s is safe margin over the 10s recon_time + epoch overhead.
     * Phase 9/10 (cracking): shorter hold -- cracking runs in background.
     * Phase 11 (HULK): 30s hold -- hulk smash takes time + dramatic effect. */
    if (phase == 9 || phase == 10) {
        g_attack_phase_hold_until = time(NULL) + 5;  /* Brief hold for crack messages */
    } else if (phase == 11) {
        g_attack_phase_hold_until = time(NULL) + 30; /* HULK hold: dramatic rage display */
    } else {
        g_attack_phase_hold_until = time(NULL) + 20;
    }
    pthread_mutex_unlock(&g_ui_mutex);
}

static void brain_mood_callback(brain_mood_t mood, void *user_data) {
    (void)user_data;
    const char *voice;
    face_state_t face_state = get_face_state_for_mood(mood);

    /* Context-aware messages for SAD/ANGRY */
    if ((mood == MOOD_SAD || mood == MOOD_ANGRY) && g_brain_ctx) {
        brain_frustration_t reason = brain_get_frustration(g_brain_ctx);
        voice = get_frustration_voice(mood, reason);
    } else {
        voice = brain_get_voice(mood);
    }

    pthread_mutex_lock(&g_ui_mutex);

    /* If attack phase display is still held, don't overwrite face/voice.
     * EXCEPT: MOOD_READY is high-priority — user must see FACE_COOL
     * when bettercap connects, even if an attack phase just fired. */
    if (mood != MOOD_READY && time(NULL) < g_attack_phase_hold_until) {
        pthread_mutex_unlock(&g_ui_mutex);
        return;
    }
    /* If MOOD_READY breaks through the hold, clear the hold timer
     * so the 3-second brain delay gives the user a clean COOL face. */
    if (mood == MOOD_READY) {
        g_attack_phase_hold_until = 0;
    }

    strncpy(g_ui_state.status, voice, sizeof(g_ui_state.status) - 1);
    
    /* Start/stop animations based on mood */
    if (mood == MOOD_NORMAL || mood == MOOD_STARTING) {
        animation_start(ANIM_LOOK, 2500);
    } else if (mood == MOOD_EXCITED) {
        animation_start(ANIM_LOOK_HAPPY, 2500);
    } else if (mood == MOOD_SLEEPING) {
        animation_start(ANIM_SLEEP, 2000);
    } else {
        animation_stop();
        g_ui_state.face_enum = face_state;
        strncpy(g_ui_state.face, g_face_state_names[face_state], sizeof(g_ui_state.face) - 1);
    }
    g_dirty = 1;
    pthread_mutex_unlock(&g_ui_mutex);
    PWNAUI_LOG_DEBUG("[mood] face_state=%d anim=%d -> %s", face_state, animation_is_active(), voice);
}

static void brain_epoch_callback(int epoch_num, const brain_epoch_t *data, void *user_data) {
    (void)user_data;

    /* Get real uptime from brain */
    int uptime_secs = 0;
    if (g_brain_ctx) {
        uptime_secs = brain_get_uptime(g_brain_ctx);
    }

    int days = uptime_secs / 86400;
    int hours = (uptime_secs % 86400) / 3600;
    int mins = (uptime_secs % 3600) / 60;
    int secs = uptime_secs % 60;

    /* Get AP count from bettercap */
    int ap_count = 0;
    if (g_bcap_ctx) {
        ap_count = bcap_get_ap_count(g_bcap_ctx);
    }

    fprintf(stderr, "[epoch] #%d uptime=%ds aps=%d\n", epoch_num, uptime_secs, ap_count);

    pthread_mutex_lock(&g_ui_mutex);
    snprintf(g_ui_state.uptime, sizeof(g_ui_state.uptime), "%02d:%02d:%02d:%02d", days, hours, mins, secs);
    snprintf(g_ui_state.aps, sizeof(g_ui_state.aps), "%d", ap_count);
    snprintf(g_ui_state.shakes, sizeof(g_ui_state.shakes), "%d", data->num_shakes);
    /* Channel is managed by brain_channel_callback — don't reset here */
    /* g_ui_state.tcaps set in brain callback - tracks lifetime APs */
    
    /* UPDATE PWNHUB FOOD POOL (unified macros) */
    {
        static int food = -1;       /* -1 = not yet loaded */
        static int prev_fhs = 0;
        static int prev_phs = 0;
        static int prev_pwds = 0;

        /* Load food from file on first call */
        if (food < 0) {
            FILE *f = fopen(FOOD_FILE, "r");
            if (f) {
                if (fscanf(f, "%d", &food) != 1) food = 0;
                fclose(f);
            }
            if (food < 0) food = 0;
            prev_fhs = g_ui_state.fhs;
            prev_phs = g_ui_state.phs;
            prev_pwds = g_ui_state.pwds;
            fprintf(stderr, "[food] Loaded food: %d (fhs=%d phs=%d pwds=%d)\n",
                    food, prev_fhs, prev_phs, prev_pwds);
        }

        /* Award food for NEW captures only (delta since last epoch) */
        int new_fhs = g_ui_state.fhs - prev_fhs;
        int new_phs = g_ui_state.phs - prev_phs;
        int new_pwds = g_ui_state.pwds - prev_pwds;
        if (new_fhs > 0) { food += new_fhs * 100; prev_fhs = g_ui_state.fhs; }
        if (new_phs > 0) { food += new_phs * 30;  prev_phs = g_ui_state.phs; }
        if (new_pwds > 0) { food += new_pwds * 200; prev_pwds = g_ui_state.pwds; }

        /* +5 per deauth/assoc attack action this epoch */
        int food_earned = (data->num_deauths + data->num_assocs) * 5;
        food += food_earned;

        /* Drain: only drain when idle (no attacks this epoch).
         * If actively attacking, you're eating! No drain while hunting. */
        if (food_earned == 0 && new_fhs == 0 && new_phs == 0 && new_pwds == 0) {
            food--;
        }
        if (food < 0) food = 0;
        if (food > FOOD_MAX) food = FOOD_MAX;

        /* Map food level to macro icons:
         *   >66% (>660) = all 3 icons (protein + fat + carbs)
         *   33-66% (330-660) = 2 icons (protein + fat)
         *   1-33% (1-329) = 1 icon (protein only)
         *   0 = no icons */
        if (food > 660) {
            g_ui_state.pwnhub_protein = 50;
            g_ui_state.pwnhub_fat = 50;
            g_ui_state.pwnhub_carbs = 50;
        } else if (food >= 330) {
            g_ui_state.pwnhub_protein = 50;
            g_ui_state.pwnhub_fat = 50;
            g_ui_state.pwnhub_carbs = 0;
        } else if (food >= 1) {
            g_ui_state.pwnhub_protein = 50;
            g_ui_state.pwnhub_fat = 0;
            g_ui_state.pwnhub_carbs = 0;
        } else {
            g_ui_state.pwnhub_protein = 0;
            g_ui_state.pwnhub_fat = 0;
            g_ui_state.pwnhub_carbs = 0;
        }

        /* Log food every epoch so we can see what's actually happening */
        {
            int icons = (food > 660) ? 3 : (food >= 330) ? 2 : (food >= 1) ? 1 : 0;
            fprintf(stderr, "[food] food=%d/%d icons=%d (earned=%d deauths=%d assocs=%d +fhs=%d +phs=%d +pwds=%d)\n",
                    food, FOOD_MAX, icons, food_earned, data->num_deauths, data->num_assocs, new_fhs, new_phs, new_pwds);
        }

        /* Save food state every epoch (survives power loss) */
        save_food_state(food);
    }
    
    /* UPDATE XP PROGRESSION - Prestige System with Persistence */
    {
        static int total_xp = -1;  /* -1 = not yet loaded */
        static int last_fhs = 0;   /* Track FHS changes for XP award */
        static int last_phs = 0;   /* Track PHS changes for XP award */

        /* Load XP from file on first call */
        if (total_xp < 0) {
            FILE* f = fopen(XP_FILE, "r");
            if (f) {
                if (fscanf(f, "%d", &total_xp) != 1) {
                    total_xp = 0;
                }
                fclose(f);
            }
            if (total_xp < 0) total_xp = 0;

            /* Bootstrap: ensure XP reflects existing pcap evidence.
             * Each pcap = at least 1 handshake = 100 XP minimum.
             * Each FULL handshake (FHS) worth 200 XP.
             * Prevents "level 2 with 22 pcaps" after power loss. */
            int pcap_count = count_pcap_files();
            int evidence_xp = (g_ui_state.fhs * 200) + ((pcap_count - g_ui_state.fhs) * 100);
            if (evidence_xp < 0) evidence_xp = 0;
            if (total_xp < evidence_xp) {
                fprintf(stderr, "[xp] Bootstrap: %d pcaps, %d FHS -> evidence_xp=%d (was %d)\n",
                        pcap_count, g_ui_state.fhs, evidence_xp, total_xp);
                total_xp = evidence_xp;
            }

            last_fhs = g_ui_state.fhs;
            last_phs = g_ui_state.phs;
            fprintf(stderr, "[xp] Loaded XP: %d (pcaps=%d fhs=%d phs=%d)\n",
                    total_xp, pcap_count, g_ui_state.fhs, g_ui_state.phs);
        }

        /* Award XP for new activity this epoch */
        int new_fhs = g_ui_state.fhs - last_fhs;
        int new_phs = g_ui_state.phs - last_phs;
        if (new_fhs > 0) { total_xp += new_fhs * 200; last_fhs = g_ui_state.fhs; }
        if (new_phs > 0) { total_xp += new_phs * 100; last_phs = g_ui_state.phs; }
        total_xp += data->num_deauths / 10 + 1;  /* Base XP per epoch */

        /* TCAPS = total pcap files (simple, accurate, no tracking bugs) */
        g_ui_state.tcaps = count_pcap_files();

        /* Calculate level: XP_needed = max(100, 10 * level * isqrt(level)) */
        int level = 1;
        int xp_check = total_xp;
        while (level < 9999) {
            int sq = 1;
            while ((sq+1)*(sq+1) <= level) sq++;
            int xp_needed = 10 * level * sq;
            if (xp_needed < 100) xp_needed = 100;
            if (xp_check < xp_needed) break;
            xp_check -= xp_needed;
            level++;
        }

        /* XP progress to next level */
        int sq = 1;
        while ((sq+1)*(sq+1) <= level) sq++;
        int xp_for_next = 10 * level * sq;
        if (xp_for_next < 100) xp_for_next = 100;

        g_ui_state.pwnhub_level = level;
        g_ui_state.pwnhub_xp_percent = (xp_for_next > 0) ? (xp_check * 100 / xp_for_next) : 0;
        if (g_ui_state.pwnhub_xp_percent > 99) g_ui_state.pwnhub_xp_percent = 99;

        /* Save XP EVERY epoch with fsync (survives power loss) */
        save_xp_state(total_xp);

        /* Stage titles */
        if (level >= 600) strncpy(g_ui_state.pwnhub_title, "Mythic", 23);
        else if (level >= 400) strncpy(g_ui_state.pwnhub_title, "Legendary", 23);
        else if (level >= 250) strncpy(g_ui_state.pwnhub_title, "Master", 23);
        else if (level >= 175) strncpy(g_ui_state.pwnhub_title, "Veteran", 23);
        else if (level >= 120) strncpy(g_ui_state.pwnhub_title, "Elite", 23);
        else if (level >= 80) strncpy(g_ui_state.pwnhub_title, "Predator", 23);
        else if (level >= 55) strncpy(g_ui_state.pwnhub_title, "Stalker", 23);
        else if (level >= 35) strncpy(g_ui_state.pwnhub_title, "Hunter", 23);
        else if (level >= 20) strncpy(g_ui_state.pwnhub_title, "Apprentice", 23);
        else if (level >= 10) strncpy(g_ui_state.pwnhub_title, "Rookie", 23);
        else if (level >= 5) strncpy(g_ui_state.pwnhub_title, "Newborn", 23);
        else strncpy(g_ui_state.pwnhub_title, "Hatchling", 23);
    }
    
    g_dirty = 1;
    pthread_mutex_unlock(&g_ui_mutex);

    /* Report to health monitor */
    bool blind = (data->blind_for > 0) || (ap_count == 0);
    health_report_epoch(&g_health, epoch_num, ap_count, 0, blind);
}

static void brain_channel_callback(int channel, void *user_data) {
    (void)user_data;
    if (channel < 1 || channel > 14) return;  /* 2.4GHz only: ch 1-14 */
    pthread_mutex_lock(&g_ui_mutex);
    snprintf(g_ui_state.channel, sizeof(g_ui_state.channel), "%02d", channel);
    g_dirty = 1;
    pthread_mutex_unlock(&g_ui_mutex);
}

/* NOTE: brain_handshake_callback removed - was never called by brain.c.
 * Handshake notifications are handled by bcap_on_event(BCAP_EVT_HANDSHAKE)
 * which fires on real bettercap wifi.client.handshake WebSocket events,
 * with dedup via hs_mac_already_seen() to prevent spam. */

/* ==========================================================================
 * Bettercap WebSocket Event Callbacks
 * ========================================================================== */

static void bcap_on_event(const bcap_event_t *event, void *user_data) {
    (void)user_data;
    char mac_str[18];
    
    switch (event->type) {
        case BCAP_EVT_AP_NEW:
            bcap_format_mac(&event->data.ap.bssid, mac_str);
            {
            /* Only count as NEW if we haven't seen this MAC before */
            bool is_genuinely_new = !mac_already_seen(mac_str);
            if (is_genuinely_new) {
                add_seen_mac(mac_str);
                g_bcap_total_aps++;
            }
            /* APS = current visible count (set directly, don't accumulate) */
            PWNAUI_LOG_DEBUG("[bcap] AP %s: %s (%s) ch=%d",
                           is_genuinely_new ? "NEW" : "REDISCOVERED",
                           mac_str, event->data.ap.ssid, event->data.ap.channel);
            /* Update UI state - use ACTUAL count from bettercap */
            int actual_ap_count = g_bcap_ctx ? bcap_get_ap_count(g_bcap_ctx) : 0;
            pthread_mutex_lock(&g_ui_mutex);
            snprintf(g_ui_state.aps, sizeof(g_ui_state.aps), "%d", actual_ap_count);
            /* Only show excited message for genuinely new APs (not re-discovered) */
            if (is_genuinely_new) {
                strncpy(g_ui_state.status, "Oh! Something new! Let's check it out!", sizeof(g_ui_state.status) - 1);
                animation_start(ANIM_LOOK_HAPPY, 2500);
            }
            g_dirty = 1;
            g_ui_state.tcaps = count_pcap_files();
            pthread_mutex_unlock(&g_ui_mutex);
            scan_handshake_stats();  /* Rescan to pick up new pcap */

            /* INSTANT-ATTACK: immediately associate with new AP for PMKID grab.
             * Don't wait for next epoch -- fresh APs are most receptive.
             * Only fire on genuinely new APs that haven't been handshake'd yet.
             * Uses LOCAL pcap cache (not bettercap's session-only flag).
             * Also checks stealth whitelist to never attack home/office networks. */
            if (is_genuinely_new && g_brain_ctx && g_bcap_ctx &&
                !brain_has_full_handshake(mac_str) &&
                !(g_brain_ctx->stealth && stealth_is_whitelisted(g_brain_ctx->stealth, event->data.ap.ssid))) {
                char assoc_cmd[128];
                snprintf(assoc_cmd, sizeof(assoc_cmd), "wifi.assoc %s", mac_str);
                bcap_send_command(g_bcap_ctx, assoc_cmd);
                PWNAUI_LOG_INFO("[instant-attack] ASSOC new AP %s (%s) ch%d",
                                mac_str, event->data.ap.ssid, event->data.ap.channel);

                /* Flash face/voice for insta-attack (only if no attack phase hold active) */
                pthread_mutex_lock(&g_ui_mutex);
                if (time(NULL) >= g_attack_phase_hold_until) {
                    strncpy(g_ui_state.status, "Fresh target! Grabbing PMKID NOW!",
                            sizeof(g_ui_state.status) - 1);
                    g_ui_state.face_enum = FACE_LOOK_R_HAPPY;
                    strncpy(g_ui_state.face, "LOOK_R_HAPPY", sizeof(g_ui_state.face) - 1);
                    animation_start(ANIM_UPLOAD, 1000);
                    g_attack_phase_hold_until = time(NULL) + 3; /* Brief 3s flash */
                    g_dirty = 1;
                }
                pthread_mutex_unlock(&g_ui_mutex);
            }
            }
            break;
            
        case BCAP_EVT_AP_LOST:
            if (g_bcap_ap_count > 0) g_bcap_ap_count--;
            pthread_mutex_lock(&g_ui_mutex);
            snprintf(g_ui_state.aps, sizeof(g_ui_state.aps), "%d", g_bcap_ap_count);
            g_dirty = 1;
            pthread_mutex_unlock(&g_ui_mutex);
            scan_handshake_stats();  /* Rescan to pick up new pcap */
            break;
            
        case BCAP_EVT_HANDSHAKE:
            g_bcap_handshake_count++;
            bcap_format_mac(&event->data.hs.ap_bssid, mac_str);
            /* Only show voice/animation for genuinely NEW handshake captures */
            if (!hs_mac_already_seen(mac_str)) {
                add_hs_seen_mac(mac_str);
                PWNAUI_LOG_INFO("[bcap] *** NEW HANDSHAKE *** AP=%s SSID=%s", mac_str, event->data.hs.ssid);
                pthread_mutex_lock(&g_ui_mutex);
                const char *hs_voice = get_random_voice(VOICE_HANDSHAKE);
                strncpy(g_ui_state.status, hs_voice, sizeof(g_ui_state.status) - 1);
                g_ui_state.face_enum = FACE_HAPPY;
                animation_start(ANIM_DOWNLOAD, 500);
                g_download_start_time = time(NULL);  /* Start auto-stop timer */
                g_dirty = 1;
                pthread_mutex_unlock(&g_ui_mutex);
            } else {
                PWNAUI_LOG_DEBUG("[bcap] handshake AP=%s (already captured, suppressing voice)", mac_str);
            }
            scan_handshake_stats();
            break;
            
        case BCAP_EVT_CLIENT_NEW:
            /* INSTANT-ATTACK: deauth newly discovered clients immediately.
             * Fresh client associations are prime handshake targets.
             * Only fire if client is associated to an AP we haven't captured yet.
             * Uses LOCAL pcap cache (not bettercap's session-only flag).
             * Also checks stealth whitelist to never attack home/office networks. */
            if (g_brain_ctx && g_bcap_ctx && event->data.sta.associated) {
                char sta_mac[18], ap_mac[18];
                bcap_format_mac(&event->data.sta.mac, sta_mac);
                bcap_format_mac(&event->data.sta.ap_bssid, ap_mac);
                /* Check if we already have the handshake for this AP (local pcap cache) */
                bcap_ap_t client_ap;
                bool ap_found = (bcap_find_ap(g_bcap_ctx, &event->data.sta.ap_bssid, &client_ap) == 0);
                bool ap_whitelisted = ap_found && g_brain_ctx->stealth &&
                    stealth_is_whitelisted(g_brain_ctx->stealth, client_ap.ssid);
                if (ap_found && !brain_has_full_handshake(ap_mac) && !ap_whitelisted) {
                    char deauth_cmd[128];
                    snprintf(deauth_cmd, sizeof(deauth_cmd), "wifi.deauth %s", sta_mac);
                    bcap_send_command(g_bcap_ctx, deauth_cmd);
                    PWNAUI_LOG_INFO("[instant-attack] DEAUTH new client %s on AP %s",
                                    sta_mac, ap_mac);

                    /* Flash face/voice for insta-deauth (only if no hold active) */
                    pthread_mutex_lock(&g_ui_mutex);
                    if (time(NULL) >= g_attack_phase_hold_until) {
                        strncpy(g_ui_state.status, "New client spotted! Deauthing on sight!",
                                sizeof(g_ui_state.status) - 1);
                        g_ui_state.face_enum = FACE_INTENSE;
                        strncpy(g_ui_state.face, "INTENSE", sizeof(g_ui_state.face) - 1);
                        animation_start(ANIM_UPLOAD, 500);
                        g_attack_phase_hold_until = time(NULL) + 3; /* Brief 3s flash */
                        g_dirty = 1;
                    }
                    pthread_mutex_unlock(&g_ui_mutex);
                }
            }
            /* fall through */
        case BCAP_EVT_CLIENT_LOST:
        case BCAP_EVT_CLIENT_PROBE:
            /* Track but don't spam logs */
            break;
            
        default:
            break;
    }
}

static void bcap_on_state_change(bool connected, void *user_data) {
    (void)user_data;
    PWNAUI_LOG_INFO("[bcap] Connection: %s", connected ? "CONNECTED" : "DISCONNECTED");
    
    if (!connected) {
        /* Reset counts on disconnect - will be repopulated on reconnect */
        g_bcap_ap_count = 0;
    }
}

/*
 * Signal handlers
 */
static void signal_handler(int sig) {
    switch (sig) {
        case SIGTERM:
        case SIGINT:
            g_running = 0;
            break;
        case SIGHUP:
            g_reload_config = 1;
            break;
    }
}

static void setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    
    /* Ignore SIGPIPE - handle write errors explicitly */
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
}

/*
 * Get current time in milliseconds
 */
static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * Initialize UI state with defaults
 */
/* Webserver state callback - provides current UI state as JSON */
/* Sprint 5: GPS callback for Crack City */
static void webserver_gps_cb(double *lat, double *lon, int *has_fix) {
    if (g_native_plugins && g_plugins.gps_enabled) {
        *lat = g_plugins.gps.latitude;
        *lon = g_plugins.gps.longitude;
        *has_fix = g_plugins.gps.has_fix ? 1 : 0;
    } else {
        *lat = 0.0;
        *lon = 0.0;
        *has_fix = 0;
    }
}

static void webserver_state_cb(char *buf, size_t bufsize) {
    pthread_mutex_lock(&g_ui_mutex);
    
    /* Get PNG face filename - use animated frame if animation is active */
    face_state_t face_state;
    if (animation_is_active()) {
        face_state = animation_get_frame();
    } else {
        face_state = g_ui_state.face_enum;
    }
    const char *face_png = theme_get_face_name(face_state);
    
    snprintf(buf, bufsize,
        "{\"face\":\"%s\",\"face_img\":\"%s.png\",\"status\":\"%s\",\"channel\":\"%s\","
        "\"aps\":\"%s\",\"uptime\":\"%s\",\"shakes\":\"%s\","
        "\"mode\":\"%s\",\"name\":\"%s\",\"bluetooth\":\"%s\","
        "\"battery\":\"%s\",\"gps\":\"%s\",\"pwds\":%d,\"fhs\":%d,\"phs\":%d,\"tcaps\":%d,"
        "\"memtemp\":\"%s\",\"pwnhub\":%d,\"protein\":%d,\"fat\":%d,\"carbs\":%d,\"xp\":%d,\"lvl\":%d,\"title\":\"%s\",\"wins\":%d,\"battles\":%d}",
        g_ui_state.face, face_png ? face_png : "", g_ui_state.status, g_ui_state.channel,
        g_ui_state.aps, g_ui_state.uptime, g_ui_state.shakes,
        g_ui_state.mode, g_ui_state.name, g_ui_state.bluetooth,
        g_ui_state.battery, g_ui_state.gps,
        g_ui_state.pwds, g_ui_state.fhs, g_ui_state.phs, g_ui_state.tcaps,
        g_ui_state.memtemp_data,
        g_ui_state.pwnhub_enabled, g_ui_state.pwnhub_protein, g_ui_state.pwnhub_fat, g_ui_state.pwnhub_carbs,
        g_ui_state.pwnhub_xp_percent, g_ui_state.pwnhub_level, g_ui_state.pwnhub_title,
        g_ui_state.pwnhub_wins, g_ui_state.pwnhub_battles);
    pthread_mutex_unlock(&g_ui_mutex);
}

static void init_ui_state(void) {
    memset(&g_ui_state, 0, sizeof(g_ui_state));
    memset(g_framebuffer, 0xFF, sizeof(g_framebuffer));  /* White background */
    
    /* Default values matching Pwnagotchi layout */
    strncpy(g_ui_state.name, "pwnagotchi>", sizeof(g_ui_state.name) - 1);
    g_ui_state.face_enum = FACE_LOOK_R;  /* Initial state - looking */
    strncpy(g_ui_state.status, "Waking up...", sizeof(g_ui_state.status) - 1);
    strncpy(g_ui_state.channel, "00", sizeof(g_ui_state.channel) - 1);
    strncpy(g_ui_state.aps, "0", sizeof(g_ui_state.aps) - 1);
    strncpy(g_ui_state.uptime, "00:00:00:00", sizeof(g_ui_state.uptime) - 1);
    strncpy(g_ui_state.shakes, "0", sizeof(g_ui_state.shakes) - 1);
    strncpy(g_ui_state.mode, "MANU", sizeof(g_ui_state.mode) - 1);
    strncpy(g_ui_state.status, "Initializing...", sizeof(g_ui_state.status) - 1);
    strncpy(g_ui_state.bluetooth, "BT-", sizeof(g_ui_state.bluetooth) - 1);
    strncpy(g_ui_state.gps, "GPS-", sizeof(g_ui_state.gps) - 1);
    
    g_ui_state.invert = 0;
    
    /* PwnHub defaults - enabled by default */
    g_ui_state.pwnhub_enabled = 1;
    g_ui_state.pwnhub_protein = 0;
    g_ui_state.pwnhub_fat = 0;
    g_ui_state.pwnhub_carbs = 0;
    /* Load persisted XP/level immediately so display is correct from boot */
    {
        int saved_xp = 0;
        FILE *xf = fopen(XP_FILE, "r");
        if (xf) {
            fscanf(xf, "%d", &saved_xp);
            fclose(xf);
        }
        /* Also credit existing pcap evidence */
        int pcaps = count_pcap_files();
        int evidence = pcaps * 100;
        if (saved_xp < evidence) saved_xp = evidence;
        
        /* Calculate level from saved XP */
        int level = 1;
        int xp_check = saved_xp;
        while (level < 9999) {
            int sq = 1;
            while ((sq+1)*(sq+1) <= level) sq++;
            int xp_needed = 10 * level * sq;
            if (xp_needed < 100) xp_needed = 100;
            if (xp_check < xp_needed) break;
            xp_check -= xp_needed;
            level++;
        }
        int sq = 1;
        while ((sq+1)*(sq+1) <= level) sq++;
        int xp_for_next = 10 * level * sq;
        if (xp_for_next < 100) xp_for_next = 100;
        g_ui_state.pwnhub_xp_percent = (xp_for_next > 0) ? (xp_check * 100 / xp_for_next) : 0;
        g_ui_state.pwnhub_level = level;
        g_ui_state.tcaps = pcaps;
        if (level >= 600) strncpy(g_ui_state.pwnhub_title, "Mythic", 23);
        else if (level >= 400) strncpy(g_ui_state.pwnhub_title, "Legendary", 23);
        else if (level >= 250) strncpy(g_ui_state.pwnhub_title, "Master", 23);
        else if (level >= 175) strncpy(g_ui_state.pwnhub_title, "Veteran", 23);
        else if (level >= 120) strncpy(g_ui_state.pwnhub_title, "Elite", 23);
        else if (level >= 80) strncpy(g_ui_state.pwnhub_title, "Predator", 23);
        else if (level >= 55) strncpy(g_ui_state.pwnhub_title, "Stalker", 23);
        else if (level >= 35) strncpy(g_ui_state.pwnhub_title, "Hunter", 23);
        else if (level >= 20) strncpy(g_ui_state.pwnhub_title, "Apprentice", 23);
        else if (level >= 10) strncpy(g_ui_state.pwnhub_title, "Rookie", 23);
        else if (level >= 5) strncpy(g_ui_state.pwnhub_title, "Newborn", 23);
        else strncpy(g_ui_state.pwnhub_title, "Hatchling", 23);
        fprintf(stderr, "[init] Loaded XP=%d pcaps=%d -> Level %d (%s)\n", saved_xp, pcaps, level, g_ui_state.pwnhub_title);
    }
    g_ui_state.pwnhub_wins = 0;
    g_ui_state.pwnhub_battles = 0;
    
    g_dirty = 1;
}

/*
 * Command handlers - Parse and execute IPC commands
 */
static int handle_command(const char *cmd, char *response, size_t resp_size) {
    char cmd_name[32];
    int n;
    
    PWNAUI_LOG_DEBUG("Received command: %s", cmd);
    
    /* Parse command name */
    n = sscanf(cmd, "%31s", cmd_name);
    if (n != 1) {
        snprintf(response, resp_size, "ERR Invalid command\n");
        return -1;
    }
    
    /* CLEAR - Clear display buffer */
    if (strcmp(cmd_name, "CLEAR") == 0) {
        renderer_clear(&g_ui_state, g_framebuffer);
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* UPDATE - Flush buffer to display using partial refresh (no blink) */
    if (strcmp(cmd_name, "UPDATE") == 0) {
        if (g_dirty) {
            uint64_t now = get_time_ms();
            /* Rate limit updates */
            if (now - g_last_update_ms >= UPDATE_INTERVAL_MS) {
                renderer_render_ui(&g_ui_state, g_framebuffer);
                trigger_display_update();  /* Non-blocking - signals display thread */
                g_last_update_ms = now;
                g_dirty = 0;
            }
        }
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* FULL_UPDATE - Force full e-ink refresh */
    if (strcmp(cmd_name, "FULL_UPDATE") == 0) {
        renderer_render_ui(&g_ui_state, g_framebuffer);
        display_update(g_framebuffer);  /* Full refresh */
        g_last_update_ms = get_time_ms();
        g_dirty = 0;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_FACE face_string */
    if (strcmp(cmd_name, "SET_FACE") == 0) {
        const char *face = cmd + 9;  /* Skip "SET_FACE " */
        while (*face == ' ') face++;
        /* Convert IPC face string to enum for legacy compatibility */
        g_ui_state.face_enum = theme_face_string_to_state(face);
        /* Remove trailing newline */
        /* face_enum is set, no string to strip */
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_STATUS text */
    if (strcmp(cmd_name, "SET_STATUS") == 0) {
        const char *status = cmd + 11;  /* Skip "SET_STATUS " */
        while (*status == ' ') status++;
        strncpy(g_ui_state.status, status, sizeof(g_ui_state.status) - 1);
        g_ui_state.status[sizeof(g_ui_state.status) - 1] = '\0';
        /* Remove trailing newline */
        char *nl = strchr(g_ui_state.status, '\n');
        if (nl) *nl = '\0';
        /* Replace literal \n with space */
        char *p = g_ui_state.status;
        while ((p = strstr(p, "\\n")) != NULL) {
            *p = ' ';
            memmove(p + 1, p + 2, strlen(p + 2) + 1);
        }
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_CHANNEL value */
    if (strcmp(cmd_name, "SET_CHANNEL") == 0) {
        const char *val = cmd + 12;
        while (*val == ' ') val++;
        int ch = atoi(val);
        if (ch >= 1 && ch <= 14) {
            snprintf(g_ui_state.channel, sizeof(g_ui_state.channel), "%02d", ch);
        }
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_APS value */
    if (strcmp(cmd_name, "SET_APS") == 0) {
        const char *val = cmd + 8;
        while (*val == ' ') val++;
        strncpy(g_ui_state.aps, val, sizeof(g_ui_state.aps) - 1);
        char *nl = strchr(g_ui_state.aps, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_UPTIME value */
    if (strcmp(cmd_name, "SET_UPTIME") == 0) {
        const char *val = cmd + 11;
        while (*val == ' ') val++;
        strncpy(g_ui_state.uptime, val, sizeof(g_ui_state.uptime) - 1);
        char *nl = strchr(g_ui_state.uptime, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_SHAKES value (legacy - kept for compatibility) */
    if (strcmp(cmd_name, "SET_SHAKES") == 0) {
        const char *val = cmd + 11;
        while (*val == ' ') val++;
        strncpy(g_ui_state.shakes, val, sizeof(g_ui_state.shakes) - 1);
        char *nl = strchr(g_ui_state.shakes, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_STATS pwds fhs phs tcaps - Bottom bar stats in one command */
    if (strcmp(cmd_name, "SET_STATS") == 0) {
        int pwds = 0, fhs = 0, phs = 0, tcaps = 0;
        if (sscanf(cmd + 10, "%d %d %d %d", &pwds, &fhs, &phs, &tcaps) >= 1) {
            g_ui_state.pwds = pwds;
            g_ui_state.fhs = fhs;
            g_ui_state.phs = phs;
            g_ui_state.tcaps = tcaps;
            g_dirty = 1;
            snprintf(response, resp_size, "OK\n");
        } else {
            snprintf(response, resp_size, "ERR Invalid SET_STATS format\n");
        }
        return 0;
    }
    
    /* SET_MODE mode */
    if (strcmp(cmd_name, "SET_MODE") == 0) {
        const char *val = cmd + 9;
        while (*val == ' ') val++;
        strncpy(g_ui_state.mode, val, sizeof(g_ui_state.mode) - 1);
        char *nl = strchr(g_ui_state.mode, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_NAME name */
    if (strcmp(cmd_name, "SET_NAME") == 0) {
        const char *val = cmd + 9;
        while (*val == ' ') val++;
        strncpy(g_ui_state.name, val, sizeof(g_ui_state.name) - 1);
        char *nl = strchr(g_ui_state.name, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_FRIEND name */
    if (strcmp(cmd_name, "SET_FRIEND") == 0) {
        const char *val = cmd + 11;
        while (*val == ' ') val++;
        strncpy(g_ui_state.friend_name, val, sizeof(g_ui_state.friend_name) - 1);
        char *nl = strchr(g_ui_state.friend_name, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_BLUETOOTH status - BT-Tether plugin status ('C' = connected, '-' = disconnected) */
    if (strcmp(cmd_name, "SET_BLUETOOTH") == 0) {
        const char *val = cmd + 14;
        while (*val == ' ') val++;
        strncpy(g_ui_state.bluetooth, val, sizeof(g_ui_state.bluetooth) - 1);
        char *nl = strchr(g_ui_state.bluetooth, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_GPS CNCstatus - GPS CNCplugin status ('C' = connected, '-' = disconnected, 'S' = saved) */
    if (strcmp(cmd_name, "SET_GPS") == 0) {
        const char *val = cmd + 8;
        while (*val == ' ') val++;
        strncpy(g_ui_state.gps, val, sizeof(g_ui_state.gps) - 1);
        char *nl = strchr(g_ui_state.gps, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_BATTERY status - Battery percentage (e.g. "85%" or "85%+" for charging) */
    if (strcmp(cmd_name, "SET_BATTERY") == 0) {
        const char *val = cmd + 12;
        while (*val == ' ') val++;
        strncpy(g_ui_state.battery, val, sizeof(g_ui_state.battery) - 1);
        char *nl = strchr(g_ui_state.battery, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_PWNHUB_ENABLED 0|1 - Enable/disable PwnHub stats display */
    if (strcmp(cmd_name, "SET_PWNHUB_ENABLED") == 0) {
        int enabled;
        if (sscanf(cmd, "SET_PWNHUB_ENABLED %d", &enabled) == 1) {
            g_ui_state.pwnhub_enabled = enabled ? 1 : 0;
            g_dirty = 1;
            snprintf(response, resp_size, "OK\n");
            PWNAUI_LOG_DEBUG("PwnHub display %s", enabled ? "enabled" : "disabled");
            return 0;
        }
        snprintf(response, resp_size, "ERR Invalid SET_PWNHUB_ENABLED param\n");
        return -1;
    }
    
    /* SET_PWNHUB_MACROS protein fat carbs - Set macro values (0-50 each) */
    if (strcmp(cmd_name, "SET_PWNHUB_MACROS") == 0) {
        int protein, fat, carbs;
        if (sscanf(cmd, "SET_PWNHUB_MACROS %d %d %d", &protein, &fat, &carbs) == 3) {
            g_ui_state.pwnhub_protein = (protein < 0) ? 0 : (protein > 50) ? 50 : protein;
            g_ui_state.pwnhub_fat = (fat < 0) ? 0 : (fat > 50) ? 50 : fat;
            g_ui_state.pwnhub_carbs = (carbs < 0) ? 0 : (carbs > 50) ? 50 : carbs;
            g_dirty = 1;
            snprintf(response, resp_size, "OK\n");
            return 0;
        }
        snprintf(response, resp_size, "ERR Invalid SET_PWNHUB_MACROS params (need: protein fat carbs)\n");
        return -1;
    }
    
    /* SET_PWNHUB_XP percent - Set XP progress (0-100) */
    if (strcmp(cmd_name, "SET_PWNHUB_XP") == 0) {
        int percent;
        if (sscanf(cmd, "SET_PWNHUB_XP %d", &percent) == 1) {
            g_ui_state.pwnhub_xp_percent = (percent < 0) ? 0 : (percent > 100) ? 100 : percent;
            g_dirty = 1;
            snprintf(response, resp_size, "OK\n");
            return 0;
        }
        snprintf(response, resp_size, "ERR Invalid SET_PWNHUB_XP param\n");
        return -1;
    }
    
    /* SET_PWNHUB_STAGE title level wins total - Set stage info */
    if (strcmp(cmd_name, "SET_PWNHUB_STAGE") == 0) {
        char title[24];
        int level, wins, total;
        if (sscanf(cmd, "SET_PWNHUB_STAGE %23s %d %d %d", title, &level, &wins, &total) == 4) {
            strncpy(g_ui_state.pwnhub_title, title, sizeof(g_ui_state.pwnhub_title) - 1);
            g_ui_state.pwnhub_title[sizeof(g_ui_state.pwnhub_title) - 1] = '\0';
            g_ui_state.pwnhub_level = level;
            g_ui_state.pwnhub_wins = wins;
            g_ui_state.pwnhub_battles = total;
            g_dirty = 1;
            snprintf(response, resp_size, "OK\n");
            return 0;
        }
        snprintf(response, resp_size, "ERR Invalid SET_PWNHUB_STAGE params (need: title level wins total)\n");
        return -1;
    }
    
    /* SET_MEMTEMP_HEADER header - Memtemp column headers (e.g. "mem cpu tmp") */
    if (strcmp(cmd_name, "SET_MEMTEMP_HEADER") == 0) {
        const char *val = cmd + 18;
        while (*val == ' ') val++;
        strncpy(g_ui_state.memtemp_header, val, sizeof(g_ui_state.memtemp_header) - 1);
        char *nl = strchr(g_ui_state.memtemp_header, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* SET_MEMTEMP_DATA data - Memtemp data values (e.g. " 42%  12%  48C") */
    if (strcmp(cmd_name, "SET_MEMTEMP_DATA") == 0) {
        const char *val = cmd + 16;
        while (*val == ' ') val++;
        strncpy(g_ui_state.memtemp_data, val, sizeof(g_ui_state.memtemp_data) - 1);
        char *nl = strchr(g_ui_state.memtemp_data, '\n');
        if (nl) *nl = '\0';
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* DRAW_TEXT x y font_id text */
    if (strcmp(cmd_name, "DRAW_TEXT") == 0) {
        int x, y, font_id;
        char text[256];
        if (sscanf(cmd, "DRAW_TEXT %d %d %d %255[^\n]", &x, &y, &font_id, text) == 4) {
            renderer_draw_text(&g_ui_state, g_framebuffer, x, y, text, font_id);
            g_dirty = 1;
            snprintf(response, resp_size, "OK\n");
            return 0;
        }
        snprintf(response, resp_size, "ERR Invalid DRAW_TEXT params\n");
        return -1;
    }
    
    /* DRAW_LINE x1 y1 x2 y2 */
    if (strcmp(cmd_name, "DRAW_LINE") == 0) {
        int x1, y1, x2, y2;
        if (sscanf(cmd, "DRAW_LINE %d %d %d %d", &x1, &y1, &x2, &y2) == 4) {
            renderer_draw_line(&g_ui_state, g_framebuffer, x1, y1, x2, y2);
            g_dirty = 1;
            snprintf(response, resp_size, "OK\n");
            return 0;
        }
        snprintf(response, resp_size, "ERR Invalid DRAW_LINE params\n");
        return -1;
    }
    
    /* DRAW_ICON name x y */
    if (strcmp(cmd_name, "DRAW_ICON") == 0) {
        char icon_name[32];
        int x, y;
        if (sscanf(cmd, "DRAW_ICON %31s %d %d", icon_name, &x, &y) == 3) {
            icons_draw(g_framebuffer, icon_name, x, y);
            g_dirty = 1;
            snprintf(response, resp_size, "OK\n");
            return 0;
        }
        snprintf(response, resp_size, "ERR Invalid DRAW_ICON params\n");
        return -1;
    }
    
    /* SET_INVERT 0|1 */
    if (strcmp(cmd_name, "SET_INVERT") == 0) {
        int invert;
        if (sscanf(cmd, "SET_INVERT %d", &invert) == 1) {
            g_ui_state.invert = invert ? 1 : 0;
            g_dirty = 1;
            snprintf(response, resp_size, "OK\n");
            return 0;
        }
        snprintf(response, resp_size, "ERR Invalid SET_INVERT param\n");
        return -1;
    }
    
    /* SET_LAYOUT layout_name */
    if (strcmp(cmd_name, "SET_LAYOUT") == 0) {
        const char *layout = cmd + 11;
        while (*layout == ' ') layout++;
        renderer_set_layout(layout);
        g_dirty = 1;
        snprintf(response, resp_size, "OK\n");
        return 0;
    }
    
    /* GET_STATE - Return current UI state (for debugging) */
    if (strcmp(cmd_name, "GET_STATE") == 0) {
        snprintf(response, resp_size, 
            "OK face=%s status=%s ch=%s aps=%s up=%s shakes=%s mode=%s name=%s bt=%s memtemp=%s pwds=%d fhs=%d phs=%d tcaps=%d\n",
            g_ui_state.face, g_ui_state.status, g_ui_state.channel,
            g_ui_state.aps, g_ui_state.uptime, g_ui_state.shakes,
            g_ui_state.mode, g_ui_state.name, g_ui_state.bluetooth,
            g_ui_state.memtemp_data, g_ui_state.pwds, g_ui_state.fhs, g_ui_state.phs, g_ui_state.tcaps);
        return 0;
    }
    
    /* PING - Connection test */
    if (strcmp(cmd_name, "PING") == 0) {
        snprintf(response, resp_size, "PONG\n");
        return 0;
    }
    
    /* SET_THEME theme_name - Switch to a different face theme */
    if (strcmp(cmd_name, "SET_THEME") == 0) {
        const char *theme_name = cmd + 10;  /* Skip "SET_THEME " */
        while (*theme_name == ' ') theme_name++;
        char name_buf[64];
        strncpy(name_buf, theme_name, sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';
        /* Remove trailing newline */
        char *nl = strchr(name_buf, '\n');
        if (nl) *nl = '\0';
        
        /* Set the PNG theme */
        if (theme_set_active(name_buf) == 0) {
            themes_set_enabled(1);  /* Always enable PNG themes */
            g_dirty = 1;
            snprintf(response, resp_size, "OK Theme set to %s\n", name_buf);
            PWNAUI_LOG_INFO("Theme switched to: %s", name_buf);
        } else {
            snprintf(response, resp_size, "ERR Theme not found: %s\n", name_buf);
        }
        return 0;
    }
    
    /* LIST_THEMES - Get list of available PNG themes */
    if (strcmp(cmd_name, "LIST_THEMES") == 0) {
        int count = themes_count();
        char *p = response;
        int remaining = (int)resp_size;
        /* PNG themes only */
        int n = snprintf(p, remaining, "OK %d themes:", count);
        p += n; remaining -= n;
        
        if (count > 0) {
            const char **names = themes_list();
            for (int i = 0; i < count && remaining > 0; i++) {
                n = snprintf(p, remaining, " %s", names[i]);
                p += n; remaining -= n;
            }
        }
        if (remaining > 0) {
            snprintf(p, remaining, "\n");
        }
        return 0;
    }
    
    /* GET_THEME - Get current active PNG theme name */
    if (strcmp(cmd_name, "GET_THEME") == 0) {
        const char *current = theme_get_active();
        if (current && current[0]) {
            snprintf(response, resp_size, "OK %s\n", current);
        } else {
            snprintf(response, resp_size, "OK pwnachu\n");  /* Default PNG theme */
        }
        return 0;
    }
    
    /* Unknown command */
    snprintf(response, resp_size, "ERR Unknown command: %s\n", cmd_name);
    return -1;
}

/*
 * Create PID file
 */
static int create_pidfile(void) {
    FILE *f = fopen(PID_FILE, "w");
    if (!f) {
        PWNAUI_LOG_ERR("Failed to create PID file: %s", strerror(errno));
        return -1;
    }
    fprintf(f, "%d\n", getpid());
    fclose(f);
    return 0;
}

/*
 * Remove PID file
 */
static void remove_pidfile(void) {
    unlink(PID_FILE);
}

/*
 * Daemonize the process
 */
static int daemonize(void) {
    pid_t pid;
    
    /* Fork off parent */
    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);  /* Parent exits */
    }
    
    /* Create new session */
    if (setsid() < 0) {
        return -1;
    }
    
    /* Fork again to prevent acquiring a controlling terminal */
    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }
    
    /* Set file permissions */
    umask(0);
    
    /* Change working directory */
    chdir("/");
    
    /* Close standard file descriptors */
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    
    /* Redirect to /dev/null */
    open("/dev/null", O_RDONLY);  /* stdin */
    open("/dev/null", O_WRONLY);  /* stdout */
    open("/dev/null", O_WRONLY);  /* stderr */
    
    return 0;
}

/*
 * Print usage
 */
/*
 * Convert display type string to enum
 */
static display_type_t parse_display_type(const char *name) {
    if (strcmp(name, "dummy") == 0) return DISPLAY_DUMMY;
    if (strcmp(name, "framebuffer") == 0) return DISPLAY_FRAMEBUFFER;
    if (strcmp(name, "waveshare2in13_v2") == 0) return DISPLAY_WAVESHARE_2IN13_V2;
    if (strcmp(name, "waveshare2in13_v3") == 0) return DISPLAY_WAVESHARE_2IN13_V3;
    if (strcmp(name, "waveshare2in13_v4") == 0) return DISPLAY_WAVESHARE_2IN13_V4;
    if (strcmp(name, "waveshare2in7") == 0) return DISPLAY_WAVESHARE_2IN7;
    if (strcmp(name, "waveshare1in54") == 0) return DISPLAY_WAVESHARE_1IN54;
    if (strcmp(name, "inky_phat") == 0) return DISPLAY_INKY_PHAT;
    return DISPLAY_WAVESHARE_2IN13_V2;  /* Default */
}

/*
 * Get display dimensions for a type
 */
static void get_display_dimensions(display_type_t type, int *width, int *height) {
    switch (type) {
        case DISPLAY_WAVESHARE_2IN13_V2:
        case DISPLAY_WAVESHARE_2IN13_V3:
        case DISPLAY_WAVESHARE_2IN13_V4:
            *width = 250; *height = 122; break;
        case DISPLAY_WAVESHARE_2IN7:
            *width = 264; *height = 176; break;
        case DISPLAY_WAVESHARE_1IN54:
            *width = 200; *height = 200; break;
        case DISPLAY_INKY_PHAT:
            *width = 212; *height = 104; break;
        default:
            *width = 250; *height = 122; break;
    }
}

/*
 * Display Thread - handles all blocking display operations
 * 
 * This thread runs independently of the main IPC loop, ensuring that
 * slow e-ink display updates (which can take 200-500ms) don't block
 * socket accept() calls and cause connection pileup.
 */
static void *display_thread_func(void *arg) {
    (void)arg;  /* Unused */
    
    PWNAUI_LOG_INFO("Display thread started");
    
    while (g_running) {
        pthread_mutex_lock(&g_ui_mutex);
        
        /* Wait for render signal or timeout (for periodic checks) */
        while (!g_display_pending && g_running) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;  /* 1 second timeout for checking g_running */
            
            int rc = pthread_cond_timedwait(&g_display_cond, &g_ui_mutex, &ts);
            if (rc == ETIMEDOUT) {
                /* Just a timeout, check g_running and loop */
                continue;
            }
        }
        
        if (!g_running) {
            pthread_mutex_unlock(&g_ui_mutex);
            scan_handshake_stats();  /* Rescan to pick up new pcap */
            break;
        }
        
        /* Copy framebuffer while holding mutex */
        memcpy(g_display_fb, g_framebuffer, sizeof(g_display_fb));
        g_display_pending = 0;
        
        pthread_mutex_unlock(&g_ui_mutex);
        
        /* Now do the slow display update WITHOUT holding the mutex */
        /* This is where epd_wait_busy() blocks, but main thread is free */
        display_partial_update(g_display_fb, 0, 0, 0, 0);
        PWNAUI_LOG_DEBUG("Display updated");
    }
    
    PWNAUI_LOG_INFO("Display thread exiting");
    return NULL;
}

/*
 * Signal the display thread to render
 * Called from main thread after updating g_framebuffer
 */
static void trigger_display_update(void) {
    pthread_mutex_lock(&g_ui_mutex);
    g_display_pending = 1;
    pthread_cond_signal(&g_display_cond);
    pthread_mutex_unlock(&g_ui_mutex);
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [OPTIONS]\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -d, --daemon     Run as daemon\n");
    fprintf(stderr, "  -v, --verbose    Verbose logging\n");
    fprintf(stderr, "  -p, --plugins    Enable native C plugins (memtemp, battery, bluetooth)\n");
    fprintf(stderr, "  -b, --bcap       Enable bettercap WebSocket (real-time AP/handshake events)\n");
    fprintf(stderr, "  -s, --socket PATH  Socket path (default: %s)\n", SOCKET_PATH);
    fprintf(stderr, "  -D, --display TYPE Display type (waveshare2in13, fb, dummy)\n");
    fprintf(stderr, "  -h, --help       Show this help\n");
}

/*
 * Main entry point
 */
/* PiSugar mode change callback - updates brain's manual_mode */
static void on_mode_change_cb(pwnagotchi_mode_t new_mode, void *user_data) {
    (void)user_data;
    
    if (g_brain_ctx) {
        g_brain_ctx->manual_mode = (new_mode == MODE_MANUAL);
        g_brain_ctx->manual_mode_toggled = time(NULL);
    }
}


int main(int argc, char *argv[]) {
    int opt;
    const char *socket_path = SOCKET_PATH;
    const char *display_type = "waveshare2in13_v4";  /* User display: Waveshare 2.13" V4 */
    int server_fd = -1;
    int client_fds[MAX_CLIENTS];
    int num_clients = 0;
    
    /* Initialize client array */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_fds[i] = -1;
    }
    
    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--daemon") == 0) {
            g_daemon_mode = 1;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--plugins") == 0) {
            g_native_plugins = 1;
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--bcap") == 0) {
            g_bcap_enabled = 1;
        } else if (strcmp(argv[i], "-B") == 0 || strcmp(argv[i], "--brain") == 0) {
            g_brain_enabled = 1;
            g_bcap_enabled = 1;  /* Brain requires bcap */
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--socket") == 0) {
            if (i + 1 < argc) {
                socket_path = argv[++i];
            }
        } else if (strcmp(argv[i], "-D") == 0 || strcmp(argv[i], "--display") == 0) {
            if (i + 1 < argc) {
                display_type = argv[++i];
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return EXIT_SUCCESS;
        }
    }
    
    /* Setup logging */
    if (g_daemon_mode) {
        openlog("pwnaui", LOG_PID | LOG_NDELAY, LOG_DAEMON);
    }
    
    PWNAUI_LOG_INFO("PwnaUI starting...");
    
    /* Daemonize if requested */
    if (g_daemon_mode) {
        if (daemonize() < 0) {
            PWNAUI_LOG_ERR("Failed to daemonize: %s", strerror(errno));
            return EXIT_FAILURE;
        }
    }
    
    /* Create PID file */
    if (create_pidfile() < 0) {
        return EXIT_FAILURE;
    }
    
    /* Setup signal handlers */
    setup_signals();
    
    /* Initialize display */
    display_type_t dtype = parse_display_type(display_type);
    int disp_width, disp_height;
    get_display_dimensions(dtype, &disp_width, &disp_height);
    PWNAUI_LOG_INFO("Initializing display: %s (%dx%d)", display_type, disp_width, disp_height);
    if (display_init(dtype, disp_width, disp_height) < 0) {
        PWNAUI_LOG_ERR("Failed to initialize display");
        remove_pidfile();
        return EXIT_FAILURE;
    }
    
    /* Initialize font system */
    if (font_init() < 0) {
        PWNAUI_LOG_ERR("Failed to initialize fonts");
        display_cleanup();
        remove_pidfile();
        return EXIT_FAILURE;
    }
    
    /* Initialize icons */
    if (icons_init() < 0) {
        PWNAUI_LOG_ERR("Failed to initialize icons");
        font_cleanup();
        display_cleanup();
        remove_pidfile();
        return EXIT_FAILURE;
    }
    
    /* Initialize renderer */
    if (renderer_init() < 0) {
        PWNAUI_LOG_ERR("Failed to initialize renderer");
        icons_cleanup();
        font_cleanup();
        display_cleanup();
        remove_pidfile();
        return EXIT_FAILURE;
    }
    
    /* Set renderer layout based on display type */
    renderer_set_layout(display_type);
    PWNAUI_LOG_INFO("Set layout: %s", display_type);
    
    /* Initialize native plugins if enabled */
    if (g_native_plugins) {
        PWNAUI_LOG_INFO("Initializing native C plugins (memtemp, battery, bluetooth)");
        if (plugins_init(&g_plugins) < 0) {
            PWNAUI_LOG_ERR("Failed to initialize native plugins");
            /* Non-fatal - continue without native plugins */
            g_native_plugins = 0;
        } else {
            PWNAUI_LOG_INFO("Native plugins initialized successfully");
        }
    }
    
    /* Initialize theme system */
    PWNAUI_LOG_INFO("Initializing theme system");
    if (themes_init(NULL) < 0) {
        PWNAUI_LOG_WARN("Theme system not available (non-fatal)");
    } else {
        PWNAUI_LOG_INFO("Theme system ready, %d themes available", themes_count());
        
        /* Auto-load theme from pwnagotchi config - default to 'default' theme */
        char loaded_theme[64] = {0};
        FILE *cfg = fopen("/etc/pwnagotchi/config.toml", "r");
        if (cfg) {
            char line[512];
            int in_ui_faces = 0;
            while (fgets(line, sizeof(line), cfg)) {
                /* Track when we're in [ui.faces] section */
                if (strstr(line, "[ui.faces]")) {
                    in_ui_faces = 1;
                    continue;
                }
                /* Exit section when we hit another section */
                if (in_ui_faces && line[0] == '[') {
                    in_ui_faces = 0;
                }
                
                /* Look for theme = "themename" when in [ui.faces] section */
                if ((in_ui_faces && strstr(line, "theme")) || strstr(line, "ui.faces.theme")) {
                    char *theme_key = strstr(line, "theme");
                    if (theme_key) {
                        char *p = theme_key + 5;
                        while (*p == ' ' || *p == '\t') p++;
                        if (*p != '=') continue;
                    }
                    
                    char *quote1 = strchr(line, '"');
                    if (quote1) {
                        char *quote2 = strchr(quote1 + 1, '"');
                        if (quote2) {
                            *quote2 = '\0';
                            strncpy(loaded_theme, quote1 + 1, sizeof(loaded_theme) - 1);
                            break;
                        }
                    }
                }
            }
            fclose(cfg);
        }
        
        /* Default to "default" theme if no theme set */
        if (loaded_theme[0] == '\0') {
            strncpy(loaded_theme, "default", sizeof(loaded_theme) - 1);
        }
        
        /* Load the PNG theme */
        PWNAUI_LOG_INFO("Loading PNG theme: %s", loaded_theme);
        theme_t *theme = theme_load(loaded_theme);
        if (theme != NULL) {
            theme_set_active(loaded_theme);
            themes_set_enabled(1);
            PWNAUI_LOG_INFO("Theme '%s' loaded and activated (PNG mode)", loaded_theme);
        } else {
            /* Try default as fallback */
            PWNAUI_LOG_WARN("Failed to load theme '%s', trying default", loaded_theme);
            theme = theme_load("default");
            if (theme != NULL) {
                theme_set_active("default");
                themes_set_enabled(1);
                PWNAUI_LOG_INFO("Fallback theme 'default' loaded (PNG mode)");
            } else {
                PWNAUI_LOG_ERR("No PNG themes available!");
            }
        }
    }
    
    /* Initialize UI state */
    init_ui_state();
    g_start_time = time(NULL);  /* Initialize uptime counter */
    /* TCAPS computed live from pcap count */
    scan_handshake_stats();  /* Load initial stats from disk */
    
    /* Initialize bettercap WebSocket client if enabled */
    if (g_bcap_enabled) {
        PWNAUI_LOG_INFO("Initializing bettercap WebSocket client");
        bcap_config_t bcap_config;
        bcap_config_init(&bcap_config);
        
        /* Set callbacks for real-time events */
        bcap_config.on_event = bcap_on_event;
        bcap_config.on_state_change = bcap_on_state_change;
        bcap_config.auto_reconnect = true;
        bcap_config.max_reconnect_attempts = 0;  /* Infinite retries */
        
        g_bcap_ctx = bcap_create(&bcap_config);
        if (g_bcap_ctx) {
            if (bcap_connect_async(g_bcap_ctx) == 0) {
                bcap_subscribe(g_bcap_ctx, "wifi.*");
                PWNAUI_LOG_INFO("Bettercap WebSocket connected, subscribed to wifi events");
            } else {
                PWNAUI_LOG_WARN("Bettercap WebSocket connect failed (will retry in background)");
            }
        } else {
            PWNAUI_LOG_ERR("Failed to create bettercap WebSocket context");
            g_bcap_enabled = 0;
        }
    }
    
    /* Initialize Thompson Sampling brain if enabled */
    if (g_brain_enabled && g_bcap_ctx) {
        PWNAUI_LOG_INFO("Initializing Thompson Sampling brain");
        
        /* Use default config */
        brain_config_t brain_config = brain_config_default();
        
        health_monitor_init(&g_health, true);
            g_brain_ctx = brain_create(&brain_config, g_bcap_ctx);
        if (g_brain_ctx) {
            /* Register UI update callbacks */
            brain_set_callbacks(g_brain_ctx,
                brain_mood_callback,     /* on_mood_change */
                NULL,                    /* on_deauth */
                NULL,                    /* on_associate */
                NULL,                    /* on_handshake (handled by bcap_on_event with dedup) */
                brain_epoch_callback,    /* on_epoch */
                brain_channel_callback,  /* on_channel_change */
                NULL);                   /* user_data */
            g_brain_ctx->on_attack_phase = brain_attack_phase_callback;

            /* Default boot mode is MANUAL - tell brain to pause attacks */
            g_brain_ctx->manual_mode = true;
            g_brain_ctx->manual_mode_toggled = time(NULL);
            PWNAUI_LOG_INFO("Brain: manual_mode=true (boot default)");
            
            if (brain_start(g_brain_ctx) == 0) {
                PWNAUI_LOG_INFO("Thompson Sampling brain started - replacing Python pwnagotchi!");

                /* Sprint 4 #9: Give brain access to GPS data for mobility detection */
                if (g_native_plugins && g_plugins.gps_enabled) {
                    g_brain_ctx->gps = &g_plugins.gps;
                    PWNAUI_LOG_INFO("Brain: GPS data linked for mobility detection");
                } else {
                    g_brain_ctx->gps = NULL;
                    PWNAUI_LOG_INFO("Brain: No GPS, using AP-churn for mobility");
                }
    
        /* Start web server on port 80 */
    /* Initialize PiSugar button handler */
    g_pisugar = pisugar_init();
    if (g_pisugar) {
        PWNAUI_LOG_INFO("PiSugar3 initialized - custom btn: tap=mode, 2x=reserved, hold=reserved");
        pisugar_set_callback(g_pisugar, on_mode_change_cb, NULL);
    } else {
        PWNAUI_LOG_INFO("PiSugar not detected (optional)");
    }
        webserver_set_state_callback(webserver_state_cb);
        webserver_set_gps_callback(webserver_gps_cb);
        attack_log_init();  /* Sprint 5: JSON attack log */
        g_webserver_fd = webserver_init(80);
        if (g_webserver_fd >= 0) {
            PWNAUI_LOG_INFO("Web server started on port 80");
        } else {
            PWNAUI_LOG_WARN("Failed to start web server on port 80");
        }
            } else {
                PWNAUI_LOG_ERR("Failed to start brain thread");
                brain_destroy(g_brain_ctx);
                g_brain_ctx = NULL;
            }
        } else {
            PWNAUI_LOG_ERR("Failed to create brain context");
            g_brain_enabled = 0;
        }
    } else if (g_brain_enabled) {
        PWNAUI_LOG_WARN("Brain requires bettercap - disabling brain");
        g_brain_enabled = 0;
    }

    /* Create IPC server */
    PWNAUI_LOG_INFO("Creating IPC server at %s", socket_path);
    server_fd = ipc_server_create(socket_path);
    if (server_fd < 0) {
        PWNAUI_LOG_ERR("Failed to create IPC server");
        renderer_cleanup();
        icons_cleanup();
        font_cleanup();
        display_cleanup();
        remove_pidfile();
        return EXIT_FAILURE;
    }
    
    /* Initial render */
    renderer_render_ui(&g_ui_state, g_framebuffer);
    display_update(g_framebuffer);  /* Full update on startup */
    g_dirty = 0;
    g_last_update_ms = get_time_ms();
    
    /* Start display thread - handles all blocking display I/O */
    PWNAUI_LOG_INFO("Starting display thread");
    if (pthread_create(&g_display_thread, NULL, display_thread_func, NULL) != 0) {
        PWNAUI_LOG_ERR("Failed to create display thread");
        ipc_server_destroy(server_fd, socket_path);
        renderer_cleanup();
        icons_cleanup();
        font_cleanup();
        display_cleanup();
        remove_pidfile();
        return EXIT_FAILURE;
    }
    
    PWNAUI_LOG_INFO("PwnaUI ready, entering main loop");
    
    /* Main event loop */
    while (g_running) {
        fd_set read_fds;
        struct timeval timeout;
        int max_fd = server_fd;
        int activity;

        /* Loop rate measurement */
        {
            static uint64_t s_loop_count = 0;
            static uint64_t s_loop_report_ms = 0;
            s_loop_count++;
            uint64_t lnow = get_time_ms();
            if (lnow - s_loop_report_ms >= 5000) {
                if (s_loop_report_ms) {  /* skip first report */
                    PWNAUI_LOG_INFO("[loop] %llu iters in 5s (%.1fms avg)",
                                   (unsigned long long)s_loop_count,
                                   5000.0 / s_loop_count);
                }
                s_loop_count = 0;
                s_loop_report_ms = lnow;
            }
        }

        /* Handle config reload signal */
        if (g_reload_config) {
            PWNAUI_LOG_INFO("Reloading configuration");
            /* TODO: Implement config reload */
            g_reload_config = 0;
        }
        
        /* Setup select */
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_fds[i] >= 0) {
                FD_SET(client_fds[i], &read_fds);
                if (client_fds[i] > max_fd) {
                    max_fd = client_fds[i];
                }
            }
        }
        
        /* Add GPS UDP socket to select if enabled */
        int gps_fd = -1;
        if (g_native_plugins && g_plugins.gps_enabled) {
            gps_fd = plugin_gps_get_fd(&g_plugins.gps);
            if (gps_fd >= 0) {
                FD_SET(gps_fd, &read_fds);
                if (gps_fd > max_fd) {
                    max_fd = gps_fd;
                }
            }
        }
        
        /* Add webserver to select if running */
        if (g_webserver_fd >= 0) {
            FD_SET(g_webserver_fd, &read_fds);
            if (g_webserver_fd > max_fd) {
                max_fd = g_webserver_fd;
            }
        }
        
        /* Poll PiSugar3 custom button (reg 0x08 bit 0) - runs every loop (~10ms) */
        if (g_pisugar) {
            pisugar_tap_t tap = pisugar_poll_tap(g_pisugar);
            switch (tap) {
            case TAP_SINGLE:
                pisugar_toggle_mode(g_pisugar);
                if (pisugar_get_mode(g_pisugar) == MODE_MANUAL) {
                    strncpy(g_ui_state.mode, "MANU", sizeof(g_ui_state.mode) - 1);
                    PWNAUI_LOG_INFO("MODE -> MANUAL");
                } else {
                    strncpy(g_ui_state.mode, "AUTO", sizeof(g_ui_state.mode) - 1);
                    PWNAUI_LOG_INFO("MODE -> AUTO");
                }
                g_dirty = 1;
                break;
            case TAP_DOUBLE:
                PWNAUI_LOG_INFO("DOUBLE TAP - reserved");
                break;
            case TAP_LONG:
                PWNAUI_LOG_INFO("LONG PRESS - reserved");
                break;
            case TAP_NONE:
            default:
                break;
            }
        }

        /* Update uptime every second */
        uint64_t _sect_before_uptime = get_time_ms();
        {
            time_t now_t = time(NULL);
            if (now_t > g_last_uptime_update) {
                g_last_uptime_update = now_t;
                update_uptime_display();
            /* Rescan handshake stats every 60 seconds */
            if (now_t - g_last_stats_scan >= 60) {
                g_last_stats_scan = now_t;
                scan_handshake_stats();
            }
            }
        }
        
        /* Update animation frames - 2Hz max for e-ink */
        {
            static face_state_t s_last_anim_frame = FACE_HAPPY;
            static uint64_t s_last_frame_change_ms = 0;
            static int s_anim_log_count = 0;
            uint32_t now_ms = (uint32_t)(get_time_ms() & 0xFFFFFFFF);
            if (animation_is_active()) {
                /* WATCHDOG: if UPLOAD animation has been running way past the
                 * attack phase hold timer (45s total = 20s hold + 25s grace),
                 * the brain thread is likely stuck. Stop the animation and
                 * revert to the current mood face so the display isn't frozen. */
                if (g_attack_phase_hold_until > 0 &&
                    time(NULL) > g_attack_phase_hold_until + 25) {
                    face_state_t wdog_frame = animation_get_frame();
                    if (wdog_frame >= FACE_UPLOAD_00 && wdog_frame <= FACE_UPLOAD_11) {
                        animation_stop();
                        if (g_brain_ctx) {
                            brain_mood_t wdog_mood = brain_get_mood(g_brain_ctx);
                            face_state_t wdog_face = get_face_state_for_mood(wdog_mood);
                            const char *wdog_voice = brain_get_voice(wdog_mood);
                            pthread_mutex_lock(&g_ui_mutex);
                            g_ui_state.face_enum = wdog_face;
                            strncpy(g_ui_state.face, g_face_state_names[wdog_face],
                                    sizeof(g_ui_state.face) - 1);
                            strncpy(g_ui_state.status, wdog_voice,
                                    sizeof(g_ui_state.status) - 1);
                            g_dirty = 1;
                            pthread_mutex_unlock(&g_ui_mutex);
                        }
                        g_attack_phase_hold_until = 0;
                        fprintf(stderr, "[anim] WATCHDOG: UPLOAD stuck >45s, reverting to mood\n");
                        /* animation stopped — skip the rest of this block */
                        goto anim_done;
                    }
                }

                /* DOWNLOAD auto-stop: revert to mood face after DOWNLOAD_DISPLAY_SECS */
                if (g_download_start_time > 0 &&
                    time(NULL) > g_download_start_time + DOWNLOAD_DISPLAY_SECS) {
                    face_state_t dl_frame = animation_get_frame();
                    /* Check if current animation is DOWNLOAD range (same face enums as UPLOAD) */
                    if (dl_frame >= FACE_UPLOAD_00 && dl_frame <= FACE_UPLOAD_11) {
                        animation_stop();
                        g_download_start_time = 0;
                        if (g_brain_ctx) {
                            brain_mood_t dl_mood = brain_get_mood(g_brain_ctx);
                            face_state_t dl_face = get_face_state_for_mood(dl_mood);
                            const char *dl_voice = brain_get_voice(dl_mood);
                            pthread_mutex_lock(&g_ui_mutex);
                            g_ui_state.face_enum = dl_face;
                            strncpy(g_ui_state.face, g_face_state_names[dl_face],
                                    sizeof(g_ui_state.face) - 1);
                            strncpy(g_ui_state.status, dl_voice,
                                    sizeof(g_ui_state.status) - 1);
                            g_dirty = 1;
                            pthread_mutex_unlock(&g_ui_mutex);
                        }
                        fprintf(stderr, "[anim] DOWNLOAD auto-stop after %ds\n", DOWNLOAD_DISPLAY_SECS);
                        goto anim_done;
                    }
                }

                /* Log animation state every ~5 seconds (500 iterations at 10ms) */
                if (++s_anim_log_count >= 500) {
                    s_anim_log_count = 0;
                    face_state_t cur = animation_get_frame();
                    PWNAUI_LOG_INFO("[anim] active: cur_frame=%d last_frame=%d interval=%dms",
                                   (int)cur, (int)s_last_anim_frame,
                                   g_anim_state.interval_ms);
                }
                animation_tick(now_ms);
                face_state_t new_frame = animation_get_frame();
                if (new_frame != s_last_anim_frame) {
                    uint64_t now64 = get_time_ms();
                    uint64_t delta = s_last_frame_change_ms ? (now64 - s_last_frame_change_ms) : 0;
                    s_last_frame_change_ms = now64;
                    PWNAUI_LOG_INFO("[anim] FRAME CHANGE: %d -> %d delta=%lums",
                                   (int)s_last_anim_frame, (int)new_frame, (unsigned long)delta);
                    s_last_anim_frame = new_frame;
                    pthread_mutex_lock(&g_ui_mutex);
                    /* Only update face from animation if it's an attack animation
                     * (UPLOAD/DOWNLOAD) or if hold timer has expired.
                     * This prevents mood animations from overwriting FACE_SMART
                     * during LISTEN phase. */
                    if (new_frame == FACE_UPLOAD_00 || new_frame == FACE_UPLOAD_01 ||
                        new_frame == FACE_UPLOAD_10 || new_frame == FACE_UPLOAD_11 ||
                        time(NULL) >= g_attack_phase_hold_until) {
                        g_ui_state.face_enum = new_frame;
                        strncpy(g_ui_state.face, g_face_state_names[new_frame], sizeof(g_ui_state.face) - 1);
                        g_dirty = 1;
                    }
                    pthread_mutex_unlock(&g_ui_mutex);
                }
            }
            anim_done: ;
        }


        /* Timeout for periodic tasks - keep short to drain accept queue quickly */
        timeout.tv_sec = 0;
        timeout.tv_usec = 10000;  /* 10ms - fast response to prevent connection pileup */
        
        uint64_t _sect_before_select = get_time_ms();
        activity = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        /* Section timing to find main loop blocker */
        uint64_t _sect_after_select = get_time_ms();
        
        if (activity < 0) {
            if (errno == EINTR) {
                continue;  /* Signal interrupted, check g_running */
            }
            PWNAUI_LOG_ERR("select() error: %s", strerror(errno));
            break;
        }
        
        /* Check for new connections - drain ALL pending accepts */
        if (FD_ISSET(server_fd, &read_fds)) {
            /* Accept in a loop until EAGAIN (no more pending) or max clients */
            while (num_clients < MAX_CLIENTS) {
                int client_fd = ipc_server_accept(server_fd);
                if (client_fd < 0) {
                    break;  /* No more pending connections (EAGAIN) */
                }
                
                /* Find empty slot */
                int added = 0;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (client_fds[i] < 0) {
                        client_fds[i] = client_fd;
                        num_clients++;
                        PWNAUI_LOG_DEBUG("Client connected (slot %d, fd %d)", i, client_fd);
                        added = 1;
                        break;
                    }
                }
                if (!added) {
                    PWNAUI_LOG_WARN("Max clients reached, rejecting connection");
                    close(client_fd);
                    break;
                }
            }
        }
        
        /* Handle client data */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_fds[i] >= 0 && FD_ISSET(client_fds[i], &read_fds)) {
                char buffer[BUFFER_SIZE];
                char response[BUFFER_SIZE];
                ssize_t n;
                
                n = read(client_fds[i], buffer, sizeof(buffer) - 1);
                if (n < 0) {
                    /* Check if it's just EAGAIN (no data yet) vs real error */
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        continue;  /* No data available, not a disconnect */
                    }
                    /* Real error - disconnect client */
                    PWNAUI_LOG_DEBUG("Client error (slot %d): %s", i, strerror(errno));
                    close(client_fds[i]);
                    client_fds[i] = -1;
                    num_clients--;
                } else if (n == 0) {
                    /* Client closed connection */
                    PWNAUI_LOG_DEBUG("Client disconnected (slot %d)", i);
                    close(client_fds[i]);
                    client_fds[i] = -1;
                    num_clients--;
                } else {
                    buffer[n] = '\0';
                    
                    /* Handle command */
                    handle_command(buffer, response, sizeof(response));
                    
                    /* Send response and close - one-shot IPC model */
                    write(client_fds[i], response, strlen(response));
                    close(client_fds[i]);
                    client_fds[i] = -1;
                    num_clients--;
                }
            }
        }
        
        /* Handle webserver HTTP requests */
        if (g_webserver_fd >= 0 && FD_ISSET(g_webserver_fd, &read_fds)) {
            webserver_poll(g_webserver_fd);
        }

        /* Handle GPS UDP data if available */
        if (gps_fd >= 0 && FD_ISSET(gps_fd, &read_fds)) {
            if (plugin_gps_handle_data(&g_plugins.gps)) {
                /* GPS data received - update UI state */
                strncpy(g_ui_state.gps, plugin_gps_get_display(&g_plugins.gps),
                        sizeof(g_ui_state.gps) - 1);
                g_dirty = 1;
            }
        }
        
        /* Update native C plugins (if enabled) */
        uint64_t _sect_before_plugins = get_time_ms();
        if (g_native_plugins) {
            int updated = plugins_update(&g_plugins);
            if (updated) {
                /* Copy plugin data to UI state */
                if (updated & 0x01) {  /* MEMTEMP */
                    strncpy(g_ui_state.memtemp_header, g_plugins.memtemp.header,
                            sizeof(g_ui_state.memtemp_header) - 1);
                    strncpy(g_ui_state.memtemp_data, g_plugins.memtemp.data,
                            sizeof(g_ui_state.memtemp_data) - 1);
                    g_dirty = 1;
                }
                if (updated & 0x02) {  /* BATTERY */
                    /* Format battery status: percentage + charging indicator */
                    if (g_plugins.battery.available) {
                        snprintf(g_ui_state.battery, sizeof(g_ui_state.battery), "BAT%d%%%s", 
                                 g_plugins.battery.percentage,
                                 g_plugins.battery.charging ? "+" : "");
                    } else {
                        snprintf(g_ui_state.battery, sizeof(g_ui_state.battery), "");
                    }
                    PWNAUI_LOG_INFO("Battery: %s", g_ui_state.battery);
                    g_dirty = 1;
                }
                if (updated & 0x04) {  /* BLUETOOTH */
                    strncpy(g_ui_state.bluetooth, g_plugins.bluetooth.status,
                            sizeof(g_ui_state.bluetooth) - 1);
                    g_dirty = 1;
                }
                if (updated & 0x08) {  /* GPS timeout check */
                    strncpy(g_ui_state.gps, plugin_gps_get_display(&g_plugins.gps),
                            sizeof(g_ui_state.gps) - 1);
                    g_dirty = 1;
                }
            }
        }
        
        /* Update health monitor periodically */
        uint64_t _sect_before_health = get_time_ms();
        health_monitor_update(&g_health);

        /* Auto-render when dirty (rate limited) */
        uint64_t _sect_before_render = get_time_ms();
        if (g_dirty) {
            uint64_t now = get_time_ms();
            if (now - g_last_update_ms >= UPDATE_INTERVAL_MS) {
                renderer_render_ui(&g_ui_state, g_framebuffer);
                trigger_display_update();  /* Non-blocking - signals display thread */
                g_last_update_ms = now;
                g_dirty = 0;
            }
        }

        /* Report slow sections */
        {
            uint64_t _sect_end = get_time_ms();
            uint64_t uptime_anim = _sect_before_select - _sect_before_uptime;
            uint64_t sel_time = _sect_after_select - _sect_before_select;
            uint64_t ipc = _sect_before_plugins - _sect_after_select;
            uint64_t plg = _sect_before_health - _sect_before_plugins;
            uint64_t hlth = _sect_before_render - _sect_before_health;
            uint64_t rnd = _sect_end - _sect_before_render;
            uint64_t total = _sect_end - _sect_before_uptime;
            if (total > 100) {
                PWNAUI_LOG_INFO("[perf] SLOW %lums: pre=%lu sel=%lu ipc=%lu plg=%lu hlth=%lu rnd=%lu",
                               (unsigned long)total, (unsigned long)uptime_anim,
                               (unsigned long)sel_time, (unsigned long)ipc,
                               (unsigned long)plg, (unsigned long)hlth, (unsigned long)rnd);
            }
        }
    }
    
    PWNAUI_LOG_INFO("PwnaUI shutting down...");
    
    /* Signal display thread to exit and wait for it */
    PWNAUI_LOG_INFO("Stopping display thread...");
    pthread_mutex_lock(&g_ui_mutex);
    g_display_pending = 1;  /* Wake up thread if waiting */
    pthread_cond_signal(&g_display_cond);
    pthread_mutex_unlock(&g_ui_mutex);
    pthread_join(g_display_thread, NULL);
    PWNAUI_LOG_INFO("Display thread stopped");
    
    /* Cleanup */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_fds[i] >= 0) {
            close(client_fds[i]);
        }
    }
    
    ipc_server_destroy(server_fd, socket_path);
    
    /* Cleanup native plugins */
    if (g_native_plugins) {
        plugins_cleanup(&g_plugins);
    }
    
    /* Cleanup Thompson Sampling brain */
    if (g_brain_enabled && g_brain_ctx) {
        PWNAUI_LOG_INFO("Stopping Thompson Sampling brain...");
        brain_stop(g_brain_ctx);
        brain_destroy(g_brain_ctx);
        g_brain_ctx = NULL;
    }

    /* Cleanup bettercap WebSocket client */
    if (g_bcap_enabled && g_bcap_ctx) {
        PWNAUI_LOG_INFO("Disconnecting bettercap WebSocket...");
        bcap_destroy(g_bcap_ctx);
        g_bcap_ctx = NULL;
    }
    
    /* Cleanup theme system */
    themes_cleanup();
    
    renderer_cleanup();
    icons_cleanup();
    font_cleanup();
    display_clear(0);  /* Clear to white */
    if (g_pisugar) pisugar_destroy(g_pisugar);
    display_cleanup();
    remove_pidfile();
    
    if (g_daemon_mode) {
        closelog();
    }
    
    return EXIT_SUCCESS;
}
