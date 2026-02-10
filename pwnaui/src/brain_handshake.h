/* brain_handshake.h - Handshake quality management
 * Sprint 7 #21: Extracted from brain.c for modularity
 */
#ifndef BRAIN_HANDSHAKE_H
#define BRAIN_HANDSHAKE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "pcap_check.h"

/* Handshake quality levels */
typedef enum {
    HS_QUALITY_NONE = 0,      /* No EAPOL packets */
    HS_QUALITY_PARTIAL,       /* Missing M1/M2/M3/M4 (incomplete 4-way) */
    HS_QUALITY_PMKID,         /* PMKID only (no full handshake) */
    HS_QUALITY_FULL           /* Complete 4-way handshake (M1+M2+M3+M4) */
} hs_quality_t;

extern const char *hs_quality_names[];

/* Cached handshake info */
typedef struct {
    char bssid[18];              /* AP MAC address */
    char ssid[64];               /* Network name */
    char pcap_path[256];         /* Path to pcap file */
    hs_quality_t quality;        /* Handshake completeness */
    time_t analyzed_at;          /* When we last analyzed */
} hs_info_t;

#define HS_CACHE_MAX 256
#define HS_CACHE_TTL 300  /* Re-analyze every 5 minutes */

/* Public API */
long total_handshake_bytes(void);
hs_quality_t analyze_pcap_quality(const char *pcap_path, handshake_info_t *info_out);
int extract_bssid_from_filename(const char *filename, char *bssid_out, char *ssid_out);
void scan_handshake_stats(void);
hs_quality_t get_handshake_quality(const char *bssid);
bool has_full_handshake(const char *bssid);
bool brain_has_full_handshake(const char *bssid);
int count_full_handshakes(void);
const char *get_hs_pcap_path(const char *bssid);

#endif /* BRAIN_HANDSHAKE_H */
