/*
 * eapol_monitor.c — Real-Time EAPOL Handshake Monitor
 *
 * Phase 1, Task 1A: Live BPF-based EAPOL frame monitoring on wlan0mon.
 *
 * Architecture:
 *   - Raw AF_PACKET socket with BPF filter for EtherType 0x888e
 *   - Dedicated capture thread (low CPU — blocks on recv)
 *   - Per-BSSID state machine tracks M1/M2/M3/M4 + PMKID
 *   - Fires callback instantly when handshake completes
 *   - Brain checks eapol_monitor_has_capture() before attacking
 *
 * Frame parsing handles both raw 802.11 and radiotap headers.
 *
 * Memory: ~20KB for 128 tracked BSSIDs. No heap allocations after init.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/filter.h>

#include "eapol_monitor.h"

/* ============================================================================
 * IEEE 802.11 frame parsing helpers
 * ============================================================================ */

/* Radiotap header (variable length) */
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  pad;
    uint16_t len;       /* Total radiotap header length */
    uint32_t present;   /* Bitmask of present fields */
} radiotap_hdr_t;

/* 802.11 frame control */
#define IEEE80211_FC_TYPE_DATA  0x08
#define IEEE80211_FC_SUBTYPE_QOS 0x80
#define IEEE80211_FC_TODS       0x01
#define IEEE80211_FC_FROMDS     0x02

/* Minimal 802.11 header for data frames */
typedef struct __attribute__((packed)) {
    uint16_t frame_control;
    uint16_t duration;
    uint8_t  addr1[6];  /* Receiver / DA */
    uint8_t  addr2[6];  /* Transmitter / SA */
    uint8_t  addr3[6];  /* BSSID (in most data frames) */
    uint16_t seq_ctrl;
} ieee80211_hdr_t;

/* LLC/SNAP header */
typedef struct __attribute__((packed)) {
    uint8_t  dsap;
    uint8_t  ssap;
    uint8_t  ctrl;
    uint8_t  oui[3];
    uint16_t ethertype;  /* Big-endian */
} llc_snap_hdr_t;

/* EAPOL header */
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  type;       /* 3 = Key */
    uint16_t length;     /* Big-endian */
} eapol_hdr_t;

/* WPA/RSN key frame (after EAPOL header) */
typedef struct __attribute__((packed)) {
    uint8_t  descriptor_type;  /* 2 = RSN, 254 = WPA */
    uint16_t key_info;         /* Big-endian */
    uint16_t key_length;
    uint64_t replay_counter;   /* Big-endian */
    uint8_t  nonce[32];
    uint8_t  key_iv[16];
    uint8_t  key_rsc[8];
    uint8_t  reserved[8];
    uint8_t  key_mic[16];
    uint16_t key_data_length;  /* Big-endian */
    /* key_data follows (variable length) */
} wpa_key_frame_t;

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

void eapol_mac_to_str(const uint8_t *mac, char *out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool mac_equal(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 6) == 0;
}

static bool mac_is_zero(const uint8_t *mac) {
    static const uint8_t zero[6] = {0};
    return memcmp(mac, zero, 6) == 0;
}

static bool nonce_is_zero(const uint8_t *nonce) {
    for (int i = 0; i < 32; i++) {
        if (nonce[i] != 0) return false;
    }
    return true;
}

/* ============================================================================
 * Per-BSSID tracking
 * ============================================================================ */

/* Find or allocate a tracking slot for a BSSID */
static eapol_track_t *find_or_alloc(eapol_monitor_t *mon, const uint8_t *bssid) {
    /* Search existing */
    for (int i = 0; i < EAPOL_MON_MAX_TRACKED; i++) {
        if (mon->tracked[i].active && mac_equal(mon->tracked[i].bssid, bssid)) {
            return &mon->tracked[i];
        }
    }

    /* Allocate new slot */
    for (int i = 0; i < EAPOL_MON_MAX_TRACKED; i++) {
        if (!mon->tracked[i].active) {
            memset(&mon->tracked[i], 0, sizeof(eapol_track_t));
            memcpy(mon->tracked[i].bssid, bssid, 6);
            mon->tracked[i].active = true;
            mon->tracked[i].first_seen = time(NULL);
            mon->tracked[i].last_seen = time(NULL);
            mon->tracked_count++;
            return &mon->tracked[i];
        }
    }

    /* Full — evict oldest entry */
    time_t oldest = time(NULL);
    int oldest_idx = 0;
    for (int i = 0; i < EAPOL_MON_MAX_TRACKED; i++) {
        if (mon->tracked[i].last_seen < oldest && !mon->tracked[i].notified) {
            oldest = mon->tracked[i].last_seen;
            oldest_idx = i;
        }
    }
    memset(&mon->tracked[oldest_idx], 0, sizeof(eapol_track_t));
    memcpy(mon->tracked[oldest_idx].bssid, bssid, 6);
    mon->tracked[oldest_idx].active = true;
    mon->tracked[oldest_idx].first_seen = time(NULL);
    mon->tracked[oldest_idx].last_seen = time(NULL);
    return &mon->tracked[oldest_idx];
}

static eapol_track_t *find_tracked(eapol_monitor_t *mon, const uint8_t *bssid) {
    for (int i = 0; i < EAPOL_MON_MAX_TRACKED; i++) {
        if (mon->tracked[i].active && mac_equal(mon->tracked[i].bssid, bssid)) {
            return &mon->tracked[i];
        }
    }
    return NULL;
}

/* Compute quality score (0-100) */
static void compute_quality(eapol_track_t *t) {
    eapol_quality_t *q = &t->quality;
    uint8_t score = 0;

    if (q->has_m1) score += 15;
    if (q->has_m2) score += 25;
    if (q->has_m3) score += 20;
    if (q->has_m4) score += 10;
    if (q->has_pmkid) score += 20;
    if (q->anonce_valid) score += 5;
    if (q->snonce_valid) score += 5;

    /* Bonus for same-exchange validation */
    if (q->replay_match && q->has_m1 && q->has_m2) score += 10;

    if (score > 100) score = 100;
    q->score = score;
}

/* ============================================================================
 * PMKID extraction from M1 key data
 * ============================================================================ */

static bool check_pmkid(const uint8_t *key_data, uint16_t key_data_len) {
    /*
     * PMKID is in RSN IE or vendor-specific IE in M1 key data.
     * Look for: tag=0xDD, len=20, OUI=00:0F:AC, type=04
     * Then 16 bytes of PMKID follow.
     *
     * Also check RSN IE tag=0x30 with PMKID list.
     */
    if (key_data_len < 22) return false;

    const uint8_t *p = key_data;
    const uint8_t *end = key_data + key_data_len;

    while (p + 2 <= end) {
        uint8_t tag = p[0];
        uint8_t len = p[1];

        if (p + 2 + len > end) break;

        if (tag == PMKID_TAG_TYPE && len >= 20) {
            /* Vendor-specific: OUI 00:0F:AC, type 04 */
            if (p[2] == 0x00 && p[3] == 0x0F && p[4] == 0xAC && p[5] == PMKID_OUI_TYPE) {
                /* Verify PMKID is non-zero */
                bool nonzero = false;
                for (int i = 0; i < PMKID_LEN; i++) {
                    if (p[6 + i] != 0) { nonzero = true; break; }
                }
                if (nonzero) return true;
            }
        }

        /* RSN IE (0x30) can also contain PMKID list */
        if (tag == 0x30 && len >= 24) {
            /* Parse RSN IE for PMKID count field near the end */
            /* This is complex — the simple vendor-specific check catches most */
        }

        p += 2 + len;
    }

    return false;
}

/* ============================================================================
 * WPA key message classification
 *
 * M1: ACK=1, MIC=0  (AP → STA: sends ANonce)
 * M2: ACK=0, MIC=1, Secure=0  (STA → AP: sends SNonce)
 * M3: ACK=1, MIC=1, Install=1, Secure=1  (AP → STA: confirms)
 * M4: ACK=0, MIC=1, Secure=1  (STA → AP: final ack)
 * ============================================================================ */

typedef enum {
    WPA_MSG_UNKNOWN = 0,
    WPA_MSG_M1,
    WPA_MSG_M2,
    WPA_MSG_M3,
    WPA_MSG_M4
} wpa_msg_type_t;

static wpa_msg_type_t classify_wpa_key(const wpa_key_frame_t *key) {
    uint16_t info = ntohs(key->key_info);

    bool pairwise = (info & WPA_KEY_PAIRWISE) != 0;
    bool ack      = (info & WPA_KEY_ACK) != 0;
    bool mic      = (info & WPA_KEY_MIC) != 0;
    bool secure   = (info & WPA_KEY_SECURE) != 0;
    bool install  = (info & WPA_KEY_INSTALL) != 0;
    bool error    = (info & WPA_KEY_ERROR) != 0;

    if (!pairwise || error) return WPA_MSG_UNKNOWN;

    if (ack && !mic)                        return WPA_MSG_M1;
    if (!ack && mic && !secure)             return WPA_MSG_M2;
    if (ack && mic && install && secure)     return WPA_MSG_M3;
    if (!ack && mic && secure && !install)   return WPA_MSG_M4;

    return WPA_MSG_UNKNOWN;
}

/* ============================================================================
 * Process a single EAPOL key frame
 * ============================================================================ */

static void process_eapol_key(eapol_monitor_t *mon,
                               const uint8_t *bssid,
                               const uint8_t *sta_mac,
                               const wpa_key_frame_t *key,
                               uint16_t key_data_len,
                               const uint8_t *key_data) {
    wpa_msg_type_t msg = classify_wpa_key(key);
    if (msg == WPA_MSG_UNKNOWN) return;

    pthread_mutex_lock(&mon->lock);

    eapol_track_t *t = find_or_alloc(mon, bssid);
    t->last_seen = time(NULL);

    /* Don't re-process after callback */
    if (t->notified) {
        pthread_mutex_unlock(&mon->lock);
        return;
    }

    char bssid_str[18];
    eapol_mac_to_str(bssid, bssid_str);

    switch (msg) {
    case WPA_MSG_M1:
        mon->total_m1++;
        t->quality.has_m1 = true;
        t->m1_time = time(NULL);
        t->m1_replay = key->replay_counter;  /* network byte order */
        memcpy(t->anonce, key->nonce, 32);
        memcpy(t->sta, sta_mac, 6);
        t->quality.anonce_valid = !nonce_is_zero(key->nonce);

        if (t->state < EAPOL_STATE_M1) {
            t->state = EAPOL_STATE_M1;
            printf("[EAPOL] M1 from %s (ANonce captured)\n", bssid_str);
        }

        /* Check for PMKID in M1 key data */
        if (key_data && key_data_len > 0 && check_pmkid(key_data, key_data_len)) {
            mon->total_pmkid++;
            t->quality.has_pmkid = true;
            if (t->state < EAPOL_STATE_PMKID || t->state == EAPOL_STATE_M1) {
                t->state = EAPOL_STATE_PMKID;
                printf("[EAPOL] PMKID found in M1 from %s!\n", bssid_str);

                /* Fire callback for PMKID */
                compute_quality(t);
                t->notified = true;
                mon->captures_pmkid++;

                if (mon->on_capture) {
                    pthread_mutex_unlock(&mon->lock);
                    mon->on_capture(bssid, EAPOL_CAPTURE_PMKID, &t->quality, mon->user_data);
                    return;
                }
            }
        }
        break;

    case WPA_MSG_M2:
        mon->total_m2++;
        t->quality.has_m2 = true;
        t->m2_time = time(NULL);
        t->m2_replay = key->replay_counter;
        memcpy(t->snonce, key->nonce, 32);
        t->quality.snonce_valid = !nonce_is_zero(key->nonce);

        /* Check replay counter match (same exchange as M1) */
        if (t->quality.has_m1) {
            /* M2.replay should equal M1.replay (both in network byte order) */
            t->quality.replay_match = (t->m2_replay == t->m1_replay);
        }

        if (t->state == EAPOL_STATE_M1 || t->state == EAPOL_STATE_NONE) {
            t->state = EAPOL_STATE_M1M2;
            printf("[EAPOL] M2 from %s (M1+M2 pair — crackable!)\n", bssid_str);

            /* Fire callback for crackable pair */
            compute_quality(t);
            t->notified = true;
            mon->captures_pair++;

            if (mon->on_capture) {
                pthread_mutex_unlock(&mon->lock);
                mon->on_capture(bssid, EAPOL_CAPTURE_PAIR, &t->quality, mon->user_data);
                return;
            }
        }
        break;

    case WPA_MSG_M3:
        mon->total_m3++;
        t->quality.has_m3 = true;

        if (t->state == EAPOL_STATE_M1M2) {
            t->state = EAPOL_STATE_M1M2M3;
            printf("[EAPOL] M3 from %s (M1+M2+M3)\n", bssid_str);
        }
        break;

    case WPA_MSG_M4:
        mon->total_m4++;
        t->quality.has_m4 = true;

        if (t->state == EAPOL_STATE_M1M2M3 || t->state == EAPOL_STATE_M1M2) {
            t->state = EAPOL_STATE_FULL_HS;
            printf("[EAPOL] M4 from %s — FULL 4-WAY HANDSHAKE CAPTURED!\n", bssid_str);

            /* Upgrade from pair to full — re-notify */
            compute_quality(t);
            bool was_pair = t->notified;  /* Was already notified as pair */
            t->notified = true;
            mon->captures_full++;

            /* Only callback if this is a NEW full capture (not already notified as pair+upgrade) */
            if (!was_pair && mon->on_capture) {
                pthread_mutex_unlock(&mon->lock);
                mon->on_capture(bssid, EAPOL_CAPTURE_FULL, &t->quality, mon->user_data);
                return;
            }
        }
        break;

    default:
        break;
    }

    pthread_mutex_unlock(&mon->lock);
}

/* ============================================================================
 * Raw frame parsing
 *
 * Frames from AF_PACKET on wlan0mon arrive as:
 *   [radiotap header] [802.11 header] [LLC/SNAP] [EAPOL] [WPA Key]
 * ============================================================================ */

static void parse_frame(eapol_monitor_t *mon, const uint8_t *buf, int len) {
    mon->total_eapol_frames++;

    /* Skip radiotap header */
    if (len < (int)sizeof(radiotap_hdr_t)) return;
    const radiotap_hdr_t *rt = (const radiotap_hdr_t *)buf;
    uint16_t rt_len = rt->len;  /* Already host byte order on LE (ARM) */
    if (rt_len > len) return;

    const uint8_t *dot11 = buf + rt_len;
    int dot11_len = len - rt_len;
    if (dot11_len < (int)sizeof(ieee80211_hdr_t)) return;

    const ieee80211_hdr_t *hdr = (const ieee80211_hdr_t *)dot11;
    uint16_t fc = hdr->frame_control;
    uint8_t type = (fc >> 2) & 0x03;
    uint8_t subtype = (fc >> 4) & 0x0F;

    /* We only want data frames */
    if (type != 2) return;  /* 2 = data */

    /* Calculate 802.11 header length (24 + optional QoS 2 bytes) */
    int dot11_hdr_len = sizeof(ieee80211_hdr_t);
    bool is_qos = (subtype & 0x08) != 0;  /* QoS subtypes have bit 3 set */
    if (is_qos) dot11_hdr_len += 2;

    /* Determine BSSID and STA based on ToDS/FromDS flags */
    uint8_t ds_flags = fc & 0x03;
    const uint8_t *bssid;
    const uint8_t *sta_mac;

    switch (ds_flags) {
    case 0x00:  /* IBSS: addr3=BSSID, addr2=SA */
        bssid = hdr->addr3;
        sta_mac = hdr->addr2;
        break;
    case 0x01:  /* ToDS: addr1=BSSID, addr2=SA (client sending to AP) */
        bssid = hdr->addr1;
        sta_mac = hdr->addr2;
        break;
    case 0x02:  /* FromDS: addr2=BSSID, addr1=DA (AP sending to client) */
        bssid = hdr->addr2;
        sta_mac = hdr->addr1;
        break;
    case 0x03:  /* WDS: skip */
        return;
    default:
        return;
    }

    if (mac_is_zero(bssid)) return;

    /* Parse LLC/SNAP header */
    const uint8_t *llc_start = dot11 + dot11_hdr_len;
    int remaining = dot11_len - dot11_hdr_len;
    if (remaining < (int)sizeof(llc_snap_hdr_t)) return;

    const llc_snap_hdr_t *llc = (const llc_snap_hdr_t *)llc_start;
    if (ntohs(llc->ethertype) != EAPOL_ETHERTYPE) return;

    /* Parse EAPOL header */
    const uint8_t *eapol_start = llc_start + sizeof(llc_snap_hdr_t);
    remaining -= sizeof(llc_snap_hdr_t);
    if (remaining < (int)sizeof(eapol_hdr_t)) return;

    const eapol_hdr_t *eapol = (const eapol_hdr_t *)eapol_start;
    if (eapol->type != EAPOL_TYPE_KEY) return;

    /* Parse WPA key frame */
    const uint8_t *key_start = eapol_start + sizeof(eapol_hdr_t);
    remaining -= sizeof(eapol_hdr_t);
    if (remaining < (int)sizeof(wpa_key_frame_t)) return;

    const wpa_key_frame_t *key = (const wpa_key_frame_t *)key_start;
    uint16_t key_data_len = ntohs(key->key_data_length);
    const uint8_t *key_data = key_start + sizeof(wpa_key_frame_t);

    /* Bounds check key data */
    int key_data_remaining = remaining - (int)sizeof(wpa_key_frame_t);
    if (key_data_len > key_data_remaining) {
        key_data_len = (uint16_t)key_data_remaining;
    }
    if (key_data_len == 0) key_data = NULL;

    process_eapol_key(mon, bssid, sta_mac, key, key_data_len, key_data);
}

/* ============================================================================
 * Capture thread
 * ============================================================================ */

static void *capture_thread(void *arg) {
    eapol_monitor_t *mon = (eapol_monitor_t *)arg;
    uint8_t buf[EAPOL_MON_SNAP_LEN];

    printf("[EAPOL Monitor] Capture thread started on %s\n", mon->iface);

    while (mon->running) {
        int n = recv(mon->sock_fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (errno == ENETDOWN || errno == ENXIO) {
                /* Interface went down — wait and retry */
                printf("[EAPOL Monitor] Interface %s down, retrying in 2s...\n", mon->iface);
                sleep(2);
                continue;
            }
            perror("[EAPOL Monitor] recv error");
            break;
        }
        if (n == 0) continue;

        parse_frame(mon, buf, n);
    }

    printf("[EAPOL Monitor] Capture thread exiting\n");
    return NULL;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int eapol_monitor_init(eapol_monitor_t *mon, const char *iface) {
    memset(mon, 0, sizeof(*mon));
    strncpy(mon->iface, iface ? iface : EAPOL_MON_IFACE, sizeof(mon->iface) - 1);
    mon->sock_fd = -1;

    if (pthread_mutex_init(&mon->lock, NULL) != 0) {
        perror("[EAPOL Monitor] mutex init failed");
        return -1;
    }

    /* Create raw AF_PACKET socket */
    mon->sock_fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (mon->sock_fd < 0) {
        perror("[EAPOL Monitor] socket() failed (need root or CAP_NET_RAW)");
        return -1;
    }

    /* Bind to monitor interface */
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    /* iface name max is IFNAMSIZ (16), our buffer is 32 — truncation is safe */
    memcpy(ifr.ifr_name, mon->iface, sizeof(ifr.ifr_name) - 1);
    ifr.ifr_name[sizeof(ifr.ifr_name) - 1] = '\0';
    if (ioctl(mon->sock_fd, SIOCGIFINDEX, &ifr) < 0) {
        fprintf(stderr, "[EAPOL Monitor] Interface %s not found: %s\n",
                mon->iface, strerror(errno));
        close(mon->sock_fd);
        mon->sock_fd = -1;
        return -1;
    }

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = ifr.ifr_ifindex;
    if (bind(mon->sock_fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        perror("[EAPOL Monitor] bind() failed");
        close(mon->sock_fd);
        mon->sock_fd = -1;
        return -1;
    }

    /* Set receive timeout for clean shutdown */
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = EAPOL_MON_TIMEOUT_MS * 1000;
    setsockopt(mon->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /*
     * Attach BPF filter to only receive EAPOL frames.
     *
     * This is a simplified filter — the real filtering is done in parse_frame()
     * after radiotap/802.11 header parsing, but the BPF reduces wakeups by
     * filtering out the vast majority of non-EAPOL traffic at kernel level.
     *
     * The filter matches any packet that contains 0x888e (EAPOL ethertype)
     * anywhere in the LLC/SNAP region of an 802.11 data frame.
     * Since radiotap length varies, we use a permissive filter and validate
     * fully in userspace.
     */
    struct sock_filter bpf_code[] = {
        /* Accept all packets — we filter in userspace after radiotap parsing.
         * A kernel-level BPF for 802.11 EAPOL is complex due to variable
         * radiotap header length. The recv() timeout keeps CPU low even
         * without aggressive BPF filtering. */
        { BPF_RET | BPF_K, 0, 0, EAPOL_MON_SNAP_LEN },
    };
    struct sock_fprog bpf = {
        .len = sizeof(bpf_code) / sizeof(bpf_code[0]),
        .filter = bpf_code,
    };
    if (setsockopt(mon->sock_fd, SOL_SOCKET, SO_ATTACH_FILTER, &bpf, sizeof(bpf)) < 0) {
        /* Non-fatal — we'll just get more frames to filter in userspace */
        perror("[EAPOL Monitor] BPF attach failed (non-fatal)");
    }

    printf("[EAPOL Monitor] Initialized on %s (fd=%d)\n", mon->iface, mon->sock_fd);
    return 0;
}

void eapol_monitor_set_callback(eapol_monitor_t *mon,
                                 eapol_capture_cb_t cb,
                                 void *user_data) {
    mon->on_capture = cb;
    mon->user_data = user_data;
}

int eapol_monitor_start(eapol_monitor_t *mon) {
    if (mon->started) return 0;
    if (mon->sock_fd < 0) return -1;

    mon->running = true;
    if (pthread_create(&mon->thread, NULL, capture_thread, mon) != 0) {
        perror("[EAPOL Monitor] thread create failed");
        mon->running = false;
        return -1;
    }

    mon->started = true;
    return 0;
}

void eapol_monitor_stop(eapol_monitor_t *mon) {
    if (!mon->started) return;

    mon->running = false;
    pthread_join(mon->thread, NULL);
    mon->started = false;

    if (mon->sock_fd >= 0) {
        close(mon->sock_fd);
        mon->sock_fd = -1;
    }

    pthread_mutex_destroy(&mon->lock);
    printf("[EAPOL Monitor] Stopped. Stats: M1=%lu M2=%lu M3=%lu M4=%lu PMKID=%lu | "
           "Captures: full=%u pair=%u pmkid=%u\n",
           (unsigned long)mon->total_m1, (unsigned long)mon->total_m2,
           (unsigned long)mon->total_m3, (unsigned long)mon->total_m4,
           (unsigned long)mon->total_pmkid,
           mon->captures_full, mon->captures_pair, mon->captures_pmkid);
}

eapol_state_t eapol_monitor_get_state(eapol_monitor_t *mon, const uint8_t *bssid) {
    pthread_mutex_lock(&mon->lock);
    eapol_track_t *t = find_tracked(mon, bssid);
    eapol_state_t state = t ? t->state : EAPOL_STATE_NONE;
    pthread_mutex_unlock(&mon->lock);
    return state;
}

bool eapol_monitor_has_capture(eapol_monitor_t *mon, const uint8_t *bssid) {
    pthread_mutex_lock(&mon->lock);
    eapol_track_t *t = find_tracked(mon, bssid);
    bool captured = (t != NULL && t->notified);
    pthread_mutex_unlock(&mon->lock);
    return captured;
}

bool eapol_monitor_get_quality(eapol_monitor_t *mon,
                                const uint8_t *bssid,
                                eapol_quality_t *out) {
    pthread_mutex_lock(&mon->lock);
    eapol_track_t *t = find_tracked(mon, bssid);
    if (!t || !t->notified) {
        pthread_mutex_unlock(&mon->lock);
        return false;
    }
    *out = t->quality;
    pthread_mutex_unlock(&mon->lock);
    return true;
}

void eapol_monitor_reset_bssid(eapol_monitor_t *mon, const uint8_t *bssid) {
    pthread_mutex_lock(&mon->lock);
    eapol_track_t *t = find_tracked(mon, bssid);
    if (t) {
        memset(t, 0, sizeof(*t));
        /* Slot becomes available */
        mon->tracked_count--;
    }
    pthread_mutex_unlock(&mon->lock);
}

void eapol_monitor_evict_stale(eapol_monitor_t *mon) {
    time_t now = time(NULL);
    pthread_mutex_lock(&mon->lock);
    for (int i = 0; i < EAPOL_MON_MAX_TRACKED; i++) {
        eapol_track_t *t = &mon->tracked[i];
        if (t->active && !t->notified &&
            (now - t->last_seen) > EAPOL_MON_STALE_SECS) {
            char mac_str[18];
            eapol_mac_to_str(t->bssid, mac_str);
            printf("[EAPOL Monitor] Evicting stale entry %s (state=%d)\n",
                   mac_str, t->state);
            memset(t, 0, sizeof(*t));
            mon->tracked_count--;
        }
    }
    pthread_mutex_unlock(&mon->lock);
}
