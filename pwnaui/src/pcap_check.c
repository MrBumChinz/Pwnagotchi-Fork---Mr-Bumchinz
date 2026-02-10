/*
 * pcap_check.c - Lightweight pcap parser for WPA handshake validation
 *
 * Phase 5: Enhanced with AngryOxide-style validation:
 *   - Nonce correction: M1.ANonce[0..28] must match M3.ANonce[0..28]
 *   - Replay counter: M2.RC within [M1.RC, M1.RC+3], etc.
 *   - Temporal: consecutive messages within 250ms
 *
 * Phase 6: Rolling match — finds first valid M1+M2 pair by replay counter
 *   matching during parse, then locks it in. Prevents later frames from
 *   different exchanges overwriting a good pair. Same for M3/M4.
 *   Fixes false PARTIAL on pcaps with hundreds of interleaved exchanges.
 *
 * Classifies pcap files into:
 *   FHS (result=2): Crackable + validated - has valid M1+M2 pair, or PMKID
 *   PHS (result=1): Partial - has EAPOL frames but not validated crackable
 *   Nothing (result=0): No EAPOL key frames found
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include "pcap_check.h"

/* PCAP file header (24 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t  thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t linktype;
} pcap_hdr_t;

/* PCAP packet header (16 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
} pcap_pkt_hdr_t;

/* Radiotap header (variable length) */
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  pad;
    uint16_t len;
    uint32_t present;
} radiotap_hdr_t;

/* IEEE 802.11 frame control */
typedef struct __attribute__((packed)) {
    uint16_t frame_control;
    uint16_t duration;
    uint8_t  addr1[6];
    uint8_t  addr2[6];
    uint8_t  addr3[6];
    uint16_t seq_ctrl;
} ieee80211_hdr_t;

/* 802.2 LLC header */
typedef struct __attribute__((packed)) {
    uint8_t  dsap;
    uint8_t  ssap;
    uint8_t  ctrl;
    uint8_t  oui[3];
    uint16_t type;
} llc_hdr_t;

/* EAPOL header */
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  type;
    uint16_t length;
} eapol_hdr_t;

/* WPA/RSN key descriptor */
typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint16_t key_info;
    uint16_t key_len;
    uint8_t  replay_counter[8];
    uint8_t  nonce[32];
    uint8_t  key_iv[16];
    uint8_t  key_rsc[8];
    uint8_t  key_id[8];
    uint8_t  key_mic[16];
    uint16_t key_data_len;
} wpa_key_t;

/* Extract 64-bit replay counter from 8-byte big-endian field */
static uint64_t extract_replay_counter(const wpa_key_t *key) {
    uint64_t rc = 0;
    for (int i = 0; i < 8; i++)
        rc = (rc << 8) | key->replay_counter[i];
    return rc;
}

/* Calculate time delta in milliseconds between two pcap timestamps.
 * Returns negative if t2 is before t1. */
static int64_t ts_delta_ms(uint32_t t1_sec, uint32_t t1_usec,
                           uint32_t t2_sec, uint32_t t2_usec) {
    int64_t sec_diff  = (int64_t)t2_sec - (int64_t)t1_sec;
    int64_t usec_diff = (int64_t)t2_usec - (int64_t)t1_usec;
    return sec_diff * 1000 + usec_diff / 1000;
}

/* RSN PMKID KDE: DD <len> 00:0F:AC:04 <16-byte PMKID>
 * Look for OUI 00-0F-AC with data type 04 in key data */
static bool check_pmkid_in_key_data(const uint8_t *key_data, uint16_t key_data_len) {
    if (key_data_len < 22) return false;

    const uint8_t *ptr = key_data;
    const uint8_t *end = key_data + key_data_len;

    while (ptr + 2 < end) {
        uint8_t tag = ptr[0];
        uint8_t len = ptr[1];

        if (ptr + 2 + len > end) break;

        if (tag == 0xDD && len >= 20) {
            if (ptr[2] == 0x00 && ptr[3] == 0x0F && ptr[4] == 0xAC && ptr[5] == 0x04) {
                const uint8_t *pmkid = ptr + 6;
                for (int i = 0; i < 16; i++) {
                    if (pmkid[i] != 0) return true;
                }
            }
        }

        ptr += 2 + len;
    }

    return false;
}

/* Classify EAPOL message based on key_info flags.
 *
 * IEEE 802.11i key_info bit meanings:
 *   ACK     (0x0080): Set by AP (authenticator) in M1, M3
 *   MIC     (0x0100): Set when MIC is present (M2, M3, M4)
 *   Install (0x0040): Set in M3 (install PTK)
 *   Secure  (0x0200): Set in M3, M4 (keys installed)
 *
 * Disambiguation:
 *   M1: ACK=1, MIC=0
 *   M2: ACK=0, MIC=1, Secure=0   (STA sends SNonce)
 *   M3: ACK=1, MIC=1, Install=1  (AP sends GTK)
 *   M4: ACK=0, MIC=1, Secure=1   (STA confirms)
 *
 * CRITICAL: M4 must be checked before M2, as both have ACK=0 MIC=1,
 * but M4 has Secure=1 while M2 has Secure=0.
 */
static int classify_eapol_message(uint16_t key_info) {
    int ack     = (key_info & WPA_KEY_INFO_ACK) != 0;
    int mic     = (key_info & WPA_KEY_INFO_MIC) != 0;
    int install = (key_info & WPA_KEY_INFO_INSTALL) != 0;
    int secure  = (key_info & WPA_KEY_INFO_SECURE) != 0;

    if (ack && !mic) return 1;
    if (ack && mic && install) return 3;
    if (!ack && mic && secure) return 4;
    if (!ack && mic && !secure) return 2;

    return 0;
}

/* Check if MIC field is all zeros */
static bool mic_is_zero(const wpa_key_t *key) {
    for (int i = 0; i < 16; i++) {
        if (key->key_mic[i] != 0) return false;
    }
    return true;
}

/* Check if nonce field is all zeros */
static bool nonce_is_zero(const uint8_t *nonce) {
    for (int i = 0; i < 32; i++) {
        if (nonce[i] != 0) return false;
    }
    return true;
}

/* Parse a single packet for EAPOL data.
 * Now stores per-message nonce, replay counter, and timestamp for validation. */
static void parse_packet(const uint8_t *data, uint32_t len, int linktype,
                         handshake_info_t *info, bool swapped,
                         uint32_t ts_sec, uint32_t ts_usec) {
    const uint8_t *ptr = data;
    const uint8_t *end = data + len;

    /* Skip link layer header based on type */
    if (linktype == DLT_IEEE802_11_RADIO) {
        if (len < sizeof(radiotap_hdr_t)) return;
        const radiotap_hdr_t *rt = (const radiotap_hdr_t *)ptr;
        uint16_t rt_len = swapped ? ntohs(rt->len) : rt->len;
        if (rt_len > len) return;
        ptr += rt_len;
    }

    if (linktype == DLT_IEEE802_11 || linktype == DLT_IEEE802_11_RADIO) {
        if (ptr + sizeof(ieee80211_hdr_t) > end) return;
        const ieee80211_hdr_t *wifi = (const ieee80211_hdr_t *)ptr;

        uint16_t fc = wifi->frame_control;
        if (swapped) fc = ntohs(fc);
        int type = (fc >> 2) & 0x03;
        int subtype = (fc >> 4) & 0x0f;

        if (type != 2) return;  /* Not a data frame */

        int hdr_size = 24;
        if (subtype >= 8) hdr_size += 2;  /* QoS data */

        int to_ds = fc & 0x0100;
        int from_ds = fc & 0x0200;
        if (to_ds && from_ds) hdr_size += 6;

        ptr += hdr_size;

        if (ptr + sizeof(llc_hdr_t) > end) return;
        const llc_hdr_t *llc = (const llc_hdr_t *)ptr;

        if (llc->dsap != 0xaa || llc->ssap != 0xaa) return;
        if (ntohs(llc->type) != ETHERTYPE_EAPOL) return;

        ptr += sizeof(llc_hdr_t);
    } else if (linktype == DLT_EN10MB) {
        if (len < 14) return;
        uint16_t ethertype = (ptr[12] << 8) | ptr[13];
        if (ethertype != ETHERTYPE_EAPOL) return;
        ptr += 14;
    } else {
        return;
    }

    /* Parse EAPOL header */
    if (ptr + sizeof(eapol_hdr_t) > end) return;
    const eapol_hdr_t *eapol = (const eapol_hdr_t *)ptr;

    if (eapol->type != EAPOL_KEY) return;
    ptr += sizeof(eapol_hdr_t);

    /* Parse WPA key descriptor */
    if (ptr + sizeof(wpa_key_t) > end) return;
    const wpa_key_t *key = (const wpa_key_t *)ptr;

    uint16_t key_info = ntohs(key->key_info);
    uint16_t key_data_len = ntohs(key->key_data_len);

    info->eapol_count++;

    int msg = classify_eapol_message(key_info);
    switch (msg) {
        case 1:
            /* M1: AP sends ANonce. Should have no MIC, must have nonce. */
            if (!mic_is_zero(key)) break;  /* Invalid M1: MIC should not be present */
            if (nonce_is_zero(key->nonce)) break;  /* Invalid M1: ANonce required */

            /* Rolling match: don't overwrite a locked M1+M2 pair */
            if (!info->m1_m2_locked) {
                info->has_m1 = true;
                memcpy(info->m1_anonce, key->nonce, 32);
                info->m1_replay = extract_replay_counter(key);
                info->m1_ts_sec = ts_sec;
                info->m1_ts_usec = ts_usec;
            }

            /* Always check for PMKID regardless of lock state */
            if (key_data_len > 0) {
                const uint8_t *key_data = ptr + sizeof(wpa_key_t);
                if (key_data + key_data_len <= end) {
                    if (check_pmkid_in_key_data(key_data, key_data_len)) {
                        info->has_pmkid = true;
                    }
                }
            }
            break;

        case 2:
            /* M2: STA sends SNonce + MIC. Must have MIC and SNonce. */
            if (info->m1_m2_locked) break;  /* Already found a valid pair */
            if (mic_is_zero(key)) break;  /* Invalid M2: MIC required */
            if (nonce_is_zero(key->nonce)) break;  /* Invalid M2: SNonce required */

            {
                uint64_t rc = extract_replay_counter(key);

                /* Check if this M2 matches the current candidate M1 */
                if (info->has_m1 &&
                    rc >= info->m1_replay &&
                    rc <= info->m1_replay + 3) {
                    /* Valid M1+M2 pair from same exchange — lock it in */
                    info->has_m2 = true;
                    memcpy(info->m2_snonce, key->nonce, 32);
                    info->m2_replay = rc;
                    info->m2_ts_sec = ts_sec;
                    info->m2_ts_usec = ts_usec;
                    info->m1_m2_locked = true;
                } else if (!info->has_m2) {
                    /* No match yet — store as fallback */
                    info->has_m2 = true;
                    memcpy(info->m2_snonce, key->nonce, 32);
                    info->m2_replay = rc;
                    info->m2_ts_sec = ts_sec;
                    info->m2_ts_usec = ts_usec;
                }
            }
            break;

        case 3:
            /* M3: AP sends ANonce + GTK. Must have MIC and nonce. */
            if (mic_is_zero(key)) break;  /* Invalid M3: MIC required */
            if (nonce_is_zero(key->nonce)) break;  /* Invalid M3: ANonce required */

            /* If M1+M2 locked, only accept M3 with matching ANonce */
            if (info->m1_m2_locked && !info->m3_locked) {
                if (memcmp(info->m1_anonce, key->nonce, 28) == 0) {
                    /* ANonce matches locked M1 — same exchange */
                    info->has_m3 = true;
                    memcpy(info->m3_anonce, key->nonce, 32);
                    info->m3_replay = extract_replay_counter(key);
                    info->m3_ts_sec = ts_sec;
                    info->m3_ts_usec = ts_usec;
                    info->m3_locked = true;
                }
                /* else: different exchange M3, skip */
            } else if (!info->m1_m2_locked) {
                /* No lock yet — store any M3 as candidate */
                info->has_m3 = true;
                memcpy(info->m3_anonce, key->nonce, 32);
                info->m3_replay = extract_replay_counter(key);
                info->m3_ts_sec = ts_sec;
                info->m3_ts_usec = ts_usec;
            }
            break;

        case 4:
            /* M4: STA confirms. Must have MIC. */
            if (mic_is_zero(key)) break;  /* Invalid M4: MIC required */

            {
                uint64_t rc = extract_replay_counter(key);

                /* If M3 is locked, only accept M4 with matching RC */
                if (info->m3_locked) {
                    if (rc >= info->m3_replay &&
                        rc <= info->m3_replay + 3) {
                        info->has_m4 = true;
                        info->m4_replay = rc;
                        info->m4_ts_sec = ts_sec;
                        info->m4_ts_usec = ts_usec;
                    }
                } else {
                    /* No lock — store any M4 */
                    info->has_m4 = true;
                    info->m4_replay = rc;
                    info->m4_ts_sec = ts_sec;
                    info->m4_ts_usec = ts_usec;
                }
            }
            break;
    }
}

/* Temporal validation threshold in milliseconds.
 * AngryOxide uses 200ms for live capture. We use 250ms for offline pcap
 * analysis to allow for minor timing jitter in captures. */
#define TEMPORAL_THRESHOLD_MS 250

/* Validate handshake quality using AngryOxide-style checks.
 *
 * 1. Replay Counter: M2.RC within [M1.RC, M1.RC+3] (same exchange check)
 *    If M1+M2 replay counters don't match, they're from different exchanges
 *    and the capture is NOT crackable (wrong ANonce/SNonce pair).
 *
 * 2. Nonce Correction: M1.ANonce[0..28] == M3.ANonce[0..28]
 *    If first 28 bytes match but last 4 differ, hashcat needs the NC flag.
 *    If first 28 bytes DON'T match, M1 and M3 are from different exchanges.
 *
 * 3. Temporal: Consecutive messages within TEMPORAL_THRESHOLD_MS.
 *    Informational — doesn't affect crackability (hashcat ignores timing)
 *    but indicates capture quality.
 */
static void validate_handshake(handshake_info_t *info) {
    /* Default to valid — each check can invalidate */
    info->nonce_valid = true;
    info->nonce_correction = false;
    info->replay_valid = true;
    info->temporal_valid = true;

    /* === Replay Counter Validation === */

    /* M2.RC should be within [M1.RC, M1.RC+3] */
    if (info->has_m1 && info->has_m2) {
        if (info->m2_replay < info->m1_replay ||
            info->m2_replay > info->m1_replay + 3) {
            info->replay_valid = false;
        }
    }

    /* M3.RC should be >= M2.RC (AP increments RC for M3) */
    if (info->has_m2 && info->has_m3) {
        if (info->m3_replay < info->m2_replay ||
            info->m3_replay > info->m2_replay + 3) {
            info->replay_valid = false;
        }
    }

    /* M4.RC should be within [M3.RC, M3.RC+3] */
    if (info->has_m3 && info->has_m4) {
        if (info->m4_replay < info->m3_replay ||
            info->m4_replay > info->m3_replay + 3) {
            info->replay_valid = false;
        }
    } else if (info->has_m2 && info->has_m4) {
        /* Fallback: validate M4 against M2 if no M3 */
        if (info->m4_replay < info->m2_replay ||
            info->m4_replay > info->m2_replay + 3) {
            info->replay_valid = false;
        }
    }

    /* === Nonce Correction Validation === */

    /* Compare M1.ANonce with M3.ANonce (AngryOxide auth.rs:444-462) */
    if (info->has_m1 && info->has_m3) {
        if (memcmp(info->m1_anonce, info->m3_anonce, 28) != 0) {
            /* First 28 bytes don't match — different exchanges */
            info->nonce_valid = false;
        } else if (memcmp(info->m1_anonce + 28, info->m3_anonce + 28, 4) != 0) {
            /* First 28 match, last 4 differ — nonce correction needed */
            info->nonce_correction = true;
            /* Still valid! Just needs NC flag for hashcat */
        }
        /* else: all 32 bytes match — no NC needed, valid */
    }

    /* === Temporal Validation === */

    if (info->has_m1 && info->has_m2) {
        int64_t delta = ts_delta_ms(info->m1_ts_sec, info->m1_ts_usec,
                                     info->m2_ts_sec, info->m2_ts_usec);
        if (delta < 0 || delta > TEMPORAL_THRESHOLD_MS) {
            info->temporal_valid = false;
        }
    }

    if (info->has_m2 && info->has_m3) {
        int64_t delta = ts_delta_ms(info->m2_ts_sec, info->m2_ts_usec,
                                     info->m3_ts_sec, info->m3_ts_usec);
        if (delta < 0 || delta > TEMPORAL_THRESHOLD_MS) {
            info->temporal_valid = false;
        }
    }

    if (info->has_m3 && info->has_m4) {
        int64_t delta = ts_delta_ms(info->m3_ts_sec, info->m3_ts_usec,
                                     info->m4_ts_sec, info->m4_ts_usec);
        if (delta < 0 || delta > TEMPORAL_THRESHOLD_MS) {
            info->temporal_valid = false;
        }
    }

    /* === Overall Validation === */
    /* Crackability depends on replay counter + nonce match only.
     * Temporal is a quality indicator — does NOT affect crackability.
     * Hashcat doesn't care if M1→M2 was 50ms or 5 seconds apart. */
    info->validated = info->nonce_valid && info->replay_valid;

    /* === Adjust crackability based on validation === */

    /* PMKID is always crackable regardless of 4-way validation */
    if (info->has_pmkid) {
        info->is_crackable = true;
        /* Keep validated status for the 4-way part */
    }

    /* M1+M2 with invalid replay counter = different exchanges = NOT crackable */
    if (info->has_m1 && info->has_m2 && !info->replay_valid && !info->has_pmkid) {
        info->is_crackable = false;
        info->is_full = false;
    }

    /* M1+M3 nonce mismatch = different exchanges = downgrade is_full */
    if (info->has_m1 && info->has_m3 && !info->nonce_valid) {
        info->is_full = false;
    }
}

int pcap_check_handshake(const char *filepath, handshake_info_t *info) {
    FILE *fp = fopen(filepath, "rb");
    if (!fp) return -1;

    memset(info, 0, sizeof(*info));

    pcap_hdr_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, fp) != 1) {
        fclose(fp);
        return -1;
    }

    bool swapped = false;
    switch (hdr.magic) {
        case PCAP_MAGIC_US:
        case PCAP_MAGIC_NS:
            break;
        case PCAP_MAGIC_US_SWAP:
        case PCAP_MAGIC_NS_SWAP:
            swapped = true;
            break;
        default:
            fclose(fp);
            return -1;
    }

    uint32_t linktype = swapped ? ntohl(hdr.linktype) : hdr.linktype;

    uint8_t *pkt_buf = malloc(65536);
    if (!pkt_buf) {
        fclose(fp);
        return -1;
    }

    pcap_pkt_hdr_t pkt_hdr;
    while (fread(&pkt_hdr, sizeof(pkt_hdr), 1, fp) == 1) {
        uint32_t pkt_len = swapped ? ntohl(pkt_hdr.incl_len) : pkt_hdr.incl_len;
        uint32_t ts_sec  = swapped ? ntohl(pkt_hdr.ts_sec)   : pkt_hdr.ts_sec;
        uint32_t ts_usec = swapped ? ntohl(pkt_hdr.ts_usec)  : pkt_hdr.ts_usec;

        if (pkt_len > 65536) break;

        if (fread(pkt_buf, pkt_len, 1, fp) != 1) break;

        parse_packet(pkt_buf, pkt_len, linktype, info, swapped, ts_sec, ts_usec);
    }

    free(pkt_buf);
    fclose(fp);

    /* Basic crackability (before validation) */
    info->is_crackable = info->has_pmkid ||
                         (info->has_m1 && info->has_m2) ||
                         (info->has_m2 && info->has_m3);
    info->is_full = info->has_m1 && info->has_m2 &&
                    info->has_m3 && info->has_m4;

    /* Run AngryOxide-style validation — may downgrade crackability */
    validate_handshake(info);

    /* Result mapping:
     * 2 = Validated crackable (PMKID, or valid M1+M2 pair from same exchange)
     * 1 = Partial (has EAPOL but not validated as crackable)
     * 0 = Nothing useful */
    if (info->is_crackable) return 2;
    if (info->eapol_count > 0) return 1;
    return 0;
}
