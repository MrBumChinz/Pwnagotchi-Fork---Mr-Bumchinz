#ifndef ATTACK_LOG_H
#define ATTACK_LOG_H

#include <stdint.h>
#include <time.h>
#include <stdbool.h>

#define ATTACK_LOG_MAX 256
#define ATTACK_LOG_FILE "/home/pi/attack_log.json"

typedef struct {
    time_t timestamp;
    char ssid[33];
    char bssid[18];
    char attack_type[24];   /* assoc, deauth, csa, rogue_m2, disassoc, probe, pmf, hulk */
    char result[8];         /* ok, fail, skip */
    int rssi;
    int channel;
} attack_log_entry_t;

typedef struct {
    attack_log_entry_t entries[ATTACK_LOG_MAX];
    int head;       /* next write position (circular) */
    int count;      /* entries currently in buffer */
    int total;      /* total attacks logged lifetime */
    time_t last_flush;
} attack_log_t;

/* Singleton access */
attack_log_t *attack_log_get(void);

void attack_log_init(void);
void attack_log_add(const char *ssid, const char *bssid,
                    const char *type, const char *result,
                    int rssi, int channel);
int  attack_log_to_json(char *buf, size_t bufsize, int max_entries);
void attack_log_flush(void);

#endif /* ATTACK_LOG_H */
