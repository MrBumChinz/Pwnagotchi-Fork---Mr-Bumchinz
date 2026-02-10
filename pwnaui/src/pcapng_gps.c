/*
 * pcapng_gps.c - Convert pcap to pcapng with Kismet GPS custom options
 *
 * Implements the Kismet PCAPNG GPS standard for embedding GPS coordinates
 * directly into pcapng Enhanced Packet Block (EPB) custom options.
 *
 * References:
 *   https://www.kismetwireless.net/docs/dev/pcapng_gps/
 *   https://github.com/Ragnt/AngryOxide  (src/gps.rs, src/pcapng.rs)
 *
 * This produces pcapng files compatible with:
 *   - AngryOxide output format
 *   - Kismet pcapng tools
 *   - WiGLE upload
 *   - hcxpcapngtool / hashcat
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <arpa/inet.h>

#include "pcapng_gps.h"
#include "cJSON.h"

/* ========================================================================== */
/* PCAPNG Block Types                                                          */
/* ========================================================================== */
#define PCAPNG_SHB_TYPE       0x0A0D0D0A  /* Section Header Block */
#define PCAPNG_IDB_TYPE       0x00000001  /* Interface Description Block */
#define PCAPNG_EPB_TYPE       0x00000006  /* Enhanced Packet Block */
#define PCAPNG_BYTE_ORDER_MAGIC 0x1A2B3C4D

/* PCAPNG option codes */
#define PCAPNG_OPT_ENDOFOPT   0x0000
#define PCAPNG_OPT_COMMENT    0x0001
#define PCAPNG_OPT_CUSTOM_BIN 2989        /* Custom binary option (not copied) */

/* Interface Description Block options */
#define PCAPNG_IF_NAME        0x0002
#define PCAPNG_IF_TSRESOL     0x0009

/* Kismet GPS constants */
#define KISMET_PEN            55922
#define GPS_MAGIC             0x47
#define GPS_VERSION           0x01

/* GPS field presence bitmask */
#define GPS_FIELD_LON         0x00000002
#define GPS_FIELD_LAT         0x00000004
#define GPS_FIELD_ALT         0x00000008

/* Link type */
#define LINKTYPE_IEEE802_11_RADIOTAP 127

/* Legacy pcap header (24 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t  thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t linktype;
} pcap_file_hdr_t;

/* Legacy pcap packet header (16 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
} pcap_pkt_hdr_t;

/* ========================================================================== */
/* Fixed-point encoding (Kismet spec)                                          */
/* ========================================================================== */

/*
 * fixed3_7: Encode latitude/longitude (-180.0 to +180.0) as uint32_t
 * Maps [-180, +180] → [0, 3600000000]
 */
static uint32_t float_to_fixed3_7(double flt) {
    if (flt < -180.0) flt = -180.0;
    if (flt > 180.0)  flt = 180.0;
    int32_t scaled = (int32_t)(flt * 10000000.0);
    return (uint32_t)(scaled + (180 * 10000000));
}

/*
 * fixed6_4: Encode altitude (-180000.0 to +180000.0) as uint32_t
 * Maps [-180000, +180000] → [0, 3600000000]
 */
static uint32_t float_to_fixed6_4(double flt) {
    if (flt < -180000.0) flt = -180000.0;
    if (flt > 180000.0)  flt = 180000.0;
    int32_t scaled = (int32_t)(flt * 10000.0);
    return (uint32_t)(scaled + (180000 * 10000));
}

/* ========================================================================== */
/* Write helpers (little-endian, native on ARM)                                */
/* ========================================================================== */

static int write_u16(FILE *f, uint16_t v) {
    return fwrite(&v, 2, 1, f) == 1 ? 0 : -1;
}

static int write_u32(FILE *f, uint32_t v) {
    return fwrite(&v, 4, 1, f) == 1 ? 0 : -1;
}

static int write_pad32(FILE *f, size_t len) {
    static const uint8_t zeros[4] = {0};
    size_t pad = (4 - (len % 4)) % 4;
    if (pad > 0) {
        return fwrite(zeros, 1, pad, f) == pad ? 0 : -1;
    }
    return 0;
}

/* ========================================================================== */
/* Build Kismet GPS option block                                               */
/* ========================================================================== */

/*
 * Build the GPS custom option payload for an EPB.
 * Layout:
 *   [4] PEN (55922)
 *   [1] GPS Magic (0x47)
 *   [1] GPS Version (0x01)
 *   [2] GPS Data Length
 *   [4] GPS Fields Presence Bitmask
 *   [4] Longitude (fixed3_7)    — if present
 *   [4] Latitude  (fixed3_7)    — if present
 *   [4] Altitude  (fixed6_4)    — if present
 *   ... padded to 32-bit boundary
 *
 * Returns total bytes written to buf, or -1 on error.
 */
static int build_gps_option(const pcapng_gps_data_t *gps, uint8_t *buf, size_t bufsize) {
    if (!gps || !gps->has_fix || bufsize < 64) return -1;

    int p = 0;

    /* PEN */
    uint32_t pen = KISMET_PEN;
    memcpy(&buf[p], &pen, 4); p += 4;

    /* GPS Magic + Version */
    buf[p++] = GPS_MAGIC;
    buf[p++] = GPS_VERSION;

    /* Length placeholder (filled after data) */
    int length_offset = p;
    p += 2;

    /* Fields presence bitmask */
    uint32_t bitmask = 0;
    int data_start = p + 4; /* after bitmask */

    bitmask |= GPS_FIELD_LON;
    bitmask |= GPS_FIELD_LAT;
    if (gps->altitude != 0.0) {
        bitmask |= GPS_FIELD_ALT;
    }

    memcpy(&buf[p], &bitmask, 4); p += 4;

    /* Longitude */
    uint32_t lon_fixed = float_to_fixed3_7(gps->longitude);
    memcpy(&buf[p], &lon_fixed, 4); p += 4;

    /* Latitude */
    uint32_t lat_fixed = float_to_fixed3_7(gps->latitude);
    memcpy(&buf[p], &lat_fixed, 4); p += 4;

    /* Altitude (only if non-zero) */
    if (bitmask & GPS_FIELD_ALT) {
        uint32_t alt_fixed = float_to_fixed6_4(gps->altitude);
        memcpy(&buf[p], &alt_fixed, 4); p += 4;
    }

    /* Fill in GPS data length (just the GPS data, not PEN/magic/version) */
    uint16_t gps_data_len = (uint16_t)(p - data_start);
    memcpy(&buf[length_offset], &gps_data_len, 2);

    /* Pad GPS payload to 32-bit boundary */
    while (p % 4 != 0) {
        buf[p++] = 0;
    }

    return p;
}

/* ========================================================================== */
/* Write PCAPNG blocks                                                          */
/* ========================================================================== */

/* Write Section Header Block (SHB) */
static int write_shb(FILE *f) {
    /* Calculate block length */
    /* SHB: type(4) + length(4) + BOM(4) + major(2) + minor(2) + section_len(8) +
     *      options + length(4) */

    const char *app_name = "PwnaUI pcapng_gps";
    size_t app_len = strlen(app_name);

    /* Option: shb_userappl (code=4) */
    size_t opt_total = 4 + app_len + ((4 - (app_len % 4)) % 4); /* option hdr + data + pad */
    opt_total += 4; /* end-of-options */

    uint32_t block_len = 4 + 4 + 4 + 2 + 2 + 8 + opt_total + 4;

    write_u32(f, PCAPNG_SHB_TYPE);
    write_u32(f, block_len);
    write_u32(f, PCAPNG_BYTE_ORDER_MAGIC);
    write_u16(f, 1);  /* Major version */
    write_u16(f, 0);  /* Minor version */
    uint64_t section_len = 0xFFFFFFFFFFFFFFFFULL; /* unspecified */
    fwrite(&section_len, 8, 1, f);

    /* Option: shb_userappl (code 4) */
    write_u16(f, 4);
    write_u16(f, (uint16_t)app_len);
    fwrite(app_name, 1, app_len, f);
    write_pad32(f, app_len);

    /* End of options */
    write_u16(f, PCAPNG_OPT_ENDOFOPT);
    write_u16(f, 0);

    /* Block total length (repeated) */
    write_u32(f, block_len);

    return 0;
}

/* Write Interface Description Block (IDB) */
static int write_idb(FILE *f, uint32_t snaplen) {
    const char *if_name = "wlan0mon";
    size_t name_len = strlen(if_name);

    /* Options: if_name + if_tsresol + end-of-options */
    size_t opt_total = 0;
    opt_total += 4 + name_len + ((4 - (name_len % 4)) % 4);  /* if_name */
    opt_total += 4 + 4;  /* if_tsresol (1 byte data + 3 pad) */
    opt_total += 4;       /* end-of-options */

    uint32_t block_len = 4 + 4 + 2 + 2 + 4 + opt_total + 4;

    write_u32(f, PCAPNG_IDB_TYPE);
    write_u32(f, block_len);
    write_u16(f, LINKTYPE_IEEE802_11_RADIOTAP);
    write_u16(f, 0); /* Reserved */
    write_u32(f, snaplen > 0 ? snaplen : 0x0000FFFF);

    /* Option: if_name (code 2) */
    write_u16(f, PCAPNG_IF_NAME);
    write_u16(f, (uint16_t)name_len);
    fwrite(if_name, 1, name_len, f);
    write_pad32(f, name_len);

    /* Option: if_tsresol (code 9) — microsecond resolution (6) */
    write_u16(f, PCAPNG_IF_TSRESOL);
    write_u16(f, 1);
    uint8_t tsresol = 6; /* 10^-6 = microseconds */
    fwrite(&tsresol, 1, 1, f);
    write_pad32(f, 1);

    /* End of options */
    write_u16(f, PCAPNG_OPT_ENDOFOPT);
    write_u16(f, 0);

    /* Block total length (repeated) */
    write_u32(f, block_len);

    return 0;
}

/* Write Enhanced Packet Block (EPB) with optional GPS custom option */
static int write_epb(FILE *f, uint32_t ts_sec, uint32_t ts_usec,
                     const uint8_t *pkt_data, uint32_t cap_len, uint32_t orig_len,
                     const pcapng_gps_data_t *gps) {
    /* Timestamp as 64-bit microseconds */
    uint64_t ts = ((uint64_t)ts_sec * 1000000ULL) + ts_usec;
    uint32_t ts_high = (uint32_t)(ts >> 32);
    uint32_t ts_low  = (uint32_t)(ts & 0xFFFFFFFF);

    /* Packet data padding */
    size_t pkt_pad = (4 - (cap_len % 4)) % 4;

    /* Build GPS option if available */
    uint8_t gps_buf[128];
    int gps_opt_len = 0;
    size_t opt_total = 0;

    if (gps && gps->has_fix) {
        gps_opt_len = build_gps_option(gps, gps_buf, sizeof(gps_buf));
        if (gps_opt_len > 0) {
            /* Option header: code(2) + length(2) + data + padding */
            opt_total += 4 + gps_opt_len;
        }
    }

    if (opt_total > 0) {
        opt_total += 4; /* end-of-options */
    }

    /* Block: type(4) + length(4) + iface(4) + ts_high(4) + ts_low(4) +
     *        cap_len(4) + orig_len(4) + pkt_data + pkt_pad + options + length(4) */
    uint32_t block_len = 4 + 4 + 4 + 4 + 4 + 4 + 4 + cap_len + pkt_pad + opt_total + 4;

    write_u32(f, PCAPNG_EPB_TYPE);
    write_u32(f, block_len);
    write_u32(f, 0);         /* Interface ID */
    write_u32(f, ts_high);
    write_u32(f, ts_low);
    write_u32(f, cap_len);
    write_u32(f, orig_len);

    /* Packet data */
    fwrite(pkt_data, 1, cap_len, f);
    if (pkt_pad > 0) {
        uint8_t zeros[4] = {0};
        fwrite(zeros, 1, pkt_pad, f);
    }

    /* GPS custom option */
    if (gps_opt_len > 0) {
        write_u16(f, PCAPNG_OPT_CUSTOM_BIN); /* Option code 2989 */
        write_u16(f, (uint16_t)gps_opt_len);
        fwrite(gps_buf, 1, gps_opt_len, f);

        /* End of options */
        write_u16(f, PCAPNG_OPT_ENDOFOPT);
        write_u16(f, 0);
    }

    /* Block total length (repeated) */
    write_u32(f, block_len);

    return 0;
}

/* ========================================================================== */
/* GPS JSON parsing                                                             */
/* ========================================================================== */

bool pcapng_parse_gps_json(const char *json_path, pcapng_gps_data_t *gps) {
    if (!json_path || !gps) return false;

    memset(gps, 0, sizeof(*gps));

    FILE *f = fopen(json_path, "r");
    if (!f) return false;

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 4096) {
        fclose(f);
        return false;
    }

    char *json_str = malloc(fsize + 1);
    if (!json_str) {
        fclose(f);
        return false;
    }

    fread(json_str, 1, fsize, f);
    json_str[fsize] = '\0';
    fclose(f);

    /* Parse JSON */
    cJSON *root = cJSON_Parse(json_str);
    free(json_str);
    if (!root) return false;

    cJSON *lat = cJSON_GetObjectItem(root, "Latitude");
    cJSON *lon = cJSON_GetObjectItem(root, "Longitude");
    cJSON *alt = cJSON_GetObjectItem(root, "Altitude");

    if (lat && cJSON_IsNumber(lat) && lon && cJSON_IsNumber(lon)) {
        gps->latitude  = lat->valuedouble;
        gps->longitude = lon->valuedouble;
        gps->altitude  = (alt && cJSON_IsNumber(alt)) ? alt->valuedouble : 0.0;
        gps->has_fix   = true;
    }

    cJSON_Delete(root);
    return gps->has_fix;
}

/* ========================================================================== */
/* Main conversion function                                                      */
/* ========================================================================== */

int pcapng_convert_with_gps(const char *input_pcap,
                            const char *gps_json_path,
                            const char *output_pcapng) {
    if (!input_pcap || !output_pcapng) return -1;

    /* Parse GPS data if available */
    pcapng_gps_data_t gps = {0};
    if (gps_json_path) {
        pcapng_parse_gps_json(gps_json_path, &gps);
    }

    /* Open input pcap */
    FILE *fin = fopen(input_pcap, "rb");
    if (!fin) {
        fprintf(stderr, "[pcapng_gps] Cannot open: %s\n", input_pcap);
        return -1;
    }

    /* Read pcap file header */
    pcap_file_hdr_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, fin) != 1) {
        fprintf(stderr, "[pcapng_gps] Cannot read pcap header: %s\n", input_pcap);
        fclose(fin);
        return -1;
    }

    /* Validate magic */
    bool swap = false;
    bool nano = false;
    if (hdr.magic == 0xa1b2c3d4) {
        /* OK: microsecond, native endian */
    } else if (hdr.magic == 0xa1b23c4d) {
        nano = true;
    } else if (hdr.magic == 0xd4c3b2a1) {
        swap = true;
        /* Would need byte-swapping — rare on ARM, skip for now */
        fprintf(stderr, "[pcapng_gps] Byte-swapped pcap not supported: %s\n", input_pcap);
        fclose(fin);
        return -1;
    } else {
        fprintf(stderr, "[pcapng_gps] Not a pcap file: %s (magic=0x%08x)\n",
                input_pcap, hdr.magic);
        fclose(fin);
        return -1;
    }

    /* Write to temp file first, then rename (atomic-ish) */
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", output_pcapng);

    FILE *fout = fopen(tmp_path, "wb");
    if (!fout) {
        fprintf(stderr, "[pcapng_gps] Cannot create: %s\n", tmp_path);
        fclose(fin);
        return -1;
    }

    /* Write SHB */
    write_shb(fout);

    /* Write IDB */
    write_idb(fout, hdr.snaplen);

    /* Convert each packet to EPB */
    pcap_pkt_hdr_t pkt_hdr;
    int pkt_count = 0;
    uint8_t *pkt_buf = NULL;
    size_t pkt_buf_size = 0;

    while (fread(&pkt_hdr, sizeof(pkt_hdr), 1, fin) == 1) {
        uint32_t cap_len = pkt_hdr.incl_len;
        uint32_t orig_len = pkt_hdr.orig_len;

        /* Sanity check */
        if (cap_len > 262144) {
            fprintf(stderr, "[pcapng_gps] Insane packet length %u at pkt %d\n",
                    cap_len, pkt_count);
            break;
        }

        /* Grow buffer if needed */
        if (cap_len > pkt_buf_size) {
            pkt_buf = realloc(pkt_buf, cap_len);
            if (!pkt_buf) break;
            pkt_buf_size = cap_len;
        }

        if (fread(pkt_buf, 1, cap_len, fin) != cap_len) {
            fprintf(stderr, "[pcapng_gps] Short read at pkt %d\n", pkt_count);
            break;
        }

        /* Convert nanosecond timestamps to microsecond */
        uint32_t ts_usec = pkt_hdr.ts_usec;
        if (nano) ts_usec /= 1000;

        /* Write EPB with GPS */
        write_epb(fout, pkt_hdr.ts_sec, ts_usec,
                  pkt_buf, cap_len, orig_len,
                  gps.has_fix ? &gps : NULL);

        pkt_count++;
    }

    free(pkt_buf);
    fclose(fin);
    fclose(fout);

    /* Rename temp to final */
    if (rename(tmp_path, output_pcapng) != 0) {
        fprintf(stderr, "[pcapng_gps] rename failed: %s -> %s\n", tmp_path, output_pcapng);
        return -1;
    }

    fprintf(stderr, "[pcapng_gps] Converted %s -> %s (%d pkts, GPS=%s)\n",
            input_pcap, output_pcapng, pkt_count,
            gps.has_fix ? "yes" : "no");

    return 0;
}

/* ========================================================================== */
/* Directory auto-conversion                                                    */
/* ========================================================================== */

int pcapng_convert_directory(const char *handshakes_dir) {
    DIR *dir = opendir(handshakes_dir);
    if (!dir) {
        fprintf(stderr, "[pcapng_gps] Cannot open directory: %s\n", handshakes_dir);
        return -1;
    }

    int converted = 0;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        size_t len = strlen(name);

        /* Only process .pcap files (not .pcapng) */
        if (len < 6) continue;
        if (strcmp(name + len - 5, ".pcap") != 0) continue;
        /* Skip if it ends with .pcapng already */
        if (len > 7 && strcmp(name + len - 7, ".pcapng") == 0) continue;

        /* Build paths */
        char pcap_path[512], pcapng_path[512], json_path[512];
        snprintf(pcap_path, sizeof(pcap_path), "%s/%s", handshakes_dir, name);

        /* Output: replace .pcap with .pcapng */
        snprintf(pcapng_path, sizeof(pcapng_path), "%s/%.*s.pcapng",
                 handshakes_dir, (int)(len - 5), name);

        /* Skip if .pcapng already exists and is newer */
        struct stat st_pcap, st_pcapng;
        if (stat(pcap_path, &st_pcap) != 0) continue;
        if (stat(pcapng_path, &st_pcapng) == 0) {
            if (st_pcapng.st_mtime >= st_pcap.st_mtime) {
                continue; /* Already converted and up to date */
            }
        }

        /* GPS JSON companion file: same basename + .gps.json */
        /* e.g., NetworkName_aabbccddeeff.pcap → NetworkName_aabbccddeeff.gps.json */
        snprintf(json_path, sizeof(json_path), "%s/%.*s.gps.json",
                 handshakes_dir, (int)(len - 5), name);

        /* Check if GPS JSON exists */
        const char *gps_path = NULL;
        if (stat(json_path, &st_pcap) == 0) {
            gps_path = json_path;
        }

        /* Convert */
        if (pcapng_convert_with_gps(pcap_path, gps_path, pcapng_path) == 0) {
            converted++;
        }
    }

    closedir(dir);

    if (converted > 0) {
        fprintf(stderr, "[pcapng_gps] Converted %d files in %s\n",
                converted, handshakes_dir);
    }

    return converted;
}
