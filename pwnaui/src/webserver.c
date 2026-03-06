/*
 * webserver.c - HTTP server for PwnaUI with PNG face support
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include "webserver.h"
#include "cJSON.h"
#include "attack_log.h"
#include "ap_database.h"
#include <dirent.h>
static webserver_state_callback_t g_state_cb = NULL;
static webserver_gps_callback_t g_gps_cb = NULL;
static webserver_name_callback_t g_name_cb = NULL;

void webserver_set_gps_callback(webserver_gps_callback_t cb) {
    g_gps_cb = cb;
}
void webserver_set_name_callback(webserver_name_callback_t cb) {
    g_name_cb = cb;
}
/* Theme faces directory - will be set dynamically based on current theme */
#define THEME_BASE "/home/pi/pwnaui/themes"
static char g_current_theme[64] = "default";
/* Set current theme for face image serving */
void webserver_set_theme(const char *theme) {
    if (theme && theme[0]) {
        strncpy(g_current_theme, theme, sizeof(g_current_theme) - 1);
        g_current_theme[sizeof(g_current_theme) - 1] = '\0';
    }
}
/* HTML that matches the actual e-ink display layout exactly */
static const char *HTML_PAGE =
"<!DOCTYPE html>\n"
"<html><head>\n"
"<meta charset='UTF-8'>\n"
"<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
"<title>PwnaUI</title>\n"
"<style>\n"
"*{margin:0;padding:0;box-sizing:border-box}\n"
"body{background:#666;font-family:'DejaVu Sans Mono','Courier New',monospace;display:flex;justify-content:center;align-items:center;min-height:100vh}\n"
".screen{width:250px;height:122px;background:#e8e8e8;border:3px solid #333;position:relative;overflow:hidden;transform:scale(3);box-shadow:0 4px 20px rgba(0,0,0,0.5)}\n"
".topbar{position:absolute;top:0;left:0;right:0;height:14px;font-size:9px;line-height:14px;border-bottom:1px solid #000;padding:0 3px;display:flex;justify-content:space-between}\n"
".main{position:absolute;top:15px;left:0;right:0;bottom:14px}\n"
".left{position:absolute;left:3px;top:0;width:85px}\n"
".name{font-size:8px;font-weight:bold}\n"
".face-container{width:85px;height:70px;display:flex;align-items:center;justify-content:center}\n"
".face-img{max-width:85px;max-height:70px;image-rendering:pixelated}\n"
".face-text{font-size:24px;text-align:center}\n"
".right{position:absolute;left:90px;top:0;right:3px;bottom:0;font-size:7px}\n"
".status{position:absolute;left:58px;top:0;font-size:7px;line-height:1.3;word-wrap:break-word;max-height:28px;overflow:hidden;text-align:left}\n"
".stats{position:absolute;right:0;bottom:22px;text-align:left}\n"
".xp-row{font-size:7px}\n"
".xp-bar{display:inline-block;width:68px;height:5px;border:1px solid #000;vertical-align:middle}\n"
".xp-fill{height:100%;background:#000}\n"
".lvl-row{font-size:7px}\n"
".memtemp{position:absolute;right:0;bottom:0;text-align:right}\n"
".pwnhub{display:none;position:absolute;right:65px;bottom:0;font-size:8px}\n"
".pwnhub.active{display:flex;gap:2px;align-items:center}\n"
".macro-icon{width:20px;height:16px;object-fit:contain}\n"
".memtemp table{border-collapse:collapse}\n"
".memtemp td{text-align:center;padding:0 2px}\n"
".mt-hdr{font-size:6px}\n"
".mt-val{font-size:8px;font-weight:bold}\n"
".bottombar{position:absolute;bottom:0;left:0;right:0;height:13px;font-size:8px;line-height:13px;border-top:1px solid #000;padding:0 3px;display:flex;justify-content:space-between;font-weight:bold}\n"
"@media(max-width:800px){.screen{transform:scale(2)}}\n"
"@media(max-width:550px){.screen{transform:scale(1.5)}}\n"
"</style>\n"
"</head><body>\n"
"<div class='screen'>\n"
"<div class='topbar'>\n"
"<span>CH:<span id='ch'>-</span> APS:<span id='aps'>0</span> <span id='bt'>BT-</span></span>\n"
"<span><span id='gps'>GPS-</span> <span id='uptime'>00:00:00:00</span></span>\n"
"</div>\n"
"<div class='main'>\n"
"<div class='left'>\n"
"<div class='name'><span id='name'>pwnagotchi</span>&gt;</div>\n"
"<div class='face-container'>\n"
"<img class='face-img' id='face-img' style='display:none'>\n"
"<span class='face-text' id='face-text'></span>\n"
"</div>\n"
"</div>\n"
"<div class='right'>\n"
"<div class='status' id='status'>...</div>\n"
"<div class='stats'>\n"
"<div class='xp-row'>XP:<span id='xp'>0</span>% <span class='xp-bar'><div class='xp-fill' id='xpbar' style='width:0%'></div></span></div>\n"
"<div class='lvl-row'>Lvl:<span id='lvl'>0</span> <span id='title'>Newborn</span> W:<span id='wins'>0</span>/<span id='losses'>0</span></div>\n"
"</div>\n"
"<div class='memtemp'><table><tr><td class='mt-hdr'>mem</td><td class='mt-hdr'>cpu</td><td class='mt-hdr'>tmp</td></tr><tr><td class='mt-val' id='mem'>0%</td><td class='mt-val' id='cpu'>0%</td><td class='mt-val' id='tmp'>0C</td></tr></table></div>\n"
"<div class='pwnhub' id='pwnhub'>\n"
"<img class='macro-icon' id='m-food' src='/assets/Food_100.png'>\n"
"<img class='macro-icon' id='m-str' src='/assets/Strength_100.png'>\n"
"<img class='macro-icon' id='m-spr' src='/assets/Spirit_100.png'>\n"
"</div>\n"
"</div>\n"
"</div>\n"
"<div class='bottombar'>\n"
"<span>PWDS:<span id='pwds'>0</span> FHS:<span id='fhs'>0</span> PHS:<span id='phs'>0</span> TCAPS:<span id='tcaps'>0</span></span>\n"
"<span id='bat'>BAT-</span> <span id='mode'>AUTO</span>\n"
"</div>\n"
"</div>\n"
"<script>\n"
"var lastFace='';\n"
"function u(){fetch('/api/state').then(r=>r.json()).then(d=>{\n"
"document.getElementById('name').textContent=d.name||'pwnagotchi';\n"
"document.getElementById('status').textContent=d.status||'...';\n"
"document.getElementById('ch').textContent=d.channel||'-';\n"
"document.getElementById('aps').textContent=d.aps||'0';\n"
"document.getElementById('bt').textContent=d.bluetooth||'BT-';\n"
"document.getElementById('gps').textContent=d.gps||'GPS-';\n"
"document.getElementById('uptime').textContent=d.uptime||'00:00:00:00';\n"
"document.getElementById('pwds').textContent=d.pwds||0;\n"
"document.getElementById('fhs').textContent=d.fhs||0;\n"
"document.getElementById('phs').textContent=d.phs||0;\n"
"document.getElementById('tcaps').textContent=d.tcaps||0;\n"
"document.getElementById('bat').textContent=d.battery||'BAT-';\n"
"document.getElementById('mode').textContent=d.mode||'AUTO';\n"
"var mt=d.memtemp||'';\n"
"var m=mt.match(/(\\d+)%\\s*(\\d+)%\\s*(\\d+)C/);\n"
"if(m){document.getElementById('mem').textContent=m[1]+'%';document.getElementById('cpu').textContent=m[2]+'%';document.getElementById('tmp').textContent=m[3]+'C';}\n"
"var faceImg=document.getElementById('face-img');\n"
"var faceTxt=document.getElementById('face-text');\n"
"if(d.face_img && d.face_img!=''){\n"
"if(lastFace!=d.face_img){faceImg.src='/face/'+d.face_img+'?t='+Date.now();lastFace=d.face_img;}\n"
"faceImg.style.display='block';faceTxt.style.display='none';\n"
"}else{\n"
"faceTxt.textContent=d.face||'';faceImg.style.display='none';faceTxt.style.display='block';\n"
"}\n"
"/* Update pwnhub stats */\n"
"if(d.pwnhub){\n"
"document.getElementById('pwnhub').classList.add('active');\n"
"document.getElementById('xp').textContent=d.xp||0;\n"
"document.getElementById('xpbar').style.width=(d.xp||0)+'%';\n"
"document.getElementById('lvl').textContent=d.lvl||0;\n"
"document.getElementById('title').textContent=d.title||'';\n"
"document.getElementById('wins').textContent=d.wins||0;\n"
"document.getElementById('losses').textContent=d.battles||0;\n"
"var mp=((d.food||0)+(d.strength||0)+(d.spirit||0))/3;\n"
"document.getElementById('m-food').style.visibility=(d.food||0)>0?'visible':'hidden';\n"
"document.getElementById('m-str').style.visibility=(d.strength||0)>0?'visible':'hidden';\n"
"document.getElementById('m-spr').style.visibility=(d.spirit||0)>0?'visible':'hidden';\n"
"}else{document.getElementById('pwnhub').classList.remove('active');}\n"
"}).catch(e=>console.log(e))}\n"
"u();setInterval(u,1000);\n"
"</script>\n"
"<a href='/crackcity' style='position:fixed;top:12px;right:12px;z-index:9999;"
"color:#0f0;font-family:monospace;font-size:12px;background:rgba(26,26,46,0.9);"
"padding:6px 14px;border:1px solid #0f3460;border-radius:6px;"
"text-decoration:none;backdrop-filter:blur(4px);"
"box-shadow:0 0 10px rgba(0,255,0,0.2);transition:all 0.3s'"
">&#127961; Crack City</a>\n"
"</body></html>\n";
static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}
int webserver_init(int port) {
    int server_fd;
    struct sockaddr_in addr;
    int opt = 1;
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("webserver socket");
        return -1;
    }
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_nonblocking(server_fd);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY); /* Bind all interfaces so phone/PC can access */
    addr.sin_port = htons(port);
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("webserver bind");
        close(server_fd);
        return -1;
    }
    if (listen(server_fd, 5) < 0) {
        perror("webserver listen");
        close(server_fd);
        return -1;
    }
    printf("[WEB] Server listening on 0.0.0.0:%d\n", port);
    return server_fd;
}
void webserver_set_state_callback(webserver_state_callback_t cb) {
    g_state_cb = cb;
}
static void send_response(int client_fd, const char *status, const char *content_type, const char *body, size_t body_len) {
    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        status, content_type, body_len);
    write(client_fd, header, header_len);
    if (body && body_len > 0) {
        write(client_fd, body, body_len);
    }
}
/* Serve a PNG file from theme directory */
static int serve_png(int client_fd, const char *filename) {
    char filepath[512];
    struct stat st;
    /* Sanitize filename - only allow alphanumeric, dash, underscore, dot */
    for (const char *p = filename; *p; p++) {
        if (!(*p >= 'a' && *p <= 'z') && !(*p >= 'A' && *p <= 'Z') &&
            !(*p >= '0' && *p <= '9') && *p != '-' && *p != '_' && *p != '.') {
            send_response(client_fd, "400 Bad Request", "text/plain", "Invalid filename", 16);
            return 0;
        }
    }
    /* Try current theme directory directly (default theme has PNGs in root) */
    snprintf(filepath, sizeof(filepath), "%s/%s/%s", THEME_BASE, g_current_theme, filename);
    if (stat(filepath, &st) != 0) {
        /* Try _faces subdirectory (some themes use this) */
        snprintf(filepath, sizeof(filepath), "%s/%s/_faces/%s", THEME_BASE, g_current_theme, filename);
        if (stat(filepath, &st) != 0) {
            /* Try default theme as fallback */
            snprintf(filepath, sizeof(filepath), "%s/default/%s", THEME_BASE, filename);
            if (stat(filepath, &st) != 0) {
                send_response(client_fd, "404 Not Found", "text/plain", "Face not found", 14);
                return 0;
            }
        }
    }
    /* Read and send file */
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        send_response(client_fd, "500 Internal Server Error", "text/plain", "Cannot read file", 16);
        return 0;
    }
    size_t filesize = st.st_size;
    char *data = malloc(filesize);
    if (!data) {
        fclose(f);
        send_response(client_fd, "500 Internal Server Error", "text/plain", "Out of memory", 13);
        return 0;
    }
    fread(data, 1, filesize, f);
    fclose(f);
    send_response(client_fd, "200 OK", "image/png", data, filesize);
    free(data);
    return 1;
}

/* === Name change API — POST /api/config/name === */
/* Updates config.toml, system hostname, and avahi so mDNS resolves the new name.
 * Mobile app sends: POST /api/config/name  {"name":"Sniffles"}
 * After this, http://sniffles.local/crackcity works. */
static void serve_config_name_api(int client_fd, const char *request, ssize_t req_len) {
    /* Find JSON body after headers (\r\n\r\n) */
    const char *body = strstr(request, "\r\n\r\n");
    if (!body) {
        const char *err = "{\"error\":\"no body\"}";
        send_response(client_fd, "400 Bad Request", "application/json", err, strlen(err));
        return;
    }
    body += 4;

    /* Parse "name" from JSON (simple extraction — no full JSON parser needed) */
    const char *nkey = strstr(body, "\"name\"");
    if (!nkey) {
        const char *err = "{\"error\":\"missing name field\"}";
        send_response(client_fd, "400 Bad Request", "application/json", err, strlen(err));
        return;
    }
    const char *q1 = strchr(nkey + 6, '"');
    if (!q1) { send_response(client_fd, "400 Bad Request", "application/json", "{\"error\":\"bad json\"}", 20); return; }
    const char *q2 = strchr(q1 + 1, '"');
    if (!q2) { send_response(client_fd, "400 Bad Request", "application/json", "{\"error\":\"bad json\"}", 20); return; }

    size_t name_len = q2 - q1 - 1;
    if (name_len == 0 || name_len > 24) {
        const char *err = "{\"error\":\"name must be 1-24 chars\"}";
        send_response(client_fd, "400 Bad Request", "application/json", err, strlen(err));
        return;
    }

    char new_name[64];
    memcpy(new_name, q1 + 1, name_len);
    new_name[name_len] = '\0';

    /* Validate: alphanumeric, hyphens, underscores only */
    for (size_t i = 0; i < name_len; i++) {
        char c = new_name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            const char *err = "{\"error\":\"invalid chars in name\"}";
            send_response(client_fd, "400 Bad Request", "application/json", err, strlen(err));
            return;
        }
    }

    /* 1. Update config.toml — read, replace name line, write back */
    {
        FILE *f = fopen("/etc/pwnagotchi/config.toml", "r");
        char *content = NULL;
        long fsize = 0;
        if (f) {
            fseek(f, 0, SEEK_END);
            fsize = ftell(f);
            fseek(f, 0, SEEK_SET);
            content = malloc(fsize + 1);
            if (content) { fread(content, 1, fsize, f); content[fsize] = '\0'; }
            fclose(f);
        }
        FILE *fw = fopen("/etc/pwnagotchi/config.toml", "w");
        if (fw) {
            if (content) {
                /* Replace existing name = "..." line */
                int replaced = 0;
                char *line_start = content;
                while (line_start && *line_start) {
                    char *line_end = strchr(line_start, '\n');
                    size_t ll = line_end ? (size_t)(line_end - line_start) : strlen(line_start);
                    /* Check if this is a top-level name = "..." (not inside [section]) */
                    if (!replaced && line_start[0] != '[' && strstr(line_start, "name") &&
                        strstr(line_start, "=") && (strstr(line_start, "=") < line_start + ll)) {
                        /* Make sure it's "name" not "phone-name" etc */
                        char *eq = strstr(line_start, "=");
                        char tmp[64];
                        size_t klen = eq - line_start;
                        if (klen < sizeof(tmp)) {
                            memcpy(tmp, line_start, klen); tmp[klen] = '\0';
                            /* Trim whitespace */
                            char *k = tmp; while (*k == ' ' || *k == '\t') k++;
                            char *ke = k + strlen(k) - 1;
                            while (ke > k && (*ke == ' ' || *ke == '\t')) *ke-- = '\0';
                            if (strcmp(k, "name") == 0) {
                                fprintf(fw, "name = \"%s\"\n", new_name);
                                replaced = 1;
                                line_start = line_end ? line_end + 1 : NULL;
                                continue;
                            }
                        }
                    }
                    fwrite(line_start, 1, ll, fw);
                    if (line_end) { fputc('\n', fw); line_start = line_end + 1; }
                    else break;
                }
                if (!replaced) {
                    /* No existing name line — add one at the top */
                    fprintf(fw, "name = \"%s\"\n", new_name);
                }
            } else {
                /* No existing config — create minimal one */
                fprintf(fw, "name = \"%s\"\n", new_name);
            }
            fclose(fw);
        }
        free(content);
    }

    /* 2. Update system hostname + avahi for mDNS */
    {
        char hostname[64];
        size_t hi = 0;
        for (size_t i = 0; i < name_len && hi < sizeof(hostname) - 1; i++) {
            char c = new_name[i];
            if ((c >= 'A' && c <= 'Z')) c += 32; /* lowercase */
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')
                hostname[hi++] = c;
        }
        hostname[hi] = '\0';
        if (hi == 0) strcpy(hostname, "pwnagotchi");

        /* Set hostname + /etc/hostname + restart avahi */
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
            "hostname '%s'; echo '%s' > /etc/hostname; "
            "sed -i 's/127.0.1.1.*/127.0.1.1\\t%s/' /etc/hosts; "
            "systemctl restart avahi-daemon 2>/dev/null",
            hostname, hostname, hostname);
        system(cmd);
    }

    /* 3. Notify pwnaui to update display name */
    if (g_name_cb) {
        g_name_cb(new_name);
    }

    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"name\":\"%s\"}", new_name);
    send_response(client_fd, "200 OK", "application/json", resp, strlen(resp));
}

/* === Sprint 5: Crack City API (v2 — pure SQL, no file scanning) === */
static void serve_crackcity_api(int client_fd) {
    /* Current GPS — prepend to the DB-generated JSON */
    char gps_json[256] = "\"current_gps\":{\"lat\":0,\"lon\":0,\"has_fix\":false}";
    if (g_gps_cb) {
        double lat = 0, lon = 0;
        int has_fix = 0;
        g_gps_cb(&lat, &lon, &has_fix);
        snprintf(gps_json, sizeof(gps_json),
            "\"current_gps\":{\"lat\":%.8f,\"lon\":%.8f,\"has_fix\":%s}",
            lat, lon, has_fix ? "true" : "false");
    }

    /* Get networks + stats from DB in one fast SQL query */
    char *db_json = NULL;
    if (ap_db_crackcity_json(&db_json) != 0 || !db_json) {
        const char *err = "{\"error\":\"db\"}";
        send_response(client_fd, "500 Internal Server Error", "application/json", err, strlen(err));
        return;
    }

    /* Merge: insert current_gps into the DB JSON */
    /* db_json is: {"networks":[...],"stats":{...}} */
    /* We want:    {"current_gps":{...},"networks":[...],"stats":{...}} */
    size_t gps_len = strlen(gps_json);
    size_t db_len = strlen(db_json);
    char *merged = malloc(gps_len + db_len + 4);  /* {gps,db_without_leading_brace} */
    if (!merged) {
        free(db_json);
        const char *err = "{\"error\":\"oom\"}";
        send_response(client_fd, "500 Internal Server Error", "application/json", err, strlen(err));
        return;
    }
    merged[0] = '{';
    memcpy(merged + 1, gps_json, gps_len);
    merged[1 + gps_len] = ',';
    /* Skip the leading '{' of db_json */
    memcpy(merged + 2 + gps_len, db_json + 1, db_len - 1);
    merged[1 + gps_len + db_len] = '\0';

    send_response(client_fd, "200 OK", "application/json", merged, strlen(merged));
    free(merged);
    free(db_json);
}

/* === Sprint 5: Attack Log API === */
static void serve_attacks_api(int client_fd) {
    char buf[65536];
    attack_log_to_json(buf, sizeof(buf), 100);
    send_response(client_fd, "200 OK", "application/json", buf, strlen(buf));
}

/* === Sprint 5: Serve HTML file from disk === */
static void serve_html_file(int client_fd, const char *filepath) {
    FILE *f = fopen(filepath, "r");
    if (!f) {
        const char *msg = "Page not found";
        send_response(client_fd, "404 Not Found", "text/plain", msg, strlen(msg));
        return;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = (char *)malloc(fsize + 1);
    if (!data) { fclose(f); return; }
    fread(data, 1, fsize, f);
    data[fsize] = '\0';
    fclose(f);
    send_response(client_fd, "200 OK", "text/html; charset=utf-8", data, fsize);
    free(data);
}

int webserver_poll(int server_fd) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    char request[2048];
    int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("webserver accept");
        }
        return 0;
    }
    /* Read request */
    ssize_t n = read(client_fd, request, sizeof(request) - 1);
    if (n <= 0) {
        close(client_fd);
        return 0;
    }
    request[n] = '\0';
    /* Parse request path */
    if (strncmp(request, "GET /api/state", 14) == 0) {
        /* JSON API endpoint */
        char json[2048];
        if (g_state_cb) {
            g_state_cb(json, sizeof(json));
        } else {
            strcpy(json, "{\"error\":\"no state callback\"}");
        }
        send_response(client_fd, "200 OK", "application/json", json, strlen(json));
    } else if (strncmp(request, "GET /face/", 10) == 0) {
        /* Serve PNG face image */
        char filename[256];
        char *end = strchr(request + 10, ' ');
        if (!end) end = strchr(request + 10, '?');
        if (!end) end = request + strlen(request);
        size_t len = end - (request + 10);
        if (len >= sizeof(filename)) len = sizeof(filename) - 1;
        /* Remove query string if present */
        char *query = memchr(request + 10, '?', len);
        if (query) len = query - (request + 10);
        strncpy(filename, request + 10, len);
        filename[len] = '\0';
        serve_png(client_fd, filename);
    } else if (strncmp(request, "GET /assets/", 12) == 0) {
        char filename[256];
        char *end = strchr(request + 12, ' ');
        if (!end) end = strchr(request + 12, '?');
        if (!end) end = request + strlen(request);
        size_t len = end - (request + 12);
        if (len >= sizeof(filename)) len = sizeof(filename) - 1;
        char *query = memchr(request + 12, '?', len);
        if (query) len = query - (request + 12);
        strncpy(filename, request + 12, len);
        filename[len] = '\0';

        /* T293: Sanitize filename — reject path traversal attempts */
        if (strstr(filename, "..") || strchr(filename, '\0') != filename + strlen(filename) ||
            memchr(filename, '\0', len) != NULL || strchr(filename, '/') || strchr(filename, '\\')) {
            const char *msg = "Invalid filename";
            send_response(client_fd, "400 Bad Request", "text/plain", msg, strlen(msg));
        } else {

        char filepath[512];
        snprintf(filepath, sizeof(filepath), "/home/pi/pwnaui/assets/%s", filename);
        FILE *fp = fopen(filepath, "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long fsize = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            unsigned char *data = (unsigned char *)malloc(fsize);
            if (data) {
                fread(data, 1, fsize, fp);
                fclose(fp);
                /* Determine content type from extension */
                const char *ctype = "application/octet-stream";
                if (strstr(filename, ".js"))       ctype = "application/javascript";
                else if (strstr(filename, ".css")) ctype = "text/css";
                else if (strstr(filename, ".png")) ctype = "image/png";
                else if (strstr(filename, ".jpg")) ctype = "image/jpeg";
                else if (strstr(filename, ".svg")) ctype = "image/svg+xml";
                else if (strstr(filename, ".json")) ctype = "application/json";
                send_response(client_fd, "200 OK", ctype, (const char *)data, fsize);
                free(data);
            } else {
                fclose(fp);
                const char *msg = "Memory error";
                send_response(client_fd, "500 Internal Server Error", "text/plain", msg, strlen(msg));
            }
        } else {
            const char *msg = "Asset not found";
            send_response(client_fd, "404 Not Found", "text/plain", msg, strlen(msg));
        }
        } /* end T293 sanitize else */
    } else if (strncmp(request, "GET /api/crackcity", 18) == 0) {
        /* Sprint 5: Crack City API */
        serve_crackcity_api(client_fd);

    } else if (strncmp(request, "GET /api/attacks", 16) == 0) {
        /* Sprint 5: Attack log API */
        serve_attacks_api(client_fd);

    } else if (strncmp(request, "POST /api/config/name", 21) == 0) {
        /* Name change API — mobile app sets pwnagotchi name */
        serve_config_name_api(client_fd, request, n);

    } else if (strncmp(request, "GET /crackcity", 14) == 0) {
        /* Sprint 5: Crack City page */
        serve_html_file(client_fd, "/home/pi/pwnaui/crackcity.html");

    } else if (strncmp(request, "GET / ", 6) == 0 || strncmp(request, "GET /index", 10) == 0) {
        /* Serve HTML page */
        send_response(client_fd, "200 OK", "text/html; charset=utf-8", HTML_PAGE, strlen(HTML_PAGE));
    } else {
        /* 404 */
        const char *msg = "Not Found";
        send_response(client_fd, "404 Not Found", "text/plain", msg, strlen(msg));
    }
    close(client_fd);
    return 1;
}
void webserver_cleanup(int server_fd) {
    if (server_fd >= 0) {
        close(server_fd);
    }
}
