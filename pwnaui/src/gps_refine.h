#ifndef GPS_REFINE_H
#define GPS_REFINE_H

#include <stdbool.h>
#include <stdint.h>
#include "gps.h"

/**
 * GPS Refinement System
 *
 * Updates handshake GPS coordinates when the pwnagotchi sees an AP
 * it already has a handshake for at a stronger signal (closer range).
 *
 * Logic: Higher RSSI (dBm) = physically closer to AP = our GPS position
 * more accurately represents the AP's true location.
 *
 * Stores RSSI in the .gps.json file so future comparisons know whether
 * we've gotten even closer.
 */

/* Initialize the refinement cache */
void gps_refine_init(void);

/**
 * Check if an AP's stored GPS should be refined.
 *
 * @param bssid       AP BSSID string "aa:bb:cc:dd:ee:ff"
 * @param rssi        Current signal strength (dBm, higher = closer)
 * @param gps         Current live GPS data (must have fix)
 * @param pcap_path   Path to existing .pcap file (derives .gps.json path)
 * @return true if GPS was updated
 */
bool gps_refine_check(const char *bssid, int8_t rssi,
                      const gps_data_t *gps, const char *pcap_path);

/* Get count of refinements performed this session */
int gps_refine_count(void);

#endif /* GPS_REFINE_H */
