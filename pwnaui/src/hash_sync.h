/*
 * hash_sync.h - GitHub Community Hash Sharing
 *
 * Sprint 8: Automatic sync of .22000 hash files to a shared GitHub repo.
 * Push new hashes, pull community cracked passwords.
 * Runs when internet is available (home mode or hotspot "2nd home").
 */
#ifndef HASH_SYNC_H
#define HASH_SYNC_H

#include <stdbool.h>
#include <time.h>

#define HASH_SYNC_REPO_DIR      "/home/pi/hash_repo"
#define HASH_SYNC_INTERVAL      21600   /* 6 hours in seconds */
#define HASH_SYNC_LOCK_FILE     "/tmp/hash_sync.lock"

/* Sync configuration */
typedef struct {
    char github_repo[256];      /* e.g., "user/pwnhub-hashes" */
    char github_token[128];     /* Personal Access Token */
    char contributor_name[64];  /* Device name for commits */
    int  sync_interval;         /* Seconds between syncs (default 21600) */
    bool enabled;
} hash_sync_config_t;

/* Sync result */
typedef struct {
    int  hashes_pushed;
    int  hashes_pulled;
    int  passwords_imported;
    bool success;
    time_t sync_time;
    char error[256];
} hash_sync_result_t;

/* Default config */
hash_sync_config_t hash_sync_config_default(void);

/* Initialize sync (clone repo if needed) */
int  hash_sync_init(const hash_sync_config_t *config);

/* Check if sync is due (respects interval) */
bool hash_sync_is_due(void);

/* Check if internet is available */
bool hash_sync_has_internet(void);

/* Run full sync cycle: pull -> import cracked -> push new hashes */
int  hash_sync_run(hash_sync_result_t *result);

/* Get last sync result */
void hash_sync_last_result(hash_sync_result_t *result);

/* Get time until next sync */
int  hash_sync_seconds_until_next(void);

#endif /* HASH_SYNC_H */
