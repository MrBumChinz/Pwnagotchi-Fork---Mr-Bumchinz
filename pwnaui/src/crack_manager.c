/*
 * crack_manager.c — Idle handshake cracking for PwnaUI
 *
 * Runs aircrack-ng at nice -19 when the brain is idle.
 * On a Pi Zero W (~20 keys/sec WPA2), a 5500-word list takes ~5 minutes.
 * Designed for quick-win dictionary attacks, not brute force.
 *
 * Process lifecycle:
 *   1. crack_mgr_start() — fork+exec aircrack-ng with nice -19
 *   2. crack_mgr_check() — waitpid(WNOHANG), check key file
 *   3. crack_mgr_stop()  — SIGTERM → SIGKILL when brain needs CPU
 *
 * State is persisted to /home/pi/cracked/state.txt so we don't retry
 * completed combos after restart.  Cracked passwords are saved to
 * /home/pi/cracked/<SSID>.key and logged to /home/pi/cracked/log.txt.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include "crack_manager.h"

/* ========================================================================== */
/*  Helpers                                                                    */
/* ========================================================================== */

static void ensure_output_dir(void) {
    mkdir(CRACK_OUTPUT_DIR, 0755);
}

/*
 * Parse BSSID and SSID from bettercap pcap filename.
 * Format: "SSID_aabbccddeeff.pcap"  (12 hex chars, no separators)
 * Output: bssid_out = "aa:bb:cc:dd:ee:ff", ssid_out = "SSID"
 */
static bool parse_bssid_from_filename(const char *filename,
                                       char *bssid_out, char *ssid_out) {
    const char *ext = strstr(filename, ".pcap");
    if (!ext) return false;
    /* Reject .pcapng */
    if (ext[5] == 'n') return false;

    /* Find the LAST underscore before .pcap */
    const char *underscore = NULL;
    for (const char *p = filename; p < ext; p++) {
        if (*p == '_') underscore = p;
    }
    if (!underscore) return false;

    /* Must be exactly 12 hex chars between underscore and .pcap */
    int hex_len = (int)(ext - underscore - 1);
    if (hex_len != 12) return false;

    const char *hex = underscore + 1;
    for (int i = 0; i < 12; i++) {
        char c = hex[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return false;
    }

    /* Build colon-separated BSSID */
    snprintf(bssid_out, 18, "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c",
             hex[0],  hex[1],  hex[2],  hex[3],
             hex[4],  hex[5],  hex[6],  hex[7],
             hex[8],  hex[9],  hex[10], hex[11]);

    /* SSID = everything before the last underscore */
    int ssid_len = (int)(underscore - filename);
    if (ssid_len > 63) ssid_len = 63;
    memcpy(ssid_out, filename, ssid_len);
    ssid_out[ssid_len] = '\0';

    return true;
}

/*
 * Scan the wordlists directory.
 * Order: common.txt → combined_wifi.txt → rockyou.txt → anything else.
 * Smallest files tried first for quick wins.
 */
static void scan_wordlists(crack_mgr_t *mgr) {
    mgr->num_wordlists = 0;

    /* Priority order (small → large) */
    const char *priority[] = {
        "learned.txt",
        "common.txt",
        "combined_wifi.txt",
        "rockyou.txt",
        NULL
    };

    /* Add known wordlists in priority order */
    for (int i = 0; priority[i] && mgr->num_wordlists < CRACK_MAX_WORDLISTS; i++) {
        char path[CRACK_MAX_PATH];
        snprintf(path, sizeof(path), "%s/%s", CRACK_WORDLIST_DIR, priority[i]);

        struct stat st;
        if (stat(path, &st) == 0 && st.st_size > 0) {
            strncpy(mgr->wordlists[mgr->num_wordlists], path,
                    CRACK_MAX_PATH - 1);
            mgr->num_wordlists++;
        }
    }

    /* Then scan for any extra .txt files we didn't list */
    DIR *dir = opendir(CRACK_WORDLIST_DIR);
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL &&
           mgr->num_wordlists < CRACK_MAX_WORDLISTS) {
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (len < 5 || strcmp(name + len - 4, ".txt") != 0) continue;

        char path[CRACK_MAX_PATH];
        snprintf(path, sizeof(path), "%s/%s", CRACK_WORDLIST_DIR, name);

        /* Skip if already added */
        bool dup = false;
        for (int i = 0; i < mgr->num_wordlists; i++) {
            if (strcmp(mgr->wordlists[i], path) == 0) { dup = true; break; }
        }
        if (dup) continue;

        struct stat st;
        if (stat(path, &st) == 0 && st.st_size > 0) {
            strncpy(mgr->wordlists[mgr->num_wordlists], path,
                    CRACK_MAX_PATH - 1);
            mgr->num_wordlists++;
        }
    }
    closedir(dir);
}

/* ========================================================================== */
/*  State persistence                                                          */
/* ========================================================================== */

/*
 * State file format (one line per completed attempt):
 *   filename|wordlist_path|CRACKED|key
 *   filename|wordlist_path|NOKEY|
 */
static void load_state(crack_mgr_t *mgr) {
    FILE *f = fopen(CRACK_STATE_FILE, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        /* Tokenize: filename|wordlist|result|key */
        char *saveptr = NULL;
        char *tok_file   = strtok_r(line, "|", &saveptr);
        char *tok_wl     = strtok_r(NULL, "|", &saveptr);
        char *tok_result = strtok_r(NULL, "|", &saveptr);
        char *tok_key    = strtok_r(NULL, "\n", &saveptr);
        if (!tok_file || !tok_wl || !tok_result) continue;

        /* Find matching target index */
        int ti = -1;
        for (int i = 0; i < mgr->num_targets; i++) {
            if (strcmp(mgr->targets[i].filename, tok_file) == 0) {
                ti = i; break;
            }
        }
        /* Find matching wordlist index */
        int wi = -1;
        for (int i = 0; i < mgr->num_wordlists; i++) {
            if (strcmp(mgr->wordlists[i], tok_wl) == 0) {
                wi = i; break;
            }
        }

        if (ti >= 0 && wi >= 0) {
            mgr->tried[ti][wi] = true;
            if (strstr(tok_result, "CRACKED") && tok_key && strlen(tok_key) > 0) {
                /* Trim trailing whitespace */
                char *end = tok_key + strlen(tok_key) - 1;
                while (end > tok_key && (*end == '\n' || *end == '\r' || *end == ' '))
                    *end-- = '\0';
                if (!mgr->targets[ti].cracked) {
                    mgr->targets[ti].cracked = true;
                    mgr->total_cracked++;
                }
                strncpy(mgr->targets[ti].key, tok_key,
                        sizeof(mgr->targets[ti].key) - 1);
            }
        }
    }
    fclose(f);
}

static void save_state(crack_mgr_t *mgr) {
    ensure_output_dir();
    FILE *f = fopen(CRACK_STATE_FILE, "w");
    if (!f) return;

    fprintf(f, "# PwnaUI crack state — auto-generated, do not edit\n");
    for (int ti = 0; ti < mgr->num_targets; ti++) {
        for (int wi = 0; wi < mgr->num_wordlists; wi++) {
            if (!mgr->tried[ti][wi]) continue;
            fprintf(f, "%s|%s|%s|%s\n",
                    mgr->targets[ti].filename,
                    mgr->wordlists[wi],
                    mgr->targets[ti].cracked ? "CRACKED" : "NOKEY",
                    mgr->targets[ti].cracked ? mgr->targets[ti].key : "");
        }
    }
    fclose(f);
}

static void log_crack(const char *ssid, const char *bssid,
                       const char *key, const char *wordlist) {
    ensure_output_dir();
    FILE *f = fopen(CRACK_LOG_FILE, "a");
    if (!f) return;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);

    fprintf(f, "[%s] CRACKED: %s (%s) key=\"%s\" wordlist=%s\n",
            ts, ssid, bssid, key, wordlist);
    fclose(f);
}

/* ========================================================================== */
/*  Public API                                                                 */
/* ========================================================================== */

crack_mgr_t *crack_mgr_create(void) {
    crack_mgr_t *mgr = calloc(1, sizeof(crack_mgr_t));
    if (!mgr) return NULL;

    ensure_output_dir();
    mgr->state      = CRACK_IDLE;
    mgr->child_pid  = -1;
    mgr->cur_target  = -1;
    mgr->cur_wordlist = -1;

    return mgr;
}

void crack_mgr_scan(crack_mgr_t *mgr) {
    if (!mgr) return;

    /* Reset */
    mgr->num_targets = 0;
    memset(mgr->tried, 0, sizeof(mgr->tried));
    mgr->total_cracked = 0;

    /* Scan handshakes directory for .pcap files */
    DIR *dir = opendir(CRACK_HANDSHAKES_DIR);
    if (!dir) {
        fprintf(stderr, "[crack] cannot open %s\n", CRACK_HANDSHAKES_DIR);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL &&
           mgr->num_targets < CRACK_MAX_TARGETS) {
        const char *name = ent->d_name;
        size_t len = strlen(name);

        /* Only .pcap files (not .pcapng) */
        if (len < 6) continue;
        if (strcmp(name + len - 5, ".pcap") != 0) continue;
        if (len > 7 && strcmp(name + len - 7, ".pcapng") == 0) continue;

        crack_target_t *t = &mgr->targets[mgr->num_targets];
        memset(t, 0, sizeof(*t));

        if (!parse_bssid_from_filename(name, t->bssid, t->ssid)) continue;

        strncpy(t->filename, name, sizeof(t->filename) - 1);
        mgr->num_targets++;
    }
    closedir(dir);

    /* Also check for existing .key files in output dir (previous cracks) */
    dir = opendir(CRACK_OUTPUT_DIR);
    if (dir) {
        while ((ent = readdir(dir)) != NULL) {
            const char *name = ent->d_name;
            size_t len = strlen(name);
            if (len < 5 || strcmp(name + len - 4, ".key") != 0) continue;

            /* Extract SSID from filename (remove .key) */
            char ssid[64] = {0};
            int ssid_len = (int)(len - 4);
            if (ssid_len > 63) ssid_len = 63;
            memcpy(ssid, name, ssid_len);

            /* Find matching target and mark cracked */
            for (int i = 0; i < mgr->num_targets; i++) {
                if (strcmp(mgr->targets[i].ssid, ssid) == 0 &&
                    !mgr->targets[i].cracked) {
                    /* Read the key from the file */
                    char path[CRACK_MAX_PATH];
                    snprintf(path, sizeof(path), "%s/%s", CRACK_OUTPUT_DIR, name);
                    FILE *kf = fopen(path, "r");
                    if (kf) {
                        char key[128] = {0};
                        if (fgets(key, sizeof(key), kf)) {
                            char *nl = strchr(key, '\n');
                            if (nl) *nl = '\0';
                            if (strlen(key) > 0) {
                                mgr->targets[i].cracked = true;
                                strncpy(mgr->targets[i].key, key,
                                        sizeof(mgr->targets[i].key) - 1);
                                mgr->total_cracked++;
                            }
                        }
                        fclose(kf);
                    }
                }
            }
        }
        closedir(dir);
    }

    /* Scan wordlists */
    scan_wordlists(mgr);

    /* Load saved attempt state */
    load_state(mgr);

    /* Mark all wordlists as tried for targets found cracked via .key files.
     * This ensures save_state() writes CRACKED entries for them. */
    for (int i = 0; i < mgr->num_targets; i++) {
        if (mgr->targets[i].cracked) {
            for (int w = 0; w < mgr->num_wordlists; w++) {
                mgr->tried[i][w] = true;
            }
        }
    }

    mgr->last_scan = time(NULL);

    fprintf(stderr, "[crack] scanned: %d targets, %d wordlists, %d already cracked\n",
            mgr->num_targets, mgr->num_wordlists, mgr->total_cracked);
}

/*
 * Find next untried target+wordlist combination.
 * Strategy: iterate wordlists first (small → large), then targets.
 * This means we try ALL targets with the fastest wordlist before moving
 * to slower wordlists — maximizing quick wins.
 */
static bool find_next(crack_mgr_t *mgr, int *out_ti, int *out_wi) {
    for (int wi = 0; wi < mgr->num_wordlists; wi++) {
        for (int ti = 0; ti < mgr->num_targets; ti++) {
            if (mgr->targets[ti].cracked) continue;
            if (mgr->tried[ti][wi]) continue;
            *out_ti = ti;
            *out_wi = wi;
            return true;
        }
    }
    return false;
}

bool crack_mgr_start(crack_mgr_t *mgr) {
    if (!mgr || mgr->state == CRACK_RUNNING) return false;
    if (mgr->num_targets == 0 || mgr->num_wordlists == 0) return false;

    /* Re-scan if it's been more than 5 minutes (new handshakes may exist) */
    if (time(NULL) - mgr->last_scan > 300) {
        crack_mgr_scan(mgr);
    }

    int ti, wi;
    if (!find_next(mgr, &ti, &wi)) {
        return false;  /* All combos exhausted */
    }

    crack_target_t *t = &mgr->targets[ti];

    /* Build full pcap path */
    char pcap_path[CRACK_MAX_PATH];
    snprintf(pcap_path, sizeof(pcap_path), "%s/%s",
             CRACK_HANDSHAKES_DIR, t->filename);

    /* Key output file: /home/pi/cracked/SSID.key */
    snprintf(mgr->cur_keyfile, sizeof(mgr->cur_keyfile),
             "%s/%s.key", CRACK_OUTPUT_DIR, t->ssid);

    /* Remove stale keyfile if present */
    unlink(mgr->cur_keyfile);

    mgr->cur_target   = ti;
    mgr->cur_wordlist = wi;

    /* Extract just the wordlist filename for logging */
    const char *wl_name = strrchr(mgr->wordlists[wi], '/');
    wl_name = wl_name ? wl_name + 1 : mgr->wordlists[wi];

    fprintf(stderr, "[crack] starting: %s (%s) with %s\n",
            t->ssid, t->bssid, wl_name);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[crack] fork failed: %s\n", strerror(errno));
        return false;
    }

    if (pid == 0) {
        /* ── Child process ────────────────────────────────────────────── */

        /* Lowest CPU + I/O priority so we never interfere with attacks */
        nice(19);

        /* Silence stdout/stderr */
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);

        /* Close all inherited file descriptors except stdin/stdout/stderr */
        for (int fd = 3; fd < 256; fd++) close(fd);

        execlp("aircrack-ng", "aircrack-ng",
               "-a", "2",                     /* WPA/WPA2 mode */
               "-w", mgr->wordlists[wi],      /* Wordlist */
               "-b", t->bssid,                /* Target BSSID */
               "-l", mgr->cur_keyfile,        /* Write key here on success */
               "-q",                          /* Quiet */
               pcap_path,                     /* Input pcap */
               (char *)NULL);

        /* exec failed */
        _exit(127);
    }

    /* ── Parent process ───────────────────────────────────────────────── */
    mgr->child_pid = pid;
    mgr->state     = CRACK_RUNNING;
    mgr->last_start = time(NULL);
    mgr->total_attempts++;

    return true;
}

void crack_mgr_stop(crack_mgr_t *mgr) {
    if (!mgr || mgr->state != CRACK_RUNNING || mgr->child_pid <= 0) return;

    fprintf(stderr, "[crack] stopping pid %d (interrupted for attacks)\n",
            mgr->child_pid);

    /* Graceful shutdown first */
    kill(mgr->child_pid, SIGTERM);

    int status;
    pid_t result = waitpid(mgr->child_pid, &status, WNOHANG);
    if (result == 0) {
        usleep(200000);  /* 200ms grace */
        result = waitpid(mgr->child_pid, &status, WNOHANG);
        if (result == 0) {
            kill(mgr->child_pid, SIGKILL);
            waitpid(mgr->child_pid, &status, 0);
        }
    }

    /*
     * Don't mark as tried — we were interrupted mid-run.
     * This combo will be retried next time we're idle.
     */
    mgr->child_pid = -1;
    mgr->state     = CRACK_IDLE;
}


/* Sprint 7 #19: Cracked password feedback - add to learned wordlist with variants */
static void crack_feedback_add_password(const char *password) {
    if (!password || strlen(password) < 1) return;

    const char *learned_path = "/home/pi/wordlists/learned.txt";
    char variants[32][128];
    int nv = 0;

    /* Original */
    snprintf(variants[nv++], 128, "%s", password);

    /* Capitalize first letter */
    if (strlen(password) > 0 && islower((unsigned char)password[0])) {
        snprintf(variants[nv], 128, "%s", password);
        variants[nv][0] = toupper((unsigned char)variants[nv][0]);
        nv++;
    }

    /* All uppercase */
    snprintf(variants[nv], 128, "%s", password);
    for (int i = 0; variants[nv][i]; i++)
        variants[nv][i] = toupper((unsigned char)variants[nv][i]);
    nv++;

    /* All lowercase */
    snprintf(variants[nv], 128, "%s", password);
    for (int i = 0; variants[nv][i]; i++)
        variants[nv][i] = tolower((unsigned char)variants[nv][i]);
    nv++;

    /* Append common suffixes */
    const char *suffixes[] = {"1", "!", "123", "2024", "2025", "01", "69", "99", NULL};
    for (int s = 0; suffixes[s] && nv < 30; s++) {
        snprintf(variants[nv++], 128, "%s%s", password, suffixes[s]);
    }

    /* Read existing learned passwords */
    char existing[512][128];
    int nexist = 0;
    FILE *f = fopen(learned_path, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f) && nexist < 512) {
            line[strcspn(line, "\n\r")] = 0;
            if (strlen(line) > 0) {
                snprintf(existing[nexist++], 128, "%s", line);
            }
        }
        fclose(f);
    }

    /* Append new variants */
    f = fopen(learned_path, "a");
    if (!f) return;
    int added = 0;
    for (int v = 0; v < nv; v++) {
        bool found = false;
        for (int e = 0; e < nexist; e++) {
            if (strcmp(existing[e], variants[v]) == 0) { found = true; break; }
        }
        if (!found) {
            fprintf(f, "%s\n", variants[v]);
            added++;
        }
    }
    fclose(f);

    if (added > 0) {
        fprintf(stderr, "[crack] Feedback: added %d variants of '%s' to learned.txt\n",
                added, password);
    }
}


bool crack_mgr_check(crack_mgr_t *mgr) {
    if (!mgr || mgr->state != CRACK_RUNNING || mgr->child_pid <= 0)
        return false;

    int status;
    pid_t result = waitpid(mgr->child_pid, &status, WNOHANG);

    if (result == 0) {
        /* Still running */
        return false;
    }

    /* Process exited */
    mgr->child_pid = -1;
    mgr->state     = CRACK_IDLE;

    int ti = mgr->cur_target;
    int wi = mgr->cur_wordlist;
    if (ti < 0 || wi < 0) return false;

    /* Mark this combo as completed (whether key found or not) */
    mgr->tried[ti][wi] = true;

    /* Check if aircrack-ng wrote a key file */
    bool found = false;
    FILE *f = fopen(mgr->cur_keyfile, "r");
    if (f) {
        char key[128] = {0};
        if (fgets(key, sizeof(key), f)) {
            /* Trim */
            char *nl = strchr(key, '\n');
            if (nl) *nl = '\0';
            nl = strchr(key, '\r');
            if (nl) *nl = '\0';

            if (strlen(key) > 0) {
                found = true;
                strncpy(mgr->targets[ti].key, key,
                        sizeof(mgr->targets[ti].key) - 1);
                if (!mgr->targets[ti].cracked) {
                    mgr->targets[ti].cracked = true;
                    mgr->total_cracked++;
                }

                fprintf(stderr,
                    "[crack] *** KEY FOUND: %s (%s) = \"%s\" ***\n",
                    mgr->targets[ti].ssid, mgr->targets[ti].bssid, key);

                log_crack(mgr->targets[ti].ssid, mgr->targets[ti].bssid,
                          key, mgr->wordlists[wi]);
                crack_feedback_add_password(key);
            }
        }
        fclose(f);
    }

    if (!found) {
        /* Clean up empty/stale keyfile */
        unlink(mgr->cur_keyfile);

        const char *wl_name = strrchr(mgr->wordlists[wi], '/');
        wl_name = wl_name ? wl_name + 1 : mgr->wordlists[wi];

        fprintf(stderr, "[crack] no key: %s with %s (%.0fs)\n",
                mgr->targets[ti].ssid, wl_name,
                difftime(time(NULL), mgr->last_start));
    }

    /* Persist state so we don't retry after restart */
    save_state(mgr);

    return found;
}

bool crack_mgr_exhausted(crack_mgr_t *mgr) {
    if (!mgr) return true;
    int ti, wi;
    return !find_next(mgr, &ti, &wi);
}

void crack_mgr_status(crack_mgr_t *mgr, char *buf, int buflen) {
    if (!mgr || mgr->num_targets == 0) {
        snprintf(buf, buflen, "crack: no targets");
        return;
    }

    /* Count remaining combos */
    int remaining = 0;
    for (int ti = 0; ti < mgr->num_targets; ti++) {
        if (mgr->targets[ti].cracked) continue;
        for (int wi = 0; wi < mgr->num_wordlists; wi++) {
            if (!mgr->tried[ti][wi]) remaining++;
        }
    }

    if (mgr->state == CRACK_RUNNING && mgr->cur_target >= 0) {
        const char *wl_name = strrchr(mgr->wordlists[mgr->cur_wordlist], '/');
        wl_name = wl_name ? wl_name + 1 : mgr->wordlists[mgr->cur_wordlist];

        snprintf(buf, buflen, "cracking %s (%s) [%d cracked, %d left]",
                 mgr->targets[mgr->cur_target].ssid,
                 wl_name,
                 mgr->total_cracked, remaining);
    } else if (remaining == 0) {
        snprintf(buf, buflen, "crack: exhausted [%d/%d cracked]",
                 mgr->total_cracked, mgr->num_targets);
    } else {
        snprintf(buf, buflen, "crack: idle [%d cracked, %d left]",
                 mgr->total_cracked, remaining);
    }
}

void crack_mgr_destroy(crack_mgr_t *mgr) {
    if (!mgr) return;
    crack_mgr_stop(mgr);
    free(mgr);
}
