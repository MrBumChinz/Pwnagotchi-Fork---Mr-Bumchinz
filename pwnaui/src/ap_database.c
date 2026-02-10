/*
 * ap_database.c - Persistent AP Database (SQLite)
 *
 * Sprint 8 #19: SQLite-backed persistent AP tracking across power cycles.
 * Every AP ever seen is stored with GPS, encryption, attack history,
 * Thompson priors, handshake/crack status.
 */
#include "ap_database.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include "cJSON.h"

static sqlite3 *g_db = NULL;

/* ============================================================================
 * Schema
 * ========================================================================== */

static const char *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS aps ("
    "  bssid          TEXT PRIMARY KEY,"
    "  ssid           TEXT NOT NULL DEFAULT '',"
    "  encryption     TEXT NOT NULL DEFAULT '',"
    "  vendor         TEXT DEFAULT '',"
    "  channel        INTEGER DEFAULT 0,"
    "  best_rssi      INTEGER DEFAULT -100,"
    "  last_rssi      INTEGER DEFAULT -100,"
    "  lat            REAL DEFAULT 0.0,"
    "  lon            REAL DEFAULT 0.0,"
    "  first_seen     INTEGER DEFAULT 0,"
    "  last_seen      INTEGER DEFAULT 0,"
    "  times_seen     INTEGER DEFAULT 0,"
    "  has_handshake  INTEGER DEFAULT 0,"
    "  handshake_quality INTEGER DEFAULT 0,"
    "  hash_file      TEXT DEFAULT '',"
    "  cracked        INTEGER DEFAULT 0,"
    "  password       TEXT DEFAULT '',"
    "  attack_count   INTEGER DEFAULT 0,"
    "  last_attack_phase INTEGER DEFAULT -1,"
    "  thompson_alpha REAL DEFAULT 1.0,"
    "  thompson_beta  REAL DEFAULT 1.0,"
    "  clients_seen   INTEGER DEFAULT 0,"
    "  is_wpa3        INTEGER DEFAULT 0,"
    "  pmkid_available INTEGER DEFAULT 0,"
    "  exported       INTEGER DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_aps_ssid ON aps(ssid);"
    "CREATE INDEX IF NOT EXISTS idx_aps_cracked ON aps(cracked);"
    "CREATE INDEX IF NOT EXISTS idx_aps_exported ON aps(exported);"
    "CREATE INDEX IF NOT EXISTS idx_aps_has_handshake ON aps(has_handshake);";

/* ============================================================================
 * Init / Close
 * ========================================================================== */

int ap_db_init(const char *db_path) {
    if (g_db) return 0;  /* Already open */

    const char *path = db_path ? db_path : AP_DB_PATH;
    int rc = sqlite3_open(path, &g_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[ap_db] FAILED to open %s: %s\n", path, sqlite3_errmsg(g_db));
        g_db = NULL;
        return -1;
    }

    /* WAL mode for better concurrent read/write */
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_busy_timeout(g_db, 5000);

    /* Create tables */
    char *errmsg = NULL;
    rc = sqlite3_exec(g_db, SCHEMA_SQL, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[ap_db] schema error: %s\n", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        return -1;
    }

    /* Count existing records */
    sqlite3_stmt *stmt;
    int count = 0;
    if (sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM aps", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    fprintf(stderr, "[ap_db] initialized: %s (%d APs in database)\n", path, count);
    return 0;
}

void ap_db_close(void) {
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
        fprintf(stderr, "[ap_db] closed\n");
    }
}

/* ============================================================================
 * Upsert - Called on every AP sighting during scan loop
 * ========================================================================== */

int ap_db_upsert(const char *bssid, const char *ssid, const char *encryption,
                 const char *vendor, uint8_t channel, int8_t rssi,
                 double lat, double lon) {
    if (!g_db || !bssid) return -1;

    static const char *SQL =
        "INSERT INTO aps (bssid, ssid, encryption, vendor, channel, best_rssi, last_rssi,"
        "  lat, lon, first_seen, last_seen, times_seen)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1)"
        " ON CONFLICT(bssid) DO UPDATE SET"
        "  ssid = CASE WHEN excluded.ssid != '' THEN excluded.ssid ELSE aps.ssid END,"
        "  encryption = CASE WHEN excluded.encryption != '' THEN excluded.encryption ELSE aps.encryption END,"
        "  vendor = CASE WHEN excluded.vendor != '' THEN excluded.vendor ELSE aps.vendor END,"
        "  channel = excluded.channel,"
        "  last_rssi = excluded.last_rssi,"
        "  best_rssi = MAX(aps.best_rssi, excluded.best_rssi),"
        "  lat = CASE WHEN excluded.lat != 0.0 AND excluded.lon != 0.0 THEN excluded.lat ELSE aps.lat END,"
        "  lon = CASE WHEN excluded.lat != 0.0 AND excluded.lon != 0.0 THEN excluded.lon ELSE aps.lon END,"
        "  last_seen = excluded.last_seen,"
        "  times_seen = aps.times_seen + 1,"
        "  is_wpa3 = CASE WHEN excluded.encryption LIKE '%WPA3%' OR excluded.encryption LIKE '%SAE%'"
        "    THEN 1 ELSE aps.is_wpa3 END;";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(g_db, SQL, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    time_t now = time(NULL);
    sqlite3_bind_text(stmt, 1, bssid, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, ssid ? ssid : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, encryption ? encryption : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, vendor ? vendor : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 5, channel);
    sqlite3_bind_int(stmt, 6, rssi);
    sqlite3_bind_int(stmt, 7, rssi);
    sqlite3_bind_double(stmt, 8, lat);
    sqlite3_bind_double(stmt, 9, lon);
    sqlite3_bind_int64(stmt, 10, (sqlite3_int64)now);
    sqlite3_bind_int64(stmt, 11, (sqlite3_int64)now);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* ============================================================================
 * Update Functions
 * ========================================================================== */

int ap_db_set_handshake(const char *bssid, bool has_hs, int quality,
                        const char *hash_file) {
    if (!g_db || !bssid) return -1;
    static const char *SQL =
        "UPDATE aps SET has_handshake=?, handshake_quality=?, hash_file=? WHERE bssid=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db, SQL, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, has_hs ? 1 : 0);
    sqlite3_bind_int(stmt, 2, quality);
    sqlite3_bind_text(stmt, 3, hash_file ? hash_file : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, bssid, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int ap_db_set_cracked(const char *bssid, const char *password) {
    if (!g_db || !bssid) return -1;
    static const char *SQL =
        "UPDATE aps SET cracked=1, password=? WHERE bssid=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db, SQL, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, password ? password : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, bssid, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int ap_db_set_thompson(const char *bssid, float alpha, float beta) {
    if (!g_db || !bssid) return -1;
    static const char *SQL =
        "UPDATE aps SET thompson_alpha=?, thompson_beta=? WHERE bssid=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db, SQL, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_double(stmt, 1, alpha);
    sqlite3_bind_double(stmt, 2, beta);
    sqlite3_bind_text(stmt, 3, bssid, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int ap_db_record_attack(const char *bssid, int phase) {
    if (!g_db || !bssid) return -1;
    static const char *SQL =
        "UPDATE aps SET attack_count=attack_count+1, last_attack_phase=? WHERE bssid=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db, SQL, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, phase);
    sqlite3_bind_text(stmt, 2, bssid, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int ap_db_mark_exported(const char *bssid) {
    if (!g_db || !bssid) return -1;
    static const char *SQL = "UPDATE aps SET exported=1 WHERE bssid=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db, SQL, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, bssid, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* ============================================================================
 * Query Functions
 * ========================================================================== */

static void fill_record(sqlite3_stmt *stmt, ap_record_t *r) {
    memset(r, 0, sizeof(*r));
    const char *s;
    s = (const char *)sqlite3_column_text(stmt, 0);
    if (s) strncpy(r->bssid, s, sizeof(r->bssid) - 1);
    s = (const char *)sqlite3_column_text(stmt, 1);
    if (s) strncpy(r->ssid, s, sizeof(r->ssid) - 1);
    s = (const char *)sqlite3_column_text(stmt, 2);
    if (s) strncpy(r->encryption, s, sizeof(r->encryption) - 1);
    s = (const char *)sqlite3_column_text(stmt, 3);
    if (s) strncpy(r->vendor, s, sizeof(r->vendor) - 1);
    r->channel           = (uint8_t)sqlite3_column_int(stmt, 4);
    r->best_rssi         = (int8_t)sqlite3_column_int(stmt, 5);
    r->last_rssi         = (int8_t)sqlite3_column_int(stmt, 6);
    r->lat               = sqlite3_column_double(stmt, 7);
    r->lon               = sqlite3_column_double(stmt, 8);
    r->first_seen        = (time_t)sqlite3_column_int64(stmt, 9);
    r->last_seen         = (time_t)sqlite3_column_int64(stmt, 10);
    r->times_seen        = (uint32_t)sqlite3_column_int(stmt, 11);
    r->has_handshake     = sqlite3_column_int(stmt, 12) != 0;
    r->handshake_quality = sqlite3_column_int(stmt, 13);
    s = (const char *)sqlite3_column_text(stmt, 14);
    if (s) strncpy(r->hash_file, s, sizeof(r->hash_file) - 1);
    r->cracked           = sqlite3_column_int(stmt, 15) != 0;
    s = (const char *)sqlite3_column_text(stmt, 16);
    if (s) strncpy(r->password, s, sizeof(r->password) - 1);
    r->attack_count      = sqlite3_column_int(stmt, 17);
    r->last_attack_phase = sqlite3_column_int(stmt, 18);
    r->thompson_alpha    = (float)sqlite3_column_double(stmt, 19);
    r->thompson_beta     = (float)sqlite3_column_double(stmt, 20);
    r->clients_seen      = (uint32_t)sqlite3_column_int(stmt, 21);
    r->is_wpa3           = sqlite3_column_int(stmt, 22) != 0;
    r->pmkid_available   = sqlite3_column_int(stmt, 23) != 0;
    r->exported          = sqlite3_column_int(stmt, 24) != 0;
}

int ap_db_get(const char *bssid, ap_record_t *record) {
    if (!g_db || !bssid || !record) return -1;
    static const char *SQL = "SELECT * FROM aps WHERE bssid=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db, SQL, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, bssid, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        fill_record(stmt, record);
        sqlite3_finalize(stmt);
        return 0;
    }
    sqlite3_finalize(stmt);
    return -1;
}

int ap_db_get_all(ap_record_t **records, int *count) {
    if (!g_db || !records || !count) return -1;
    *records = NULL; *count = 0;

    sqlite3_stmt *stmt;
    int total = 0;
    if (sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM aps", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) total = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    if (total == 0) return 0;

    *records = calloc(total, sizeof(ap_record_t));
    if (!*records) return -1;

    if (sqlite3_prepare_v2(g_db, "SELECT * FROM aps ORDER BY last_seen DESC", -1, &stmt, NULL) != SQLITE_OK) {
        free(*records); *records = NULL;
        return -1;
    }
    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < total) {
        fill_record(stmt, &(*records)[idx++]);
    }
    sqlite3_finalize(stmt);
    *count = idx;
    return 0;
}

int ap_db_get_unexported(ap_record_t **records, int *count) {
    if (!g_db || !records || !count) return -1;
    *records = NULL; *count = 0;

    sqlite3_stmt *stmt;
    int total = 0;
    if (sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM aps WHERE has_handshake=1 AND exported=0",
                           -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) total = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    if (total == 0) return 0;

    *records = calloc(total, sizeof(ap_record_t));
    if (!*records) return -1;

    if (sqlite3_prepare_v2(g_db,
            "SELECT * FROM aps WHERE has_handshake=1 AND exported=0 ORDER BY last_seen DESC",
            -1, &stmt, NULL) != SQLITE_OK) {
        free(*records); *records = NULL;
        return -1;
    }
    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < total) {
        fill_record(stmt, &(*records)[idx++]);
    }
    sqlite3_finalize(stmt);
    *count = idx;
    return 0;
}

int ap_db_get_stats(ap_db_stats_t *stats) {
    if (!g_db || !stats) return -1;
    memset(stats, 0, sizeof(*stats));

    sqlite3_stmt *stmt;
    const char *sql = "SELECT "
        "COUNT(*), "
        "SUM(has_handshake), "
        "SUM(cracked), "
        "SUM(CASE WHEN lat != 0.0 AND lon != 0.0 THEN 1 ELSE 0 END), "
        "SUM(exported) "
        "FROM aps;";

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        stats->total_aps      = sqlite3_column_int(stmt, 0);
        stats->with_handshake = sqlite3_column_int(stmt, 1);
        stats->cracked        = sqlite3_column_int(stmt, 2);
        stats->with_gps       = sqlite3_column_int(stmt, 3);
        stats->exported       = sqlite3_column_int(stmt, 4);
    }
    sqlite3_finalize(stmt);
    return 0;
}

/* ============================================================================
 * Export - JSON for Pi-PC sync / AI training data
 * ========================================================================== */

int ap_db_export_json(const char *output_path) {
    if (!g_db) return -1;
    const char *path = output_path ? output_path : AP_DB_EXPORT_PATH;

    ap_record_t *records = NULL;
    int count = 0;
    if (ap_db_get_all(&records, &count) != 0 || count == 0) {
        return count == 0 ? 0 : -1;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ap_database_export");
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddNumberToObject(root, "exported_at", (double)time(NULL));
    cJSON_AddNumberToObject(root, "total_records", count);

    cJSON *aps = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        ap_record_t *r = &records[i];
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "bssid", r->bssid);
        cJSON_AddStringToObject(ap, "ssid", r->ssid);
        cJSON_AddStringToObject(ap, "encryption", r->encryption);
        cJSON_AddStringToObject(ap, "vendor", r->vendor);
        cJSON_AddNumberToObject(ap, "channel", r->channel);
        cJSON_AddNumberToObject(ap, "best_rssi", r->best_rssi);
        cJSON_AddNumberToObject(ap, "lat", r->lat);
        cJSON_AddNumberToObject(ap, "lon", r->lon);
        cJSON_AddNumberToObject(ap, "first_seen", (double)r->first_seen);
        cJSON_AddNumberToObject(ap, "last_seen", (double)r->last_seen);
        cJSON_AddNumberToObject(ap, "times_seen", r->times_seen);
        cJSON_AddBoolToObject(ap, "has_handshake", r->has_handshake);
        cJSON_AddNumberToObject(ap, "handshake_quality", r->handshake_quality);
        cJSON_AddBoolToObject(ap, "cracked", r->cracked);
        if (r->cracked && r->password[0]) {
            cJSON_AddStringToObject(ap, "password", r->password);
        }
        cJSON_AddNumberToObject(ap, "attack_count", r->attack_count);
        cJSON_AddNumberToObject(ap, "thompson_alpha", r->thompson_alpha);
        cJSON_AddNumberToObject(ap, "thompson_beta", r->thompson_beta);
        cJSON_AddBoolToObject(ap, "is_wpa3", r->is_wpa3);
        cJSON_AddBoolToObject(ap, "pmkid_available", r->pmkid_available);
        cJSON_AddItemToArray(aps, ap);
    }
    cJSON_AddItemToObject(root, "aps", aps);

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    free(records);

    if (!json_str) return -1;

    FILE *f = fopen(path, "w");
    if (!f) { free(json_str); return -1; }
    fputs(json_str, f);
    fclose(f);
    free(json_str);

    fprintf(stderr, "[ap_db] exported %d records to %s\n", count, path);
    return count;
}

/* ============================================================================
 * Import - Community cracked passwords (potfile format)
 *   Format: BSSID:password  or  hc22000_hash:password
 * ========================================================================== */

int ap_db_import_potfile(const char *potfile_path) {
    if (!g_db || !potfile_path) return -1;

    FILE *f = fopen(potfile_path, "r");
    if (!f) return -1;

    int imported = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        nl = strchr(line, '\r');
        if (nl) *nl = '\0';

        /* Find the last ':' separator - password is after it */
        char *sep = strrchr(line, ':');
        if (!sep || sep == line) continue;
        *sep = '\0';
        char *password = sep + 1;

        /* Try to extract BSSID */
        char bssid[18] = {0};
        char *p = line;

        /* hc22000 format: WPA*TYPE*PMKID/MIC*MAC_AP*MAC_STA*ESSID*... */
        if (strncmp(p, "WPA*", 4) == 0) {
            char linecopy[512];
            strncpy(linecopy, p, sizeof(linecopy) - 1);
            char *fields[10] = {NULL};
            int fi = 0;
            char *tok = linecopy;
            while (fi < 10 && tok) {
                fields[fi++] = tok;
                tok = strchr(tok, '*');
                if (tok) *tok++ = '\0';
            }
            if (fi >= 4 && strlen(fields[3]) == 12) {
                snprintf(bssid, sizeof(bssid), "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c",
                    fields[3][0], fields[3][1], fields[3][2], fields[3][3],
                    fields[3][4], fields[3][5], fields[3][6], fields[3][7],
                    fields[3][8], fields[3][9], fields[3][10], fields[3][11]);
            }
        } else if (strlen(p) == 17 && p[2] == ':' && p[5] == ':') {
            strncpy(bssid, p, 17);
        }

        if (bssid[0] && password[0]) {
            if (ap_db_set_cracked(bssid, password) == 0) {
                imported++;
            }
        }
    }
    fclose(f);

    if (imported > 0) {
        fprintf(stderr, "[ap_db] imported %d cracked passwords from %s\n", imported, potfile_path);
    }
    return imported;
}

/* ============================================================================
 * Prune - Remove very old, never-seen-again APs
 * ========================================================================== */

int ap_db_prune(int max_age_days) {
    if (!g_db) return -1;

    time_t cutoff = time(NULL) - (max_age_days * 86400);
    static const char *SQL =
        "DELETE FROM aps WHERE last_seen < ? AND has_handshake = 0 AND cracked = 0;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db, SQL, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)cutoff);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    int deleted = sqlite3_changes(g_db);
    if (deleted > 0) {
        fprintf(stderr, "[ap_db] pruned %d APs older than %d days\n", deleted, max_age_days);
    }
    return deleted;
}
