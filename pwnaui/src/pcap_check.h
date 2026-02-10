#ifndef PCAP_CHECK_H
#define PCAP_CHECK_H

#include <stdint.h>
#include <stdbool.h>

/* PCAP file magic numbers */
#define PCAP_MAGIC_US      0xa1b2c3d4
#define PCAP_MAGIC_NS      0xa1b23c4d
#define PCAP_MAGIC_US_SWAP 0xd4c3b2a1
#define PCAP_MAGIC_NS_SWAP 0x4d3cb2a1

/* Link layer types */
#define DLT_EN10MB         1
#define DLT_IEEE802_11     105
#define DLT_IEEE802_11_RADIO 127

/* Ethernet types */
#define ETHERTYPE_EAPOL    0x888e

/* EAPOL types */
#define EAPOL_KEY          3

/* WPA key info flags */
#define WPA_KEY_INFO_TYPE_MASK  0x0007
#define WPA_KEY_INFO_INSTALL    0x0040
#define WPA_KEY_INFO_ACK        0x0080
#define WPA_KEY_INFO_MIC        0x0100
#define WPA_KEY_INFO_SECURE     0x0200

/* Handshake validation result */
typedef struct {
    /* Basic detection */
    int eapol_count;      /* Total EAPOL key frames found */
    bool has_m1;          /* AP -> STA: ANonce, ACK, no MIC */
    bool has_m2;          /* STA -> AP: SNonce, MIC, no Secure */
    bool has_m3;          /* AP -> STA: Install, MIC, ACK, Secure */
    bool has_m4;          /* STA -> AP: MIC, Secure, no ACK */
    bool has_pmkid;       /* M1 key data contains valid PMKID */
    bool is_crackable;    /* Has enough for cracking: M1+M2, M2+M3, or PMKID */
    bool is_full;         /* Has all 4 messages from same exchange */

    /* Rolling match state — locks in first valid pair found */
    bool m1_m2_locked;    /* true once M1+M2 with matching RC found */
    bool m3_locked;       /* true once M3 with matching ANonce found */

    /* Per-message data for validation (Phase 5) */
    uint8_t  m1_anonce[32];     /* ANonce from M1 */
    uint64_t m1_replay;         /* Replay counter from M1 */
    uint32_t m1_ts_sec;         /* Pcap timestamp seconds */
    uint32_t m1_ts_usec;        /* Pcap timestamp microseconds */

    uint8_t  m2_snonce[32];     /* SNonce from M2 */
    uint64_t m2_replay;         /* Replay counter from M2 */
    uint32_t m2_ts_sec;
    uint32_t m2_ts_usec;

    uint8_t  m3_anonce[32];     /* ANonce from M3 */
    uint64_t m3_replay;         /* Replay counter from M3 */
    uint32_t m3_ts_sec;
    uint32_t m3_ts_usec;

    uint64_t m4_replay;         /* Replay counter from M4 */
    uint32_t m4_ts_sec;
    uint32_t m4_ts_usec;

    /* Validation results (Phase 5: AngryOxide-style) */
    bool nonce_valid;       /* M1.ANonce[0..28] matches M3.ANonce[0..28] */
    bool nonce_correction;  /* Last 4 bytes differ (NC bit for hashcat) */
    bool replay_valid;      /* All replay counters in valid ranges */
    bool temporal_valid;    /* Consecutive messages within 250ms threshold */
    bool validated;         /* All applicable checks pass */
} handshake_info_t;

/* Check a pcap file for WPA handshake with validation
 * Returns: 0 = no usable handshake data
 *          1 = partial (has EAPOL but not crackable/validated)
 *          2 = crackable and validated (M1+M2 same exchange, or PMKID) */
int pcap_check_handshake(const char *filepath, handshake_info_t *info);

#endif /* PCAP_CHECK_H */
