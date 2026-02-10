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
#include "webserver.h"
#include "cJSON.h"
#include "attack_log.h"
#include <dirent.h>
static webserver_state_callback_t g_state_cb = NULL;
static webserver_gps_callback_t g_gps_cb = NULL;

void webserver_set_gps_callback(webserver_gps_callback_t cb) {
    g_gps_cb = cb;
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
"<img class='macro-icon' id='m-prot' src='/assets/Protein.png'>\n"
"<img class='macro-icon' id='m-fat' src='/assets/Fat.png'>\n"
"<img class='macro-icon' id='m-carb' src='/assets/carbs.png'>\n"
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
"var mp=((d.protein||0)+(d.fat||0)+(d.carbs||0))*100/150;\n"
"document.getElementById('m-prot').style.visibility=mp>=10?'visible':'hidden';\n"
"document.getElementById('m-fat').style.visibility=mp>=34?'visible':'hidden';\n"
"document.getElementById('m-carb').style.visibility=mp>=67?'visible':'hidden';\n"
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
    addr.sin_addr.s_addr = INADDR_ANY;
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
    printf("[WEB] Server listening on port %d\n", port);
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

/* === Sprint 5: Crack City API === */
static void serve_crackcity_api(int client_fd) {
    cJSON *root = cJSON_CreateObject();

    /* Current GPS */
    cJSON *gps_obj = cJSON_CreateObject();
    if (g_gps_cb) {
        double lat = 0, lon = 0;
        int has_fix = 0;
        g_gps_cb(&lat, &lon, &has_fix);
        cJSON_AddNumberToObject(gps_obj, "lat", lat);
        cJSON_AddNumberToObject(gps_obj, "lon", lon);
        cJSON_AddBoolToObject(gps_obj, "has_fix", has_fix);
    }
    cJSON_AddItemToObject(root, "current_gps", gps_obj);

    /* Scan handshakes directory */
    cJSON *networks = cJSON_CreateArray();
    int total = 0, with_gps = 0, cracked_count = 0;

    DIR *dir = opendir("/home/pi/handshakes");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            /* Only process .pcap files (not .pcapng duplicates) */
            size_t nlen = strlen(ent->d_name);
            if (nlen < 6) continue;
            if (strcmp(ent->d_name + nlen - 5, ".pcap") != 0) continue;
            /* Skip .pcapng (ends in .pcapng not .pcap) */
            if (nlen > 7 && strcmp(ent->d_name + nlen - 7, ".pcapng") == 0) continue;

            total++;

            /* Parse SSID and BSSID from filename: SSID_bssid.pcap */
            char ssid[64] = {0};
            char bssid_hex[13] = {0};
            char bssid_str[18] = {0};
            char *underscore = strrchr(ent->d_name, '_');
            if (!underscore) continue;

            size_t ssid_len = underscore - ent->d_name;
            if (ssid_len >= sizeof(ssid)) ssid_len = sizeof(ssid) - 1;
            strncpy(ssid, ent->d_name, ssid_len);

            char *bssid_start = underscore + 1;
            char *dot = strchr(bssid_start, '.');
            if (dot) {
                size_t blen = dot - bssid_start;
                if (blen == 12) {
                    strncpy(bssid_hex, bssid_start, 12);
                    snprintf(bssid_str, sizeof(bssid_str),
                        "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c",
                        bssid_hex[0],bssid_hex[1],bssid_hex[2],bssid_hex[3],
                        bssid_hex[4],bssid_hex[5],bssid_hex[6],bssid_hex[7],
                        bssid_hex[8],bssid_hex[9],bssid_hex[10],bssid_hex[11]);
                }
            }

            /* Read GPS data from .gps.json (named {SSID}_{BSSID}.gps.json) */
            double lat = 0, lon = 0;
            int has_gps = 0;
            char gps_path[512];
            /* Strip .pcap extension to get base name, then look for .gps.json */
            char baseName[256];
            strncpy(baseName, ent->d_name, sizeof(baseName) - 1);
            baseName[sizeof(baseName) - 1] = '\0';
            char *dotExt = strrchr(baseName, '.');
            if (dotExt) *dotExt = '\0';
            snprintf(gps_path, sizeof(gps_path), "/home/pi/handshakes/%s.gps.json", baseName);
            FILE *gf = fopen(gps_path, "r");
            if (gf) {
                char gps_buf[1024];
                size_t gn = fread(gps_buf, 1, sizeof(gps_buf)-1, gf);
                gps_buf[gn] = '\0';
                fclose(gf);
                cJSON *gj = cJSON_Parse(gps_buf);
                if (gj) {
                    cJSON *jlat = cJSON_GetObjectItem(gj, "Latitude");
                    if (!jlat) jlat = cJSON_GetObjectItem(gj, "latitude");
                    cJSON *jlon = cJSON_GetObjectItem(gj, "Longitude");
                    if (!jlon) jlon = cJSON_GetObjectItem(gj, "longitude");
                    if (jlat && cJSON_IsNumber(jlat)) lat = jlat->valuedouble;
                    if (jlon && cJSON_IsNumber(jlon)) lon = jlon->valuedouble;
                    if (lat != 0.0 || lon != 0.0) has_gps = 1;
                    cJSON_Delete(gj);
                }
                if (has_gps) with_gps++;
            }

            /* Check if cracked */
            char key_path[256], password[128] = {0};
            int is_cracked = 0;
            snprintf(key_path, sizeof(key_path), "/home/pi/cracked/%s.key", ssid);
            FILE *kf = fopen(key_path, "r");
            if (kf) {
                size_t kn = fread(password, 1, sizeof(password)-1, kf);
                password[kn] = '\0';
                char *nl = strchr(password, '\n');
                if (nl) *nl = '\0';
                char *cr = strchr(password, '\r');
                if (cr) *cr = '\0';
                fclose(kf);
                is_cracked = 1;
                cracked_count++;
            }

            /* Build network entry */
            cJSON *net = cJSON_CreateObject();
            cJSON_AddStringToObject(net, "ssid", ssid);
            cJSON_AddStringToObject(net, "bssid", bssid_str);
            cJSON_AddNumberToObject(net, "lat", lat);
            cJSON_AddNumberToObject(net, "lon", lon);
            cJSON_AddBoolToObject(net, "has_gps", has_gps);
            cJSON_AddBoolToObject(net, "cracked", is_cracked);
            cJSON_AddStringToObject(net, "password", password);
            cJSON_AddStringToObject(net, "filename", ent->d_name);
            cJSON_AddItemToArray(networks, net);
        }
        closedir(dir);
    }

    cJSON_AddItemToObject(root, "networks", networks);
    cJSON *stats = cJSON_CreateObject();
    cJSON_AddNumberToObject(stats, "total", total);
    cJSON_AddNumberToObject(stats, "with_gps", with_gps);
    cJSON_AddNumberToObject(stats, "cracked", cracked_count);
    cJSON_AddItemToObject(root, "stats", stats);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        send_response(client_fd, "200 OK", "application/json", json_str, strlen(json_str));
        free(json_str);
    } else {
        const char *err = "{\"error\":\"json\"}";
        send_response(client_fd, "500 Internal Server Error", "application/json", err, strlen(err));
    }
    cJSON_Delete(root);
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
    } else if (strncmp(request, "GET /api/crackcity", 18) == 0) {
        /* Sprint 5: Crack City API */
        serve_crackcity_api(client_fd);

    } else if (strncmp(request, "GET /api/attacks", 16) == 0) {
        /* Sprint 5: Attack log API */
        serve_attacks_api(client_fd);

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
