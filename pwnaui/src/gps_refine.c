/**
 * gps_refine.c - GPS Refinement System
 *
 * When the brain sees an AP it already captured a handshake for,
 * we compare the current RSSI (signal strength in dBm). Higher dBm
 * means we're physically closer to the AP, so our GPS coordinates
 * better represent where the AP actually sits.
 *
 * On update, we write the RSSI into the .gps.json file so future
 * passes only overwrite if we get even closer.
 */

#include "gps_refine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* strcasecmp */
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <ctype.h>

/* Rate limiting: one check per AP per 5 minutes */
#define REFINE_COOLDOWN_SECS 300
#define REFINE_CACHE_MAX     256
#define REFINE_MIN_SATS      4    /* Minimum satellites for trustworthy fix */

typedef struct {
    char bssid[18];           /* "aa:bb:cc:dd:ee:ff" */
    time_t last_checked;      /* Cooldown timer */
    int8_t best_rssi;         /* Strongest signal seen this session */
} refine_entry_t;

static refine_entry_t refine_cache[REFINE_CACHE_MAX];
static int refine_cache_count = 0;
static int refine_total_updates = 0;

/* ======================================================================== */

void gps_refine_init(void) {
    memset(refine_cache, 0, sizeof(refine_cache));
    refine_cache_count = 0;
    refine_total_updates = 0;
    fprintf(stderr, "[gps-refine] GPS refinement system initialized\n");
}

int gps_refine_count(void) {
    return refine_total_updates;
}

/* Find or create cache entry for a BSSID */
static refine_entry_t *get_refine_entry(const char *bssid) {
    for (int i = 0; i < refine_cache_count; i++) {
        if (strcasecmp(refine_cache[i].bssid, bssid) == 0)
            return &refine_cache[i];
    }
    if (refine_cache_count >= REFINE_CACHE_MAX)
        return NULL;

    refine_entry_t *e = &refine_cache[refine_cache_count++];
    strncpy(e->bssid, bssid, sizeof(e->bssid) - 1);
    e->bssid[sizeof(e->bssid) - 1] = '\0';
    e->last_checked = 0;
    e->best_rssi = -127;  /* Worst possible = always accepts first update */
    return e;
}

/* Derive .gps.json path from .pcap path
 * "/home/pi/handshakes/Shane_1e8a7da704d3.pcap" →
 * "/home/pi/handshakes/Shane_1e8a7da704d3.gps.json" */
static bool derive_gps_path(const char *pcap_path, char *out, size_t out_size) {
    if (!pcap_path || !pcap_path[0]) return false;

    const char *ext = strstr(pcap_path, ".pcap");
    if (!ext) return false;

    size_t base_len = (size_t)(ext - pcap_path);
    if (base_len + 10 >= out_size) return false;  /* ".gps.json\0" */

    memcpy(out, pcap_path, base_len);
    strcpy(out + base_len, ".gps.json");
    return true;
}

/* Read stored RSSI from an existing .gps.json file.
 * Returns -127 if no RSSI field (original bettercap file, never refined). */
static int8_t read_stored_rssi(const char *json_path) {
    FILE *f = fopen(json_path, "r");
    if (!f) return -127;

    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    /* Look for "RSSI": -XX */
    char *p = strstr(buf, "\"RSSI\"");
    if (!p) return -127;

    p = strchr(p, ':');
    if (!p) return -127;
    p++;
    while (*p == ' ' || *p == '\t') p++;

    return (int8_t)atoi(p);
}

/* Write updated GPS JSON with RSSI tracking */
static bool write_gps_json(const char *json_path, double lat, double lon,
                            double alt, double hdop, int8_t rssi) {
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%S+0000", tm);

    /* Approximate accuracy from HDOP (HDOP * ~5m base GPS accuracy) */
    double accuracy = hdop > 0 ? hdop * 5.0 : 10.0;

    FILE *f = fopen(json_path, "w");
    if (!f) {
        fprintf(stderr, "[gps-refine] ERROR: cannot write %s: %s\n",
                json_path, strerror(errno));
        return false;
    }

    fprintf(f, "{\n"
               "  \"Latitude\": %.8f,\n"
               "  \"Longitude\": %.8f,\n"
               "  \"Altitude\": %.2f,\n"
               "  \"Accuracy\": %.2f,\n"
               "  \"Updated\": \"%s\",\n"
               "  \"RSSI\": %d,\n"
               "  \"RefinedBy\": \"pwnaui\"\n"
               "}\n",
               lat, lon, alt, accuracy, time_str, (int)rssi);

    fclose(f);
    return true;
}

/* ======================================================================== */

bool gps_refine_check(const char *bssid, int8_t rssi,
                      const gps_data_t *gps, const char *pcap_path) {
    /* Validate inputs */
    if (!bssid || !gps || !pcap_path || !pcap_path[0])
        return false;

    /* Must have good GPS fix with enough satellites */
    if (!gps->has_fix || gps->latitude == 0.0 || gps->longitude == 0.0)
        return false;
    if (gps->satellites < REFINE_MIN_SATS)
        return false;

    /* Rate limit: one check per AP per 5 minutes */
    refine_entry_t *entry = get_refine_entry(bssid);
    if (!entry) return false;

    time_t now = time(NULL);
    if (now - entry->last_checked < REFINE_COOLDOWN_SECS)
        return false;
    entry->last_checked = now;

    /* Derive .gps.json path from .pcap path */
    char gps_path[512];
    if (!derive_gps_path(pcap_path, gps_path, sizeof(gps_path)))
        return false;

    /* Check if GPS JSON file exists */
    struct stat st;
    if (stat(gps_path, &st) != 0) {
        /* No GPS file yet — create one if signal is decent (> -70 dBm) */
        if (rssi > -70) {
            if (write_gps_json(gps_path, gps->latitude, gps->longitude,
                               gps->altitude, gps->hdop, rssi)) {
                entry->best_rssi = rssi;
                refine_total_updates++;
                fprintf(stderr, "[gps-refine] NEW GPS for %s @ %ddBm (%.6f, %.6f)\n",
                        bssid, rssi, gps->latitude, gps->longitude);
                return true;
            }
        }
        return false;
    }

    /* Read stored RSSI from existing GPS file */
    int8_t stored_rssi = read_stored_rssi(gps_path);

    /* Only update if current signal is STRONGER (higher dBm = closer to AP) */
    if (rssi <= stored_rssi) {
        /* Not closer than previous best — sync cache and skip */
        if (entry->best_rssi < stored_rssi)
            entry->best_rssi = stored_rssi;
        return false;
    }

    /* We're closer to the AP! Update GPS coordinates */
    if (write_gps_json(gps_path, gps->latitude, gps->longitude,
                       gps->altitude, gps->hdop, rssi)) {
        fprintf(stderr, "[gps-refine] REFINED %s: %ddBm -> %ddBm (%.6f, %.6f)\n",
                bssid, (int)stored_rssi, (int)rssi,
                gps->latitude, gps->longitude);
        entry->best_rssi = rssi;
        refine_total_updates++;
        return true;
    }

    return false;
}
