/*
 * hash_sync.c - GitHub Community Hash Sharing
 *
 * Sprint 8: Automatic sync of .22000 hash files to a shared GitHub repo.
 * Uses system() calls to git - lightweight, no libgit2 dependency.
 *
 * Repo structure:
 *   uncracked/SSID_bssid.22000     - hashcat-ready files
 *   uncracked/SSID_bssid.meta      - JSON metadata (GPS, encryption, etc.)
 *   cracked/SSID_bssid.22000       - cracked (kept for reference)
 *   cracked/SSID_bssid.potfile     - BSSID:password
 */
#include "hash_sync.h"
#include "ap_database.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static hash_sync_config_t g_config;
static hash_sync_result_t g_last_result;
static time_t g_last_sync = 0;
static bool g_initialized = false;

/* ============================================================================
 * Helpers
 * ========================================================================== */

static int run_cmd(const char *cmd) {
    int rc = system(cmd);
    if (rc == -1) return -1;
    return WEXITSTATUS(rc);
}

static int run_cmd_quiet(const char *cmd) {
    char full[1024];
    snprintf(full, sizeof(full), "%s > /dev/null 2>&1", cmd);
    return run_cmd(full);
}

static bool file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

static void ensure_dir(const char *path) {
    mkdir(path, 0755);
}

/* ============================================================================
 * Config
 * ========================================================================== */

hash_sync_config_t hash_sync_config_default(void) {
    hash_sync_config_t config;
    memset(&config, 0, sizeof(config));
    config.sync_interval = HASH_SYNC_INTERVAL;
    config.enabled = false;
    snprintf(config.contributor_name, sizeof(config.contributor_name), "pwnagotchi");
    return config;
}

/* ============================================================================
 * Init
 * ========================================================================== */

int hash_sync_init(const hash_sync_config_t *config) {
    if (!config) return -1;
    memcpy(&g_config, config, sizeof(g_config));

    if (!g_config.enabled || g_config.github_repo[0] == '\0') {
        fprintf(stderr, "[hash_sync] disabled (no repo/token configured)\n");
        return 0;
    }

    /* Check if repo already cloned */
    char git_dir[512];
    snprintf(git_dir, sizeof(git_dir), "%s/.git", HASH_SYNC_REPO_DIR);

    if (!file_exists(git_dir)) {
        fprintf(stderr, "[hash_sync] cloning %s...\n", g_config.github_repo);
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
            "git clone git@github.com:%s.git %s 2>&1",
            g_config.github_repo, HASH_SYNC_REPO_DIR);
        int rc = run_cmd(cmd);
        if (rc != 0) {
            fprintf(stderr, "[hash_sync] clone FAILED (rc=%d) - will retry on next sync\n", rc);
        } else {
            fprintf(stderr, "[hash_sync] cloned successfully\n");
        }
    }

    /* Ensure directory structure */
    char path[512];
    snprintf(path, sizeof(path), "%s/uncracked", HASH_SYNC_REPO_DIR);
    ensure_dir(HASH_SYNC_REPO_DIR);
    ensure_dir(path);
    snprintf(path, sizeof(path), "%s/cracked", HASH_SYNC_REPO_DIR);
    ensure_dir(path);
    snprintf(path, sizeof(path), "%s/metadata", HASH_SYNC_REPO_DIR);
    ensure_dir(path);

    /* Configure git user */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "cd %s 2>/dev/null && git config user.name '%s' && git config user.email '%s@pwnagotchi'",
        HASH_SYNC_REPO_DIR, g_config.contributor_name, g_config.contributor_name);
    run_cmd_quiet(cmd);

    g_initialized = true;
    fprintf(stderr, "[hash_sync] initialized (repo: %s, interval: %ds)\n",
        g_config.github_repo, g_config.sync_interval);
    return 0;
}

/* ============================================================================
 * Status
 * ========================================================================== */

bool hash_sync_is_due(void) {
    if (!g_initialized || !g_config.enabled) return false;
    time_t now = time(NULL);
    return (now - g_last_sync) >= g_config.sync_interval;
}

bool hash_sync_has_internet(void) {
    return run_cmd_quiet("wget -q --spider --timeout=5 https://github.com") == 0;
}

int hash_sync_seconds_until_next(void) {
    if (!g_initialized || !g_config.enabled) return -1;
    time_t now = time(NULL);
    int elapsed = (int)(now - g_last_sync);
    int remaining = g_config.sync_interval - elapsed;
    return remaining > 0 ? remaining : 0;
}

void hash_sync_last_result(hash_sync_result_t *result) {
    if (result) memcpy(result, &g_last_result, sizeof(*result));
}

/* ============================================================================
 * Sync - Pull
 * ========================================================================== */

static int sync_pull(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "cd %s && git pull --rebase origin main 2>&1", HASH_SYNC_REPO_DIR);
    int rc = run_cmd(cmd);
    if (rc != 0) {
        snprintf(cmd, sizeof(cmd), "cd %s && git pull --rebase origin master 2>&1", HASH_SYNC_REPO_DIR);
        rc = run_cmd(cmd);
    }
    return rc;
}

/* ============================================================================
 * Sync - Import community cracked passwords
 * ========================================================================== */

static int sync_import_cracked(void) {
    int imported = 0;
    char cracked_dir[512];
    snprintf(cracked_dir, sizeof(cracked_dir), "%s/cracked", HASH_SYNC_REPO_DIR);

    DIR *d = opendir(cracked_dir);
    if (!d) return 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!strstr(ent->d_name, ".potfile")) continue;
        char potpath[768];
        snprintf(potpath, sizeof(potpath), "%s/%s", cracked_dir, ent->d_name);
        int n = ap_db_import_potfile(potpath);
        if (n > 0) imported += n;
    }
    closedir(d);
    return imported;
}

/* ============================================================================
 * Sync - Push new hashes
 * ========================================================================== */

static void write_meta_file(const char *meta_path, const ap_record_t *r) {
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "bssid", r->bssid);
    cJSON_AddStringToObject(meta, "ssid", r->ssid);
    cJSON_AddStringToObject(meta, "encryption", r->encryption);
    cJSON_AddStringToObject(meta, "vendor", r->vendor);
    cJSON_AddNumberToObject(meta, "channel", r->channel);
    cJSON_AddNumberToObject(meta, "best_rssi", r->best_rssi);
    if (r->lat != 0.0 && r->lon != 0.0) {
        cJSON_AddNumberToObject(meta, "lat", r->lat);
        cJSON_AddNumberToObject(meta, "lon", r->lon);
    }
    cJSON_AddNumberToObject(meta, "first_seen", (double)r->first_seen);
    cJSON_AddNumberToObject(meta, "last_seen", (double)r->last_seen);
    cJSON_AddNumberToObject(meta, "times_seen", r->times_seen);
    cJSON_AddBoolToObject(meta, "is_wpa3", r->is_wpa3);
    cJSON_AddNumberToObject(meta, "handshake_quality", r->handshake_quality);

    char *json = cJSON_Print(meta);
    cJSON_Delete(meta);
    if (json) {
        FILE *f = fopen(meta_path, "w");
        if (f) { fputs(json, f); fclose(f); }
        free(json);
    }
}

static int sync_push_hashes(void) {
    int pushed = 0;
    ap_record_t *records = NULL;
    int count = 0;

    if (ap_db_get_unexported(&records, &count) != 0 || count == 0) {
        return 0;
    }

    for (int i = 0; i < count; i++) {
        ap_record_t *r = &records[i];

        if (r->hash_file[0] == '\0') continue;
        if (!file_exists(r->hash_file)) continue;

        /* Build filenames: SSID_bssid_nocolon.22000 */
        char bssid_nocolon[13] = {0};
        int j = 0;
        for (int k = 0; r->bssid[k] && j < 12; k++) {
            if (r->bssid[k] != ':') bssid_nocolon[j++] = r->bssid[k];
        }

        const char *subdir = r->cracked ? "cracked" : "uncracked";
        char dest_hash[768], dest_meta[768];
        snprintf(dest_hash, sizeof(dest_hash), "%s/%s/%s_%s.22000",
            HASH_SYNC_REPO_DIR, subdir, r->ssid, bssid_nocolon);
        snprintf(dest_meta, sizeof(dest_meta), "%s/%s/%s_%s.meta",
            HASH_SYNC_REPO_DIR, subdir, r->ssid, bssid_nocolon);

        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", r->hash_file, dest_hash);
        if (run_cmd_quiet(cmd) == 0) {
            write_meta_file(dest_meta, r);

            if (r->cracked && r->password[0]) {
                char potpath[768];
                snprintf(potpath, sizeof(potpath), "%s/cracked/%s_%s.potfile",
                    HASH_SYNC_REPO_DIR, r->ssid, bssid_nocolon);
                FILE *f = fopen(potpath, "w");
                if (f) {
                    fprintf(f, "%s:%s\n", r->bssid, r->password);
                    fclose(f);
                }
            }

            ap_db_mark_exported(r->bssid);
            pushed++;
        }
    }
    free(records);

    if (pushed > 0) {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
            "cd %s && git add -A && git commit -m '[%s] +%d hashes' && "
            "git push origin HEAD 2>&1",
            HASH_SYNC_REPO_DIR, g_config.contributor_name, pushed);
        int rc = run_cmd(cmd);
        if (rc != 0) {
            fprintf(stderr, "[hash_sync] git push failed (rc=%d)\n", rc);
        }
    }

    return pushed;
}

/* ============================================================================
 * Full Sync Cycle
 * ========================================================================== */

int hash_sync_run(hash_sync_result_t *result) {
    hash_sync_result_t res;
    memset(&res, 0, sizeof(res));
    res.sync_time = time(NULL);

    if (!g_initialized || !g_config.enabled) {
        snprintf(res.error, sizeof(res.error), "not initialized or disabled");
        res.success = false;
        if (result) *result = res;
        return -1;
    }

    /* Lock */
    if (file_exists(HASH_SYNC_LOCK_FILE)) {
        snprintf(res.error, sizeof(res.error), "sync already in progress");
        res.success = false;
        if (result) *result = res;
        return -1;
    }
    FILE *lock = fopen(HASH_SYNC_LOCK_FILE, "w");
    if (lock) { fprintf(lock, "%d", (int)getpid()); fclose(lock); }

    fprintf(stderr, "[hash_sync] === SYNC STARTING ===\n");

    /* Pull */
    fprintf(stderr, "[hash_sync] pulling from remote...\n");
    if (sync_pull() != 0) {
        fprintf(stderr, "[hash_sync] pull failed - continuing with local data\n");
    }

    /* Import cracked */
    res.passwords_imported = sync_import_cracked();
    if (res.passwords_imported > 0) {
        fprintf(stderr, "[hash_sync] imported %d community passwords\n", res.passwords_imported);
    }

    /* Push hashes */
    res.hashes_pushed = sync_push_hashes();
    fprintf(stderr, "[hash_sync] pushed %d new hashes\n", res.hashes_pushed);

    /* Count pulled (approx) */
    char uncracked_dir[512];
    snprintf(uncracked_dir, sizeof(uncracked_dir), "%s/uncracked", HASH_SYNC_REPO_DIR);
    DIR *d = opendir(uncracked_dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strstr(ent->d_name, ".22000")) res.hashes_pulled++;
        }
        closedir(d);
    }

    res.success = true;
    g_last_sync = time(NULL);

    unlink(HASH_SYNC_LOCK_FILE);

    fprintf(stderr, "[hash_sync] === SYNC COMPLETE: pushed=%d, community_hashes=%d, imported=%d ===\n",
        res.hashes_pushed, res.hashes_pulled, res.passwords_imported);

    memcpy(&g_last_result, &res, sizeof(res));
    if (result) *result = res;
    return 0;
}
