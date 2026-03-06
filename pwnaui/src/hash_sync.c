/*
 * hash_sync.c - GitHub Community Hash & Wordlist Sharing
 *
 * Sprint 8: Automatic sync of .22000 hash files + wordlists to a shared GitHub repo.
 * Uses fork/execvp for safe command execution (no shell injection via SSIDs).
 *
 * Repo structure:
 *   uncracked/SSID_bssid.22000     - hashcat-ready files
 *   uncracked/SSID_bssid.meta      - JSON metadata (GPS, encryption, etc.)
 *   cracked/SSID_bssid.22000       - cracked (kept for reference)
 *   cracked/SSID_bssid.potfile     - BSSID:password
 *   wordlists/community.txt        - Fleet whitelist passwords (merged)
 *   wordlists/learned.txt          - Fleet cracked passwords (merged)
 */
#include "hash_sync.h"
#include "ap_database.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static hash_sync_config_t g_config;
static hash_sync_result_t g_last_result;
static time_t g_last_sync = 0;
static bool g_initialized = false;

/* ============================================================================
 * Helpers
 * ========================================================================== */

/* Safe command execution via fork/execvp — no shell injection.
 * argv must be NULL-terminated. workdir is optional chdir before exec.
 * If quiet is true, stdout/stderr are sent to /dev/null. */
static int run_argv(const char *const argv[], const char *workdir, bool quiet) {
    /* Temporarily restore SIGCHLD to SIG_DFL so waitpid() works.
     * pwnaui sets SIGCHLD to SIG_IGN (auto-reap) which causes
     * the kernel to reap children before waitpid can collect status. */
    struct sigaction sa_old, sa_dfl;
    memset(&sa_dfl, 0, sizeof(sa_dfl));
    sa_dfl.sa_handler = SIG_DFL;
    sigemptyset(&sa_dfl.sa_mask);
    sigaction(SIGCHLD, &sa_dfl, &sa_old);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[hash_sync] fork failed: %s\n", strerror(errno));
        sigaction(SIGCHLD, &sa_old, NULL);
        return -1;
    }

    if (pid == 0) {
        /* Child */
        if (quiet) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        if (workdir && chdir(workdir) != 0) _exit(127);
        execvp(argv[0], (char *const *)argv);
        _exit(127);  /* exec failed */
    }

    /* Parent — wait for child */
    int status;
    int wrc = waitpid(pid, &status, 0);

    /* Restore original SIGCHLD handler */
    sigaction(SIGCHLD, &sa_old, NULL);

    if (wrc < 0) {
        fprintf(stderr, "[hash_sync] waitpid failed: %s\n", strerror(errno));
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "[hash_sync] child killed by signal %d\n", WTERMSIG(status));
    }
    return -1;
}

/* Run a sequence of argv commands. Stops on first failure. Returns last rc. */
static int run_argv_seq(const char *const *cmds[], int ncmds, const char *workdir, bool quiet) {
    for (int i = 0; i < ncmds; i++) {
        int rc = run_argv(cmds[i], workdir, quiet);
        if (rc != 0) return rc;
    }
    return 0;
}

/* Safe file copy without shell — reads src, writes dest */
static int safe_copy_file(const char *src, const char *dest) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dest, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); return -1; }
    }
    fclose(in);
    fclose(out);
    return 0;
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
        char repo_url[512];
        snprintf(repo_url, sizeof(repo_url), "git@github.com:%s.git", g_config.github_repo);
        const char *clone_argv[] = {"git", "clone", repo_url, HASH_SYNC_REPO_DIR, NULL};
        int rc = run_argv(clone_argv, NULL, false);
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
    snprintf(path, sizeof(path), "%s/wordlists", HASH_SYNC_REPO_DIR);
    ensure_dir(path);

    /* Configure git user (safe — contributor_name is sanitized via fork/exec) */
    char email_buf[128];
    snprintf(email_buf, sizeof(email_buf), "%s@pwnagotchi", g_config.contributor_name);
    const char *cfg_name[] = {"git", "config", "user.name", g_config.contributor_name, NULL};
    const char *cfg_email[] = {"git", "config", "user.email", email_buf, NULL};
    run_argv(cfg_name, HASH_SYNC_REPO_DIR, true);
    run_argv(cfg_email, HASH_SYNC_REPO_DIR, true);

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
    const char *argv[] = {"wget", "-q", "--spider", "--timeout=5", "https://github.com", NULL};
    return run_argv(argv, NULL, true) == 0;
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
 * Sync - Wordlist merge (community.txt + learned.txt fleet sync)
 *
 * Merge strategy: read local file + repo file, deduplicate, sort, write both.
 * This ensures all pwnagotchies converge to the same wordlists.
 * ========================================================================== */

/* Read lines from a file into a buffer. Returns line count. */
static int read_lines(const char *path, char lines[][128], int max_lines) {
    int count = 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[128];
    while (fgets(buf, sizeof(buf), f) && count < max_lines) {
        buf[strcspn(buf, "\n\r")] = 0;
        if (strlen(buf) >= 8) {  /* WPA minimum 8 chars */
            strncpy(lines[count], buf, 127);
            lines[count][127] = '\0';
            count++;
        }
    }
    fclose(f);
    return count;
}

/* Check if a string exists in a line array */
static bool line_exists(const char lines[][128], int count, const char *needle) {
    for (int i = 0; i < count; i++) {
        if (strcmp(lines[i], needle) == 0) return true;
    }
    return false;
}

/* Merge two wordlist files: local + repo → deduplicated union → write both.
 * Returns number of new passwords merged. */
static int merge_wordlist(const char *local_path, const char *repo_path) {
    /* Max 10K passwords per list (plenty for Pi dictionary attacks) */
    #define MERGE_MAX 10000
    static char merged[MERGE_MAX][128];
    int merged_count = 0;

    /* Read local file */
    static char local_lines[MERGE_MAX][128];
    int local_count = read_lines(local_path, local_lines, MERGE_MAX);

    /* Start merged with all local lines */
    for (int i = 0; i < local_count && merged_count < MERGE_MAX; i++) {
        strncpy(merged[merged_count], local_lines[i], 127);
        merged[merged_count][127] = '\0';
        merged_count++;
    }

    /* Read repo file and add any passwords not already in merged */
    static char repo_lines[MERGE_MAX][128];
    int repo_count = read_lines(repo_path, repo_lines, MERGE_MAX);
    int new_from_repo = 0;

    for (int i = 0; i < repo_count && merged_count < MERGE_MAX; i++) {
        if (!line_exists(merged, merged_count, repo_lines[i])) {
            strncpy(merged[merged_count], repo_lines[i], 127);
            merged[merged_count][127] = '\0';
            merged_count++;
            new_from_repo++;
        }
    }

    /* Write merged result to both local and repo paths */
    FILE *f;

    f = fopen(local_path, "w");
    if (f) {
        for (int i = 0; i < merged_count; i++)
            fprintf(f, "%s\n", merged[i]);
        fclose(f);
    }

    f = fopen(repo_path, "w");
    if (f) {
        for (int i = 0; i < merged_count; i++)
            fprintf(f, "%s\n", merged[i]);
        fclose(f);
    }

    return new_from_repo;
    #undef MERGE_MAX
}

/* Sync both community.txt and learned.txt between local and repo.
 * Returns total new passwords merged from fleet. */
static int sync_wordlists(void) {
    int total_new = 0;
    char repo_path[512];

    /* Sync community.txt */
    snprintf(repo_path, sizeof(repo_path), "%s/wordlists/community.txt", HASH_SYNC_REPO_DIR);
    int community_new = merge_wordlist("/home/pi/wordlists/community.txt", repo_path);
    if (community_new > 0) {
        fprintf(stderr, "[hash_sync] community.txt: +%d new passwords from fleet\n", community_new);
    }
    total_new += community_new;

    /* Sync learned.txt */
    snprintf(repo_path, sizeof(repo_path), "%s/wordlists/learned.txt", HASH_SYNC_REPO_DIR);
    int learned_new = merge_wordlist("/home/pi/wordlists/learned.txt", repo_path);
    if (learned_new > 0) {
        fprintf(stderr, "[hash_sync] learned.txt: +%d new cracked passwords from fleet\n", learned_new);
    }
    total_new += learned_new;

    return total_new;
}

/* ============================================================================
 * Sync - Pull
 * ========================================================================== */

static int sync_pull(void) {
    const char *pull_main[] = {"git", "pull", "--rebase", "origin", "main", NULL};
    int rc = run_argv(pull_main, HASH_SYNC_REPO_DIR, false);
    if (rc != 0) {
        const char *pull_master[] = {"git", "pull", "--rebase", "origin", "master", NULL};
        rc = run_argv(pull_master, HASH_SYNC_REPO_DIR, false);
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

        if (safe_copy_file(r->hash_file, dest_hash) == 0) {
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
        char commit_msg[256];
        snprintf(commit_msg, sizeof(commit_msg), "[%s] +%d hashes", g_config.contributor_name, pushed);
        const char *git_add[] = {"git", "add", "-A", NULL};
        const char *git_commit[] = {"git", "commit", "-m", commit_msg, NULL};
        const char *git_push[] = {"git", "push", "origin", "HEAD", NULL};
        const char *const *seq[] = {git_add, git_commit, git_push};
        int rc = run_argv_seq(seq, 3, HASH_SYNC_REPO_DIR, false);
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

    /* Lock — check for stale lock from previous crash */
    if (file_exists(HASH_SYNC_LOCK_FILE)) {
        FILE *lf = fopen(HASH_SYNC_LOCK_FILE, "r");
        if (lf) {
            int lock_pid = 0;
            if (fscanf(lf, "%d", &lock_pid) == 1 && lock_pid > 0) {
                /* Check if the locking process is still alive */
                if (kill(lock_pid, 0) == 0) {
                    fclose(lf);
                    snprintf(res.error, sizeof(res.error), "sync already in progress (pid %d)", lock_pid);
                    res.success = false;
                    if (result) *result = res;
                    return -1;
                }
                /* Process is dead — stale lock from crash */
                fprintf(stderr, "[hash_sync] removing stale lock (pid %d dead)\n", lock_pid);
            }
            fclose(lf);
        }
        unlink(HASH_SYNC_LOCK_FILE);
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

    /* Sync wordlists (community.txt + learned.txt fleet merge) */
    res.wordlists_synced = sync_wordlists();
    if (res.wordlists_synced > 0) {
        fprintf(stderr, "[hash_sync] merged %d new fleet passwords into wordlists\n", res.wordlists_synced);
    }

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

    fprintf(stderr, "[hash_sync] === SYNC COMPLETE: pushed=%d, community_hashes=%d, imported=%d, wordlists_new=%d ===\n",
        res.hashes_pushed, res.hashes_pulled, res.passwords_imported, res.wordlists_synced);

    memcpy(&g_last_result, &res, sizeof(res));
    if (result) *result = res;
    return 0;
}
