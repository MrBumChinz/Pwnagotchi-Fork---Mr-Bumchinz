/*
 * hc22000.h — Hashcat 22000 format output for PwnaUI
 *
 * Converts captured pcap/pcapng handshake files to hashcat's
 * hc22000 format (.22000) for GPU-accelerated cracking.
 *
 * Uses hcxpcapngtool (from hcxtools) when available,
 * falls back to native extraction otherwise.
 */

#ifndef HC22000_H
#define HC22000_H

#include <stdbool.h>

/* Directory where .22000 files are stored alongside pcaps */
#define HC22000_OUTPUT_DIR  "/home/pi/handshakes"

/* Convert a single pcap/pcapng file to hc22000 format.
 * Output file is placed alongside input: foo.pcap → foo.22000
 *
 * Returns:
 *   >0 = number of hash lines written
 *    0 = no convertible handshakes found
 *   -1 = error (tool not found, I/O error, etc.)
 */
int hc22000_convert_file(const char *pcap_path);

/* Convert all pcap/pcapng files in a directory to .22000 format.
 * Skips files that already have a .22000 counterpart (up-to-date).
 * Returns total number of new hash lines generated. */
int hc22000_convert_directory(const char *handshakes_dir);

/* Check if hcxpcapngtool is available on the system */
bool hc22000_tool_available(void);

/* Get path to the combined hashfile containing ALL hashes.
 * This single file can be fed directly to hashcat:
 *   hashcat -m 22000 /home/pi/handshakes/all.22000 wordlist.txt
 */
const char *hc22000_combined_path(void);

#endif /* HC22000_H */
