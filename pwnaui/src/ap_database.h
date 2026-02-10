/*
 * ap_database.h - Persistent AP Database (SQLite)
 *
 * Sprint 8 #19: Track every AP ever seen across power cycles.
 * Stores BSSID, SSID, encryption, GPS, attack history, Thompson state,
 * handshake/crack status. Enables long-term intelligence and community
 * hash sharing via GitHub.
 */
#ifndef AP_DATABASE_H
#define AP_DATABASE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define AP_DB_PATH          "/home/pi/ap_database.db"
#define AP_DB_EXPORT_PATH   "/home/pi/ap_export.json"

/* AP record stored in SQLite */
typedef struct {
    char     bssid[18];          /* XX:XX:XX:XX:XX:XX */
    char     ssid[33];
    char     encryption[32];     /* WPA2, WPA3, OPEN, WEP */
    char     vendor[64];
    uint8_t  channel;
    int8_t   best_rssi;
    int8_t   last_rssi;
    double   lat;
    double   lon;
    time_t   first_seen;
    time_t   last_seen;
    uint32_t times_seen;
    bool     has_handshake;
    int      handshake_quality;  /* 0-100 */
    char     hash_file[128];     /* path to .22000 file */
    bool     cracked;
    char     password[128];
    int      attack_count;
    int      last_attack_phase;
    float    thompson_alpha;
    float    thompson_beta;
    uint32_t clients_seen;
    bool     is_wpa3;
    bool     pmkid_available;
    bool     exported;           /* pushed to GitHub? */
} ap_record_t;

/* Database stats */
typedef struct {
    int total_aps;
    int with_handshake;
    int cracked;
    int with_gps;
    int exported;
    time_t last_sync;
} ap_db_stats_t;

/* Initialize database (create tables if needed) */
int  ap_db_init(const char *db_path);

/* Close database cleanly */
void ap_db_close(void);

/* Upsert an AP - called on every scan sighting */
int  ap_db_upsert(const char *bssid, const char *ssid, const char *encryption,
                  const char *vendor, uint8_t channel, int8_t rssi,
                  double lat, double lon);

/* Update handshake status for an AP */
int  ap_db_set_handshake(const char *bssid, bool has_hs, int quality,
                         const char *hash_file);

/* Mark an AP as cracked with password */
int  ap_db_set_cracked(const char *bssid, const char *password);

/* Update Thompson priors for an AP */
int  ap_db_set_thompson(const char *bssid, float alpha, float beta);

/* Update attack tracking */
int  ap_db_record_attack(const char *bssid, int phase);

/* Mark as exported to GitHub */
int  ap_db_mark_exported(const char *bssid);

/* Query functions */
int  ap_db_get(const char *bssid, ap_record_t *record);
int  ap_db_get_all(ap_record_t **records, int *count);
int  ap_db_get_unexported(ap_record_t **records, int *count);
int  ap_db_get_stats(ap_db_stats_t *stats);

/* Export all records as JSON (for Pi-PC sync / AI training) */
int  ap_db_export_json(const char *output_path);

/* Import cracked passwords from community (potfile format) */
int  ap_db_import_potfile(const char *potfile_path);

/* Cleanup: remove APs not seen in N days */
int  ap_db_prune(int max_age_days);

#endif /* AP_DATABASE_H */
