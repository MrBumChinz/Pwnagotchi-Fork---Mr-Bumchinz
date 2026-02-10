#ifndef PCAPNG_GPS_H
#define PCAPNG_GPS_H

#include <stdint.h>
#include <stdbool.h>

/*
 * pcapng_gps.h - Convert pcap to pcapng with Kismet GPS custom options
 *
 * Implements the Kismet PCAPNG GPS standard:
 *   https://www.kismetwireless.net/docs/dev/pcapng_gps/
 *
 * GPS data embedded as EPB custom option:
 *   Option code:    2989 (custom binary, not copied)
 *   PEN:            55922 (Kismet IANA PEN)
 *   Magic:          0x47 ('G')
 *   Version:        0x01
 *   Encoding:       fixed3_7 for lat/lon, fixed6_4 for altitude
 *
 * Compatible with AngryOxide's output format.
 */

/* GPS data for a capture location */
typedef struct {
    double latitude;
    double longitude;
    double altitude;
    bool   has_fix;
} pcapng_gps_data_t;

/* Convert a legacy pcap file to pcapng with GPS custom options.
 *
 * Reads:  <input_pcap>       — Legacy pcap (802.11 + radiotap)
 *         <gps_json_path>    — Bettercap's .gps.json file (may be NULL)
 *
 * Writes: <output_pcapng>    — PCAPNG with Kismet GPS EPB options
 *
 * Returns: 0 on success, -1 on error
 */
int pcapng_convert_with_gps(const char *input_pcap,
                            const char *gps_json_path,
                            const char *output_pcapng);

/* Parse bettercap's .gps.json file for GPS coordinates.
 * Returns true if valid GPS data was found. */
bool pcapng_parse_gps_json(const char *json_path, pcapng_gps_data_t *gps);

/* Auto-convert all .pcap files in a directory to .pcapng with GPS.
 * Skips files that already have a .pcapng counterpart.
 * Returns number of files converted. */
int pcapng_convert_directory(const char *handshakes_dir);

#endif /* PCAPNG_GPS_H */
