/* brain_handshake.c — Handshake quality management
 * Sprint 7 #21: Extracted from brain.c for modularity
 * Contains: pcap quality analysis, handshake cache, validation helpers
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* strcasecmp */
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

#include "brain.h"
#include "brain_handshake.h"
#include "pcap_check.h"
#include "pcapng_gps.h"
#include "hc22000.h"


const char *hs_quality_names[] = {
    [HS_QUALITY_NONE]    = "NONE",
    [HS_QUALITY_PARTIAL] = "PARTIAL",
    [HS_QUALITY_PMKID]   = "PMKID",
    [HS_QUALITY_FULL]    = "FULL"
};


hs_info_t hs_cache[HS_CACHE_MAX];
int hs_cache_count = 0;
static time_t hs_cache_last_scan = 0;
/* ============================================================================
 * Utility Functions
 * ========================================================================== */

/* Sum total bytes of all .pcap files in handshakes directory.
 * Detects new handshakes even when bettercap appends to existing files. */
long total_handshake_bytes(void) {
    long total = 0;
    const char *hs_dir = "/home/pi/handshakes";
    DIR *dir = opendir(hs_dir);
    if (!dir) {
        hs_dir = "/var/lib/pwnagotchi/handshakes";
        dir = opendir(hs_dir);
    }
    if (!dir) return 0;

    struct dirent *ent;
    char path[512];
    struct stat st;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (len > 5 && strcmp(name + len - 5, ".pcap") == 0) {
            snprintf(path, sizeof(path), "%s/%s", hs_dir, name);
            if (stat(path, &st) == 0) {
                total += st.st_size;
            }
        }
    }
    closedir(dir);
    return total;
}


/* Analyze a pcap file using native pcap_check to determine handshake quality */
hs_quality_t analyze_pcap_quality(const char *pcap_path, handshake_info_t *info_out) {
    /* Use native pcap_check parser with AngryOxide-style validation */
    handshake_info_t _info = {0};
    handshake_info_t *info = info_out ? info_out : &_info;
    memset(info, 0, sizeof(*info));
    int result = pcap_check_handshake(pcap_path, info);

    /* result=2 means validated crackable (replay counters + nonce check passed) */
    if (info->is_full && info->validated) {
        return HS_QUALITY_FULL;  /* Complete 4-way, all validations pass */
    }
    if (result == 2 && info->is_crackable && info->validated) {
        return HS_QUALITY_FULL;  /* Crackable pair from same exchange */
    }
    if (info->has_pmkid) {
        return HS_QUALITY_PMKID;  /* PMKID always valid regardless of 4-way */
    }
    if (info->is_crackable && !info->validated) {
        /* Has M1+M2 but validation failed (different exchanges) — keep attacking */
        return HS_QUALITY_PARTIAL;
    }
    if (info->eapol_count > 0) {
        return HS_QUALITY_PARTIAL;
    }
    return HS_QUALITY_NONE;
}

/* Extract BSSID from pcap filename (format: SSID_BSSID.pcap) */
int extract_bssid_from_filename(const char *filename, char *bssid_out, char *ssid_out) {
    /* Bettercap format: NetworkName_aabbccddeeff.pcap (12 hex, no separators)
     * Also handles:     NetworkName_AA-BB-CC-DD-EE-FF.pcap (17 chars with dashes) */
    const char *underscore = strrchr(filename, '_');
    if (!underscore) return -1;

    const char *dot = strstr(underscore, ".pcap");
    if (!dot) return -1;

    /* Extract SSID (everything before last underscore) */
    if (ssid_out) {
        size_t ssid_len = underscore - filename;
        if (ssid_len > 63) ssid_len = 63;
        strncpy(ssid_out, filename, ssid_len);
        ssid_out[ssid_len] = '\0';
    }

    /* Extract BSSID — handle both compact (12 hex) and dashed (17 char) formats */
    if (bssid_out) {
        const char *bssid_start = underscore + 1;
        size_t bssid_len = dot - bssid_start;

        if (bssid_len == 12) {
            /* Compact format: aabbccddeeff -> aa:bb:cc:dd:ee:ff */
            for (int i = 0; i < 6; i++) {
                bssid_out[i * 3]     = bssid_start[i * 2];
                bssid_out[i * 3 + 1] = bssid_start[i * 2 + 1];
                if (i < 5) bssid_out[i * 3 + 2] = ':';
            }
            bssid_out[17] = '\0';
        } else if (bssid_len == 17) {
            /* Dashed format: AA-BB-CC-DD-EE-FF -> AA:BB:CC:DD:EE:FF */
            for (int i = 0; i < 17; i++) {
                bssid_out[i] = (bssid_start[i] == '-') ? ':' : bssid_start[i];
            }
            bssid_out[17] = '\0';
        } else {
            return -1;  /* Unknown format */
        }
    }

    return 0;
}
/* Scan handshakes directory and analyze quality of each pcap */
void scan_handshake_stats(void) {
    time_t now = time(NULL);
    
    /* Don't rescan too frequently */
    if ((now - hs_cache_last_scan) < HS_CACHE_TTL && hs_cache_count > 0) {
        return;
    }
    
    const char *hs_dir = "/home/pi/handshakes";
    DIR *dir = opendir(hs_dir);
    if (!dir) {
        hs_dir = "/var/lib/pwnagotchi/handshakes";
        dir = opendir(hs_dir);
    }
    if (!dir) {
        fprintf(stderr, "[brain] cannot open handshakes directory\n");
        return;
    }
    
    fprintf(stderr, "[brain] scanning handshakes for quality analysis...\n");
    hs_cache_count = 0;
    
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && hs_cache_count < HS_CACHE_MAX) {
        const char *name = ent->d_name;
        size_t len = strlen(name);
        
        /* Only process .pcap files */
        if (len < 6 || strcmp(name + len - 5, ".pcap") != 0) {
            continue;
        }
        
        hs_info_t *info = &hs_cache[hs_cache_count];
        
        /* Extract BSSID and SSID from filename */
        if (extract_bssid_from_filename(name, info->bssid, info->ssid) != 0) {
            continue;  /* Skip files that don't match expected format */
        }
        
        /* Build full path */
        snprintf(info->pcap_path, sizeof(info->pcap_path), "%s/%s", hs_dir, name);
        
        /* Analyze pcap quality */
        handshake_info_t vi = {0};
        info->quality = analyze_pcap_quality(info->pcap_path, &vi);
        info->analyzed_at = now;
        
        /* Log with validation details */
        fprintf(stderr, "[brain] %s: %s (%s) [V:%s RC:%s T:%s N:%s%s]\n",
                info->ssid, hs_quality_names[info->quality], info->bssid,
                vi.validated ? "OK" : "FAIL",
                vi.replay_valid ? "ok" : "BAD",
                vi.temporal_valid ? "ok" : "late",
                vi.nonce_valid ? "ok" : "BAD",
                vi.nonce_correction ? "+NC" : "");
        hs_cache_count++;
    }
    
    closedir(dir);
    hs_cache_last_scan = now;
    
    fprintf(stderr, "[brain] analyzed %d handshakes\n", hs_cache_count);

    /* Convert legacy .pcap files to .pcapng with GPS coordinates */
    int converted = pcapng_convert_directory(hs_dir);

    /* Sprint 3 #6: Auto-convert to hc22000 format for hashcat */
    int hc_hashes = hc22000_convert_directory(hs_dir);
    if (hc_hashes > 0) {
        fprintf(stderr, "[brain] hc22000: %d hash(es) ready for hashcat\n", hc_hashes);
    }
    if (converted > 0) {
        fprintf(stderr, "[brain] converted %d pcap -> pcapng with GPS\n", converted);
    }
}

/* Get handshake quality for a specific BSSID */
hs_quality_t get_handshake_quality(const char *bssid) {
    for (int i = 0; i < hs_cache_count; i++) {
        if (strcasecmp(hs_cache[i].bssid, bssid) == 0) {
            return hs_cache[i].quality;
        }
    }
    return HS_QUALITY_NONE;  /* No handshake captured yet */
}

/* Check if we have a FULL handshake for a given BSSID (internal) */
bool has_full_handshake(const char *bssid) {
    return get_handshake_quality(bssid) == HS_QUALITY_FULL;
}

/* Public API: check if we have a FULL or PMKID handshake for a BSSID.
 * Uses local pcap cache (not bettercap's session-only flag).
 * Called by pwnaui.c insta-attack paths to avoid re-attacking captured APs. */
bool brain_has_full_handshake(const char *bssid) {
    hs_quality_t q = get_handshake_quality(bssid);
    return (q == HS_QUALITY_FULL || q == HS_QUALITY_PMKID);
}

/* Get count of FULL handshakes (for TAPS display = pcap files with usable handshakes) */
int count_full_handshakes(void) {
    int count = 0;
    for (int i = 0; i < hs_cache_count; i++) {
        if (hs_cache[i].quality == HS_QUALITY_FULL || 
            hs_cache[i].quality == HS_QUALITY_PMKID) {
            count++;
        }
    }
    return count;
}

/* Get pcap path for a BSSID from handshake cache (for GPS refinement) */
const char *get_hs_pcap_path(const char *bssid) {
    for (int i = 0; i < hs_cache_count; i++) {
        if (strcasecmp(hs_cache[i].bssid, bssid) == 0 && hs_cache[i].pcap_path[0]) {
            return hs_cache[i].pcap_path;
        }
    }
    return NULL;
}


