/*
 * crack_manager.h — Idle handshake cracking for PwnaUI
 *
 * When the brain is BORED (all visible APs have FULL handshakes) or LONELY
 * (no APs visible), we run aircrack-ng at lowest CPU priority against
 * captured handshakes using available wordlists.
 *
 * Cracking is killed instantly when the brain needs CPU for attacks.
 * Results are saved to /home/pi/cracked/ and logged to journalctl.
 */

#ifndef CRACK_MANAGER_H
#define CRACK_MANAGER_H

#include <stdbool.h>
#include <sys/types.h>
#include <time.h>

#define CRACK_MAX_PATH      256
#define CRACK_MAX_TARGETS   64
#define CRACK_MAX_WORDLISTS 8

#define CRACK_HANDSHAKES_DIR "/home/pi/handshakes"
#define CRACK_OUTPUT_DIR     "/home/pi/cracked"
#define CRACK_STATE_FILE     "/home/pi/cracked/state.txt"
#define CRACK_LOG_FILE       "/home/pi/cracked/log.txt"
#define CRACK_WORDLIST_DIR   "/home/pi/wordlists"

typedef enum {
    CRACK_IDLE,         /* Not cracking */
    CRACK_RUNNING,      /* aircrack-ng running in background */
} crack_state_t;

typedef struct {
    char filename[CRACK_MAX_PATH];  /* e.g. "Telstra3DB7_2c3033149231.pcap" */
    char ssid[64];
    char bssid[18];                 /* "2c:30:33:14:92:31" */
    bool cracked;
    char key[128];                  /* The cracked password */
} crack_target_t;

typedef struct {
    crack_state_t state;
    pid_t child_pid;

    /* Current task */
    int cur_target;
    int cur_wordlist;
    char cur_keyfile[CRACK_MAX_PATH];

    /* Targets (handshake pcap files) */
    crack_target_t targets[CRACK_MAX_TARGETS];
    int num_targets;

    /* Wordlists (ordered: smallest first) */
    char wordlists[CRACK_MAX_WORDLISTS][CRACK_MAX_PATH];
    int num_wordlists;

    /* Attempt tracking: tried[target][wordlist] */
    bool tried[CRACK_MAX_TARGETS][CRACK_MAX_WORDLISTS];

    /* Stats */
    int total_cracked;
    int total_attempts;
    time_t last_start;
    time_t last_scan;
} crack_mgr_t;

/* Create and initialize the crack manager */
crack_mgr_t *crack_mgr_create(void);

/* Scan handshakes dir and wordlists dir, load saved state */
void crack_mgr_scan(crack_mgr_t *mgr);

/* Start cracking next untried target+wordlist combo.
 * Returns true if a crack process was started. */
bool crack_mgr_start(crack_mgr_t *mgr);

/* Stop current cracking process (kills aircrack-ng).
 * Does NOT mark the current combo as tried — it will be retried. */
void crack_mgr_stop(crack_mgr_t *mgr);

/* Non-blocking check if the running process finished.
 * Returns true if a key was found this call. */
bool crack_mgr_check(crack_mgr_t *mgr);

/* Are all target × wordlist combos exhausted? */
bool crack_mgr_exhausted(crack_mgr_t *mgr);

/* Write a status string for logging */
void crack_mgr_status(crack_mgr_t *mgr, char *buf, int buflen);

/* Cleanup (stops any running process, frees memory) */
void crack_mgr_destroy(crack_mgr_t *mgr);

#endif /* CRACK_MANAGER_H */
