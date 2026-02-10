/*
 * hc22000.c — Hashcat 22000 format output for PwnaUI
 *
 * Auto-converts captured handshake pcap files to .22000 format
 * compatible with hashcat -m 22000 for GPU cracking.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include "hc22000.h"

#define HC22000_COMBINED  "/home/pi/handshakes/all.22000"
#define HC22000_TOOL      "hcxpcapngtool"

static bool tool_checked = false;
static bool tool_exists = false;

bool hc22000_tool_available(void) {
    if (!tool_checked) {
        /* Check if hcxpcapngtool is in PATH */
        int ret = system("which " HC22000_TOOL " > /dev/null 2>&1");
        tool_exists = (ret == 0);
        tool_checked = true;
        if (tool_exists) {
            fprintf(stderr, "[hc22000] hcxpcapngtool found\n");
        } else {
            fprintf(stderr, "[hc22000] hcxpcapngtool not found — .22000 output disabled\n");
        }
    }
    return tool_exists;
}

const char *hc22000_combined_path(void) {
    return HC22000_COMBINED;
}

/* Build output path: /path/to/file.pcap → /path/to/file.22000 */
static void make_output_path(const char *input, char *output, int outlen) {
    strncpy(output, input, outlen - 1);
    output[outlen - 1] = '\0';

    /* Strip .pcap / .pcapng / .cap extension */
    char *dot = strrchr(output, '.');
    if (dot) {
        *dot = '\0';
    }
    strncat(output, ".22000", outlen - strlen(output) - 1);
}

int hc22000_convert_file(const char *pcap_path) {
    if (!hc22000_tool_available()) return -1;

    /* Check input exists */
    if (access(pcap_path, R_OK) != 0) {
        fprintf(stderr, "[hc22000] cannot read: %s\n", pcap_path);
        return -1;
    }

    /* Build output path */
    char outpath[512];
    make_output_path(pcap_path, outpath, sizeof(outpath));

    /* Check if output already exists and is newer than input */
    struct stat in_stat, out_stat;
    if (stat(pcap_path, &in_stat) == 0 && stat(outpath, &out_stat) == 0) {
        if (out_stat.st_mtime >= in_stat.st_mtime) {
            return 0;  /* Already up-to-date */
        }
    }

    /* Run hcxpcapngtool:
     *   hcxpcapngtool -o output.22000 input.pcap
     * Quiet mode, only errors to stderr */
    char cmd[1200];
    snprintf(cmd, sizeof(cmd),
             HC22000_TOOL " -o '%s' '%s' > /dev/null 2>&1",
             outpath, pcap_path);

    int ret = system(cmd);
    if (ret != 0) {
        /* hcxpcapngtool returns non-zero if no usable handshake found —
         * this is normal for partial captures, not an error */
        return 0;
    }

    /* Count lines in output file to report hash count */
    FILE *f = fopen(outpath, "r");
    if (!f) return 0;

    int lines = 0;
    int ch;
    while ((ch = fgetc(f)) != EOF) {
        if (ch == '\n') lines++;
    }
    fclose(f);

    if (lines > 0) {
        fprintf(stderr, "[hc22000] converted: %s → %d hash(es)\n",
                pcap_path, lines);
    }

    return lines;
}

int hc22000_convert_directory(const char *handshakes_dir) {
    if (!hc22000_tool_available()) return -1;

    DIR *dir = opendir(handshakes_dir);
    if (!dir) {
        fprintf(stderr, "[hc22000] cannot open dir: %s\n", handshakes_dir);
        return -1;
    }

    int total_hashes = 0;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        int len = strlen(name);

        /* Process .pcap and .pcapng files */
        bool is_pcap = (len > 5 && strcmp(name + len - 5, ".pcap") == 0);
        bool is_pcapng = (len > 7 && strcmp(name + len - 7, ".pcapng") == 0);

        if (!is_pcap && !is_pcapng) continue;

        char fullpath[512];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", handshakes_dir, name);

        int hashes = hc22000_convert_file(fullpath);
        if (hashes > 0) total_hashes += hashes;
    }

    closedir(dir);

    /* Also generate combined hashfile for convenience */
    if (total_hashes > 0) {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
                 "cat '%s'/*.22000 > '%s' 2>/dev/null",
                 handshakes_dir, HC22000_COMBINED);
        system(cmd);
        fprintf(stderr, "[hc22000] combined hashfile: %s (%d total hashes)\n",
                HC22000_COMBINED, total_hashes);
    }

    return total_hashes;
}
