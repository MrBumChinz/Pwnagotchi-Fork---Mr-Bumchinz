/*
 * eapol_monitor.h — Real-Time EAPOL Handshake Monitor
 *
 * Phase 1, Task 1A: Live BPF-based EAPOL frame monitoring.
 * Listens on wlan0mon for 802.1X (EAPOL) key frames, tracks per-BSSID
 * state machines, and fires a callback the instant a handshake completes.
 *
 * This lets the brain STOP attacking an AP immediately after capture,
 * instead of waiting for pcap file analysis.
 *
 * State machine per BSSID:
 *   NONE → M1_SEEN → M1M2_SEEN → M1M2M3_SEEN → FULL_HS
 *   NONE → PMKID_SEEN (from M1 key data)
 *
 * Thread-safe: runs its own capture thread, calls back into brain.
 */

#ifndef EAPOL_MONITOR_H
#define EAPOL_MONITOR_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define EAPOL_MON_MAX_TRACKED   128     /* Max BSSIDs we track simultaneously */
#define EAPOL_MON_IFACE         "wlan0mon"
#define EAPOL_MON_SNAP_LEN      512     /* Enough for EAPOL key frame */
#define EAPOL_MON_TIMEOUT_MS    100     /* recv timeout for clean shutdown */
#define EAPOL_MON_STALE_SECS    300     /* Evict tracking entries after 5 min */

/* EAPOL EtherType */
#define EAPOL_ETHERTYPE         0x888e

/* EAPOL packet types */
#define EAPOL_TYPE_KEY          3

/* WPA key info bit masks */
#define WPA_KEY_ACK             0x0080
#define WPA_KEY_MIC             0x0100
#define WPA_KEY_SECURE          0x0200
#define WPA_KEY_INSTALL         0x0040
#define WPA_KEY_PAIRWISE        0x0008
#define WPA_KEY_ERROR           0x0400

/* PMKID detection in M1 key data */
#define PMKID_TAG_TYPE          0xDD    /* Vendor-specific IE */
#define PMKID_OUI_TYPE          4       /* RSN PMKID type */
#define PMKID_LEN               16

/* ============================================================================
 * Types
 * ============================================================================ */

/* Per-BSSID handshake completion state */
typedef enum {
    EAPOL_STATE_NONE = 0,       /* No EAPOL frames seen */
    EAPOL_STATE_M1,             /* M1 seen (AP → STA: ANonce, ACK, no MIC) */
    EAPOL_STATE_M1M2,           /* M1+M2 seen (crackable pair) */
    EAPOL_STATE_M1M2M3,         /* M1+M2+M3 seen */
    EAPOL_STATE_FULL_HS,        /* Complete 4-way handshake */
    EAPOL_STATE_PMKID           /* PMKID extracted from M1 */
} eapol_state_t;

/* What type of capture completed */
typedef enum {
    EAPOL_CAPTURE_NONE = 0,
    EAPOL_CAPTURE_PMKID,        /* PMKID only */
    EAPOL_CAPTURE_PAIR,         /* M1+M2 (crackable, not full) */
    EAPOL_CAPTURE_FULL          /* Complete 4-way */
} eapol_capture_type_t;

/* Quality score for captured handshake */
typedef struct {
    bool has_m1;
    bool has_m2;
    bool has_m3;
    bool has_m4;
    bool has_pmkid;
    bool anonce_valid;          /* ANonce non-zero */
    bool snonce_valid;          /* SNonce non-zero */
    bool replay_match;          /* M1 replay + 1 == M2 replay (same exchange) */
    uint8_t score;              /* 0-100 quality score */
} eapol_quality_t;

/* Per-BSSID tracking entry */
typedef struct {
    uint8_t bssid[6];           /* AP MAC address */
    uint8_t sta[6];             /* Client MAC address (best candidate) */
    eapol_state_t state;        /* Current state machine position */

    /* Per-message tracking */
    uint8_t anonce[32];         /* ANonce from M1 */
    uint8_t snonce[32];         /* SNonce from M2 */
    uint64_t m1_replay;         /* Replay counter from M1 */
    uint64_t m2_replay;         /* Replay counter from M2 */

    /* Timestamps */
    time_t first_seen;          /* When first EAPOL frame arrived */
    time_t last_seen;           /* Most recent EAPOL frame */
    time_t m1_time;             /* M1 arrival time */
    time_t m2_time;             /* M2 arrival time */

    /* Quality */
    eapol_quality_t quality;

    /* Flags */
    bool notified;              /* Already called back for this BSSID */
    bool active;                /* Slot in use */
} eapol_track_t;

/* Callback: fired when a handshake completes */
typedef void (*eapol_capture_cb_t)(
    const uint8_t *bssid,           /* AP MAC (6 bytes) */
    eapol_capture_type_t type,      /* What kind of capture */
    const eapol_quality_t *quality,  /* Quality details */
    void *user_data                 /* Opaque pointer (brain_ctx) */
);

/* Monitor context */
typedef struct {
    /* Capture thread */
    pthread_t thread;
    pthread_mutex_t lock;
    bool running;
    bool started;

    /* Raw socket */
    int sock_fd;
    char iface[32];

    /* Per-BSSID tracking */
    eapol_track_t tracked[EAPOL_MON_MAX_TRACKED];
    int tracked_count;

    /* Callback */
    eapol_capture_cb_t on_capture;
    void *user_data;

    /* Stats */
    uint64_t total_eapol_frames;    /* Total EAPOL frames processed */
    uint64_t total_m1;
    uint64_t total_m2;
    uint64_t total_m3;
    uint64_t total_m4;
    uint64_t total_pmkid;
    uint32_t captures_full;         /* Complete 4-way handshakes detected */
    uint32_t captures_pmkid;        /* PMKIDs detected */
    uint32_t captures_pair;         /* M1+M2 pairs detected */
} eapol_monitor_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/*
 * Initialize the EAPOL monitor.
 * Creates raw socket with BPF filter on monitor interface.
 * Does NOT start capture thread — call eapol_monitor_start() for that.
 *
 * Returns 0 on success, -1 on error.
 */
int eapol_monitor_init(eapol_monitor_t *mon, const char *iface);

/*
 * Set the capture callback.
 * Called from capture thread when a handshake completes.
 * Must be thread-safe (brain uses mutex internally).
 */
void eapol_monitor_set_callback(eapol_monitor_t *mon,
                                 eapol_capture_cb_t cb,
                                 void *user_data);

/*
 * Start the capture thread.
 * Returns 0 on success, -1 on error.
 */
int eapol_monitor_start(eapol_monitor_t *mon);

/*
 * Stop the capture thread and close socket.
 */
void eapol_monitor_stop(eapol_monitor_t *mon);

/*
 * Check current state for a given BSSID.
 * Thread-safe (takes lock).
 * Returns EAPOL_STATE_NONE if BSSID is not tracked.
 */
eapol_state_t eapol_monitor_get_state(eapol_monitor_t *mon,
                                       const uint8_t *bssid);

/*
 * Check if a BSSID has any capture (PMKID, pair, or full).
 * Thread-safe. This is the fast check called by brain before attacking.
 */
bool eapol_monitor_has_capture(eapol_monitor_t *mon,
                                const uint8_t *bssid);

/*
 * Get quality details for a BSSID.
 * Returns false if not tracked or no capture yet.
 */
bool eapol_monitor_get_quality(eapol_monitor_t *mon,
                                const uint8_t *bssid,
                                eapol_quality_t *out);

/*
 * Reset tracking for a BSSID (e.g., if we want to re-capture a better one).
 */
void eapol_monitor_reset_bssid(eapol_monitor_t *mon, const uint8_t *bssid);

/*
 * Evict stale entries older than EAPOL_MON_STALE_SECS.
 * Called periodically from brain epoch.
 */
void eapol_monitor_evict_stale(eapol_monitor_t *mon);

/*
 * Format BSSID for logging: "AA:BB:CC:DD:EE:FF"
 */
void eapol_mac_to_str(const uint8_t *mac, char *out);

#endif /* EAPOL_MONITOR_H */
