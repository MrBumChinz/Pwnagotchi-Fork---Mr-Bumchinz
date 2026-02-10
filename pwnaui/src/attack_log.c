/*
 * attack_log.c - Ring buffer attack logger with JSON serialization
 * Sprint 5: #22 JSON attack log
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "attack_log.h"
#include "cJSON.h"

static attack_log_t g_attack_log;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

attack_log_t *attack_log_get(void) {
    return &g_attack_log;
}

void attack_log_init(void) {
    memset(&g_attack_log, 0, sizeof(g_attack_log));
    g_attack_log.last_flush = time(NULL);
    fprintf(stderr, "[attack_log] initialized (ring=%d)\n", ATTACK_LOG_MAX);
}

void attack_log_add(const char *ssid, const char *bssid,
                    const char *type, const char *result,
                    int rssi, int channel) {
    pthread_mutex_lock(&g_log_mutex);

    attack_log_entry_t *e = &g_attack_log.entries[g_attack_log.head];
    e->timestamp = time(NULL);
    strncpy(e->ssid, ssid ? ssid : "", sizeof(e->ssid) - 1);
    e->ssid[sizeof(e->ssid) - 1] = '\0';
    strncpy(e->bssid, bssid ? bssid : "", sizeof(e->bssid) - 1);
    e->bssid[sizeof(e->bssid) - 1] = '\0';
    strncpy(e->attack_type, type ? type : "", sizeof(e->attack_type) - 1);
    e->attack_type[sizeof(e->attack_type) - 1] = '\0';
    strncpy(e->result, result ? result : "", sizeof(e->result) - 1);
    e->result[sizeof(e->result) - 1] = '\0';
    e->rssi = rssi;
    e->channel = channel;

    g_attack_log.head = (g_attack_log.head + 1) % ATTACK_LOG_MAX;
    if (g_attack_log.count < ATTACK_LOG_MAX)
        g_attack_log.count++;
    g_attack_log.total++;

    /* Auto-flush every 5 minutes */
    time_t now = time(NULL);
    if (now - g_attack_log.last_flush >= 300) {
        g_attack_log.last_flush = now;
        /* Flush unlocked — we already hold the mutex */
        FILE *f = fopen(ATTACK_LOG_FILE, "w");
        if (f) {
            char buf[32768];
            /* Build JSON inline (mutex already held) */
            int n = g_attack_log.count;
            int start = (g_attack_log.head - n + ATTACK_LOG_MAX) % ATTACK_LOG_MAX;
            int written = 0;
            written += snprintf(buf + written, sizeof(buf) - written,
                "{\"total\":%d,\"entries\":[", g_attack_log.total);
            int limit = n < 100 ? n : 100;  /* file gets last 100 */
            int file_start = (g_attack_log.head - limit + ATTACK_LOG_MAX) % ATTACK_LOG_MAX;
            for (int i = 0; i < limit && written < (int)sizeof(buf) - 200; i++) {
                int idx = (file_start + i) % ATTACK_LOG_MAX;
                attack_log_entry_t *entry = &g_attack_log.entries[idx];
                if (i > 0) buf[written++] = ',';
                written += snprintf(buf + written, sizeof(buf) - written,
                    "{\"ts\":%ld,\"ssid\":\"%s\",\"bssid\":\"%s\","
                    "\"type\":\"%s\",\"result\":\"%s\","
                    "\"rssi\":%d,\"ch\":%d}",
                    (long)entry->timestamp, entry->ssid, entry->bssid,
                    entry->attack_type, entry->result, entry->rssi, entry->channel);
            }
            written += snprintf(buf + written, sizeof(buf) - written, "]}");
            fwrite(buf, 1, written, f);
            fclose(f);
        }
    }

    pthread_mutex_unlock(&g_log_mutex);
}

int attack_log_to_json(char *buf, size_t bufsize, int max_entries) {
    pthread_mutex_lock(&g_log_mutex);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "total", g_attack_log.total);

    cJSON *arr = cJSON_CreateArray();
    int n = g_attack_log.count;
    if (max_entries > 0 && max_entries < n) n = max_entries;
    int start = (g_attack_log.head - n + ATTACK_LOG_MAX) % ATTACK_LOG_MAX;

    for (int i = 0; i < n; i++) {
        int idx = (start + i) % ATTACK_LOG_MAX;
        attack_log_entry_t *e = &g_attack_log.entries[idx];
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "ts", (double)e->timestamp);
        cJSON_AddStringToObject(entry, "ssid", e->ssid);
        cJSON_AddStringToObject(entry, "bssid", e->bssid);
        cJSON_AddStringToObject(entry, "type", e->attack_type);
        cJSON_AddStringToObject(entry, "result", e->result);
        cJSON_AddNumberToObject(entry, "rssi", e->rssi);
        cJSON_AddNumberToObject(entry, "ch", e->channel);
        cJSON_AddItemToArray(arr, entry);
    }
    cJSON_AddItemToObject(root, "entries", arr);

    char *json = cJSON_PrintUnformatted(root);
    int len = 0;
    if (json) {
        len = snprintf(buf, bufsize, "%s", json);
        free(json);
    } else {
        len = snprintf(buf, bufsize, "{\"total\":0,\"entries\":[]}");
    }
    cJSON_Delete(root);

    pthread_mutex_unlock(&g_log_mutex);
    return len;
}

void attack_log_flush(void) {
    /* Force immediate flush */
    attack_log_add("", "", "", "", 0, 0);  /* trigger with dummy */
    g_attack_log.total--;  /* undo the dummy count */
    g_attack_log.count--;
    g_attack_log.head = (g_attack_log.head - 1 + ATTACK_LOG_MAX) % ATTACK_LOG_MAX;
}
