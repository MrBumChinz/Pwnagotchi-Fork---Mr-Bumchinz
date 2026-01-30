/*
 * bcap_ws.c - Bettercap WebSocket Client (Pure Socket Implementation)
 * 
 * Connects to bettercap's WebSocket API for real-time WiFi events.
 * Uses raw POSIX sockets - NO external library dependencies!
 * 
 * WebSocket Protocol (RFC 6455):
 *   1. HTTP Upgrade handshake
 *   2. Frame-based messaging with masking
 *   3. Ping/pong keepalive
 * 
 * Part of the PWND project - Phase 3 (WebSocket)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "bcap_ws.h"
#include "cJSON.h"

/* ==========================================================================
 * WebSocket Constants (RFC 6455)
 * ========================================================================== */

#define WS_OPCODE_CONT   0x00
#define WS_OPCODE_TEXT   0x01
#define WS_OPCODE_BIN    0x02
#define WS_OPCODE_CLOSE  0x08
#define WS_OPCODE_PING   0x09
#define WS_OPCODE_PONG   0x0A

#define WS_FIN_BIT       0x80
#define WS_MASK_BIT      0x80

/* Base64 encoding table for WebSocket key */
static const char b64_table[] = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* ==========================================================================
 * Internal Structures
 * ========================================================================== */

typedef enum {
    BCAP_STATE_DISCONNECTED = 0,
    BCAP_STATE_CONNECTING,
    BCAP_STATE_HANDSHAKE,
    BCAP_STATE_CONNECTED,
    BCAP_STATE_RECONNECTING,
    BCAP_STATE_CLOSING
} bcap_state_t;

struct bcap_ws_ctx {
    bcap_config_t config;
    
    /* Socket */
    int sock_fd;
    
    /* Connection state */
    bcap_state_t state;
    pthread_mutex_t state_lock;
    
    /* Receive buffer */
    char *rx_buffer;
    size_t rx_len;
    size_t rx_capacity;
    
    /* Frame assembly buffer */
    char *frame_buffer;
    size_t frame_len;
    size_t frame_capacity;
    
    /* State data (protected by lock) */
    pthread_mutex_t data_lock;
    bcap_ap_t aps[BCAP_MAX_APS];
    int ap_count;
    bcap_sta_t stas[BCAP_MAX_STAS];
    int sta_count;
    int handshake_count;
    
    /* Reconnection */
    time_t last_connect_attempt;
    int reconnect_count;
    int max_reconnect_attempts;
    int reconnect_delay_ms;
    
    /* Heartbeat */
    time_t last_ping_sent;
    time_t last_pong_recv;
    bool awaiting_pong;
    
    /* Background thread */
    pthread_t service_thread;
    volatile bool running;
    volatile bool thread_started;
};

/* ==========================================================================
 * Utility Functions
 * ========================================================================== */

void bcap_config_init(bcap_config_t *config) {
    if (!config) return;
    
    memset(config, 0, sizeof(*config));
    strncpy(config->host, BCAP_DEFAULT_HOST, sizeof(config->host) - 1);
    config->port = BCAP_DEFAULT_PORT;
    strncpy(config->path, BCAP_DEFAULT_PATH, sizeof(config->path) - 1);
    config->use_ssl = false;
    config->reconnect_delay_ms = BCAP_RECONNECT_DELAY_MS;
    config->heartbeat_interval_ms = BCAP_HEARTBEAT_MS;
    config->auto_reconnect = true;
    config->max_reconnect_attempts = 10;
    
    /* Default credentials (bettercap default) */
    strncpy(config->username, "pwnagotchi", sizeof(config->username) - 1);
    strncpy(config->password, "pwnagotchi", sizeof(config->password) - 1);
}

int bcap_parse_mac(const char *str, mac_addr_t *mac) {
    if (!str || !mac) return -1;
    
    unsigned int bytes[6];
    int ret = sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
                     &bytes[0], &bytes[1], &bytes[2],
                     &bytes[3], &bytes[4], &bytes[5]);
    
    if (ret != 6) return -1;
    
    for (int i = 0; i < 6; i++) {
        mac->addr[i] = (uint8_t)bytes[i];
    }
    return 0;
}

void bcap_format_mac(const mac_addr_t *mac, char *buf) {
    if (!mac || !buf) return;
    snprintf(buf, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac->addr[0], mac->addr[1], mac->addr[2],
             mac->addr[3], mac->addr[4], mac->addr[5]);
}

const char* bcap_event_type_name(bcap_event_type_t type) {
    switch (type) {
        case BCAP_EVT_NONE: return "none";
        case BCAP_EVT_AP_NEW: return "ap_new";
        case BCAP_EVT_AP_LOST: return "ap_lost";
        case BCAP_EVT_CLIENT_NEW: return "client_new";
        case BCAP_EVT_CLIENT_LOST: return "client_lost";
        case BCAP_EVT_CLIENT_PROBE: return "client_probe";
        case BCAP_EVT_HANDSHAKE: return "handshake";
        case BCAP_EVT_DEAUTH: return "deauth";
        case BCAP_EVT_CONNECTED: return "connected";
        case BCAP_EVT_DISCONNECTED: return "disconnected";
        case BCAP_EVT_ERROR: return "error";
        default: return "unknown";
    }
}

/* ==========================================================================
 * Base64 Encoding (for WebSocket key)
 * ========================================================================== */

static void base64_encode(const unsigned char *in, size_t len, char *out) {
    size_t i, j;
    for (i = 0, j = 0; i < len; i += 3, j += 4) {
        uint32_t v = in[i] << 16;
        if (i + 1 < len) v |= in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        
        out[j] = b64_table[(v >> 18) & 0x3F];
        out[j + 1] = b64_table[(v >> 12) & 0x3F];
        out[j + 2] = (i + 1 < len) ? b64_table[(v >> 6) & 0x3F] : '=';
        out[j + 3] = (i + 2 < len) ? b64_table[v & 0x3F] : '=';
    }
    out[j] = '\0';
}

/* Generate random WebSocket key */
static void generate_ws_key(char *key_out) {
    unsigned char raw[16];
    
    /* Use /dev/urandom for randomness */
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        read(fd, raw, 16);
        close(fd);
    } else {
        /* Fallback to time-based */
        srand(time(NULL) ^ getpid());
        for (int i = 0; i < 16; i++) {
            raw[i] = rand() & 0xFF;
        }
    }
    
    base64_encode(raw, 16, key_out);
}

/* ==========================================================================
 * Socket Operations
 * ========================================================================== */

static int set_socket_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_socket_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

static int socket_connect(const char *host, int port, int timeout_ms) {
    struct sockaddr_in addr;
    int sock;
    
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("[bcap_ws] socket()");
        return -1;
    }
    
    /* Set TCP_NODELAY for low latency */
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    
    /* Resolve host */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        /* Try DNS resolution */
        struct hostent *he = gethostbyname(host);
        if (!he) {
            fprintf(stderr, "[bcap_ws] Cannot resolve host: %s\n", host);
            close(sock);
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }
    
    /* Non-blocking connect with timeout */
    set_socket_nonblocking(sock);
    
    int ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        perror("[bcap_ws] connect()");
        close(sock);
        return -1;
    }
    
    if (ret < 0) {
        /* Wait for connection with timeout */
        fd_set wset;
        struct timeval tv;
        
        FD_ZERO(&wset);
        FD_SET(sock, &wset);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        
        ret = select(sock + 1, NULL, &wset, NULL, &tv);
        if (ret <= 0) {
            fprintf(stderr, "[bcap_ws] Connection timeout\n");
            close(sock);
            return -1;
        }
        
        /* Check for connection error */
        int error = 0;
        socklen_t len = sizeof(error);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len);
        if (error) {
            fprintf(stderr, "[bcap_ws] Connection failed: %s\n", strerror(error));
            close(sock);
            return -1;
        }
    }
    
    /* Back to blocking for simplicity */
    set_socket_blocking(sock);
    
    return sock;
}

/* ==========================================================================
 * WebSocket Frame Handling
 * ========================================================================== */

/* Send a WebSocket frame */
static int ws_send_frame(int sock, uint8_t opcode, const void *data, size_t len) {
    uint8_t header[14];
    size_t header_len = 2;
    uint8_t mask[4];
    
    /* Generate mask */
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        read(fd, mask, 4);
        close(fd);
    } else {
        for (int i = 0; i < 4; i++) mask[i] = rand() & 0xFF;
    }
    
    /* Build header */
    header[0] = WS_FIN_BIT | (opcode & 0x0F);
    
    if (len < 126) {
        header[1] = WS_MASK_BIT | len;
    } else if (len < 65536) {
        header[1] = WS_MASK_BIT | 126;
        header[2] = (len >> 8) & 0xFF;
        header[3] = len & 0xFF;
        header_len = 4;
    } else {
        header[1] = WS_MASK_BIT | 127;
        header[2] = 0; header[3] = 0; header[4] = 0; header[5] = 0;
        header[6] = (len >> 24) & 0xFF;
        header[7] = (len >> 16) & 0xFF;
        header[8] = (len >> 8) & 0xFF;
        header[9] = len & 0xFF;
        header_len = 10;
    }
    
    /* Add mask to header */
    memcpy(header + header_len, mask, 4);
    header_len += 4;
    
    /* Send header */
    if (send(sock, header, header_len, 0) != (ssize_t)header_len) {
        return -1;
    }
    
    /* Send masked payload */
    if (len > 0 && data) {
        uint8_t *masked = malloc(len);
        if (!masked) return -1;
        
        const uint8_t *src = (const uint8_t *)data;
        for (size_t i = 0; i < len; i++) {
            masked[i] = src[i] ^ mask[i % 4];
        }
        
        ssize_t sent = send(sock, masked, len, 0);
        free(masked);
        
        if (sent != (ssize_t)len) return -1;
    }
    
    return 0;
}

/* Send text message */
static int ws_send_text(int sock, const char *text) {
    return ws_send_frame(sock, WS_OPCODE_TEXT, text, strlen(text));
}

/* Send ping */
static int ws_send_ping(int sock) {
    return ws_send_frame(sock, WS_OPCODE_PING, NULL, 0);
}

/* Send pong */
static int ws_send_pong(int sock, const void *data, size_t len) {
    return ws_send_frame(sock, WS_OPCODE_PONG, data, len);
}

/* Send close */
static int ws_send_close(int sock) {
    return ws_send_frame(sock, WS_OPCODE_CLOSE, NULL, 0);
}

/* ==========================================================================
 * WebSocket Handshake
 * ========================================================================== */

static int ws_handshake(bcap_ws_ctx_t *ctx) {
    char ws_key[32];
    char request[1024];
    char response[2048];
    
    generate_ws_key(ws_key);
    
    /* Build HTTP upgrade request */
    /* Note: bettercap uses basic auth */
    char auth[128];
    snprintf(auth, sizeof(auth), "%s:%s", ctx->config.username, ctx->config.password);
    char auth_b64[256];
    base64_encode((unsigned char *)auth, strlen(auth), auth_b64);
    
    int req_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Authorization: Basic %s\r\n"
        "\r\n",
        ctx->config.path,
        ctx->config.host,
        ctx->config.port,
        ws_key,
        auth_b64);
    
    /* Send handshake request */
    if (send(ctx->sock_fd, request, req_len, 0) != req_len) {
        fprintf(stderr, "[bcap_ws] Failed to send handshake\n");
        return -1;
    }
    
    /* Receive response with timeout */
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(ctx->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    ssize_t recv_len = recv(ctx->sock_fd, response, sizeof(response) - 1, 0);
    if (recv_len <= 0) {
        fprintf(stderr, "[bcap_ws] No handshake response\n");
        return -1;
    }
    response[recv_len] = '\0';
    
    /* Check for 101 Switching Protocols */
    if (strstr(response, "101") == NULL) {
        fprintf(stderr, "[bcap_ws] Handshake rejected: %s\n", response);
        return -1;
    }
    
    /* Clear timeout */
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    setsockopt(ctx->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    printf("[bcap_ws] WebSocket handshake successful\n");
    return 0;
}

/* ==========================================================================
 * JSON Event Parsing
 * ========================================================================== */

static bcap_event_type_t parse_event_type(const char *tag) {
    if (!tag) return BCAP_EVT_NONE;
    
    if (strcmp(tag, "wifi.ap.new") == 0) return BCAP_EVT_AP_NEW;
    if (strcmp(tag, "wifi.ap.lost") == 0) return BCAP_EVT_AP_LOST;
    if (strcmp(tag, "wifi.client.new") == 0) return BCAP_EVT_CLIENT_NEW;
    if (strcmp(tag, "wifi.client.lost") == 0) return BCAP_EVT_CLIENT_LOST;
    if (strcmp(tag, "wifi.client.probe") == 0) return BCAP_EVT_CLIENT_PROBE;
    if (strcmp(tag, "wifi.client.handshake") == 0) return BCAP_EVT_HANDSHAKE;
    if (strcmp(tag, "wifi.deauthentication") == 0) return BCAP_EVT_DEAUTH;
    
    return BCAP_EVT_NONE;
}

static void parse_ap_json(cJSON *json, bcap_ap_t *ap) {
    if (!json || !ap) return;
    memset(ap, 0, sizeof(*ap));
    
    cJSON *item;
    
    if ((item = cJSON_GetObjectItem(json, "mac")) && item->valuestring)
        bcap_parse_mac(item->valuestring, &ap->bssid);
    
    if ((item = cJSON_GetObjectItem(json, "hostname")) && item->valuestring)
        strncpy(ap->ssid, item->valuestring, sizeof(ap->ssid) - 1);
    else if ((item = cJSON_GetObjectItem(json, "ssid")) && item->valuestring)
        strncpy(ap->ssid, item->valuestring, sizeof(ap->ssid) - 1);
    
    if ((item = cJSON_GetObjectItem(json, "rssi")))
        ap->rssi = (int8_t)item->valueint;
    
    if ((item = cJSON_GetObjectItem(json, "channel")))
        ap->channel = (uint8_t)item->valueint;
    
    if ((item = cJSON_GetObjectItem(json, "encryption")) && item->valuestring)
        strncpy(ap->encryption, item->valuestring, sizeof(ap->encryption) - 1);
    
    if ((item = cJSON_GetObjectItem(json, "vendor")) && item->valuestring)
        strncpy(ap->vendor, item->valuestring, sizeof(ap->vendor) - 1);
    
    if ((item = cJSON_GetObjectItem(json, "clients")) && cJSON_IsArray(item))
        ap->clients_count = cJSON_GetArraySize(item);
    
    if ((item = cJSON_GetObjectItem(json, "handshake")) && cJSON_IsBool(item))
        ap->handshake_captured = cJSON_IsTrue(item);
    
    ap->last_seen = time(NULL);
    if (ap->first_seen == 0) ap->first_seen = ap->last_seen;
}

static void parse_sta_json(cJSON *json, bcap_sta_t *sta) {
    if (!json || !sta) return;
    memset(sta, 0, sizeof(*sta));
    
    cJSON *item;
    
    if ((item = cJSON_GetObjectItem(json, "mac")) && item->valuestring)
        bcap_parse_mac(item->valuestring, &sta->mac);
    
    if ((item = cJSON_GetObjectItem(json, "ap")) && item->valuestring) {
        bcap_parse_mac(item->valuestring, &sta->ap_bssid);
        sta->associated = true;
    }
    
    if ((item = cJSON_GetObjectItem(json, "rssi")))
        sta->rssi = (int8_t)item->valueint;
    
    if ((item = cJSON_GetObjectItem(json, "vendor")) && item->valuestring)
        strncpy(sta->vendor, item->valuestring, sizeof(sta->vendor) - 1);
    
    sta->last_seen = time(NULL);
    if (sta->first_seen == 0) sta->first_seen = sta->last_seen;
}

static void parse_handshake_json(cJSON *json, bcap_handshake_t *hs) {
    if (!json || !hs) return;
    memset(hs, 0, sizeof(*hs));
    
    cJSON *item;
    
    if ((item = cJSON_GetObjectItem(json, "ap")) && item->valuestring)
        bcap_parse_mac(item->valuestring, &hs->ap_bssid);
    
    if ((item = cJSON_GetObjectItem(json, "station")) && item->valuestring)
        bcap_parse_mac(item->valuestring, &hs->client_mac);
    
    if ((item = cJSON_GetObjectItem(json, "ssid")) && item->valuestring)
        strncpy(hs->ssid, item->valuestring, sizeof(hs->ssid) - 1);
    
    if ((item = cJSON_GetObjectItem(json, "file")) && item->valuestring)
        strncpy(hs->pcap_file, item->valuestring, sizeof(hs->pcap_file) - 1);
    
    if ((item = cJSON_GetObjectItem(json, "pmkid")) && cJSON_IsBool(item))
        hs->pmkid = cJSON_IsTrue(item);
    
    if ((item = cJSON_GetObjectItem(json, "full")) && cJSON_IsBool(item))
        hs->full = cJSON_IsTrue(item);
    
    hs->captured_at = time(NULL);
}

/* ==========================================================================
 * Event Processing
 * ========================================================================== */

static void process_json_message(bcap_ws_ctx_t *ctx, const char *json_str) {
    if (!ctx || !json_str || !*json_str) return;
    
    cJSON *json = cJSON_Parse(json_str);
    if (!json) {
        /* Not all messages are JSON - could be plain text response */
        return;
    }
    
    bcap_event_t event;
    memset(&event, 0, sizeof(event));
    event.timestamp = time(NULL);
    
    /* Get event tag */
    cJSON *tag = cJSON_GetObjectItem(json, "tag");
    if (tag && tag->valuestring) {
        event.type = parse_event_type(tag->valuestring);
    }
    
    /* Get event data */
    cJSON *data = cJSON_GetObjectItem(json, "data");
    
    if (data && event.type != BCAP_EVT_NONE) {
        switch (event.type) {
            case BCAP_EVT_AP_NEW:
            case BCAP_EVT_AP_LOST:
                parse_ap_json(data, &event.data.ap);
                
                pthread_mutex_lock(&ctx->data_lock);
                if (event.type == BCAP_EVT_AP_NEW && ctx->ap_count < BCAP_MAX_APS) {
                    int found = -1;
                    for (int i = 0; i < ctx->ap_count; i++) {
                        if (memcmp(ctx->aps[i].bssid.addr, event.data.ap.bssid.addr, 6) == 0) {
                            found = i;
                            break;
                        }
                    }
                    if (found >= 0) {
                        ctx->aps[found] = event.data.ap;
                    } else {
                        ctx->aps[ctx->ap_count++] = event.data.ap;
                    }
                }
                pthread_mutex_unlock(&ctx->data_lock);
                break;
                
            case BCAP_EVT_CLIENT_NEW:
            case BCAP_EVT_CLIENT_LOST:
            case BCAP_EVT_CLIENT_PROBE:
                parse_sta_json(data, &event.data.sta);
                
                pthread_mutex_lock(&ctx->data_lock);
                if (event.type == BCAP_EVT_CLIENT_NEW && ctx->sta_count < BCAP_MAX_STAS) {
                    int found = -1;
                    for (int i = 0; i < ctx->sta_count; i++) {
                        if (memcmp(ctx->stas[i].mac.addr, event.data.sta.mac.addr, 6) == 0) {
                            found = i;
                            break;
                        }
                    }
                    if (found >= 0) {
                        ctx->stas[found] = event.data.sta;
                    } else {
                        ctx->stas[ctx->sta_count++] = event.data.sta;
                    }
                }
                pthread_mutex_unlock(&ctx->data_lock);
                break;
                
            case BCAP_EVT_HANDSHAKE:
                parse_handshake_json(data, &event.data.hs);
                pthread_mutex_lock(&ctx->data_lock);
                ctx->handshake_count++;
                pthread_mutex_unlock(&ctx->data_lock);
                break;
                
            default:
                break;
        }
        
        /* Call user callback */
        if (ctx->config.on_event) {
            ctx->config.on_event(&event, ctx->config.user_data);
        }
    }
    
    cJSON_Delete(json);
}

/* ==========================================================================
 * WebSocket Frame Receiver
 * ========================================================================== */

static int ws_recv_frame(bcap_ws_ctx_t *ctx) {
    uint8_t header[2];
    ssize_t n;
    
    /* Set short timeout for non-blocking feel */
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; /* 100ms */
    setsockopt(ctx->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    n = recv(ctx->sock_fd, header, 2, MSG_PEEK);
    if (n <= 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0; /* No data available */
        }
        return -1; /* Connection error */
    }
    
    /* Actually read header */
    n = recv(ctx->sock_fd, header, 2, 0);
    if (n != 2) return -1;
    
    uint8_t opcode = header[0] & 0x0F;
    bool fin = (header[0] & 0x80) != 0;
    bool masked = (header[1] & 0x80) != 0;
    uint64_t payload_len = header[1] & 0x7F;
    
    /* Extended length */
    if (payload_len == 126) {
        uint8_t ext[2];
        if (recv(ctx->sock_fd, ext, 2, 0) != 2) return -1;
        payload_len = (ext[0] << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8];
        if (recv(ctx->sock_fd, ext, 8, 0) != 8) return -1;
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | ext[i];
        }
    }
    
    /* Mask key (server shouldn't send masked frames, but handle it) */
    uint8_t mask[4] = {0};
    if (masked) {
        if (recv(ctx->sock_fd, mask, 4, 0) != 4) return -1;
    }
    
    /* Receive payload */
    if (payload_len > 0) {
        if (payload_len > ctx->frame_capacity) {
            /* Expand buffer */
            size_t new_cap = payload_len + 1024;
            char *new_buf = realloc(ctx->frame_buffer, new_cap);
            if (!new_buf) return -1;
            ctx->frame_buffer = new_buf;
            ctx->frame_capacity = new_cap;
        }
        
        size_t received = 0;
        while (received < payload_len) {
            n = recv(ctx->sock_fd, ctx->frame_buffer + received, 
                     payload_len - received, 0);
            if (n <= 0) return -1;
            received += n;
        }
        
        /* Unmask if needed */
        if (masked) {
            for (size_t i = 0; i < payload_len; i++) {
                ctx->frame_buffer[i] ^= mask[i % 4];
            }
        }
        
        ctx->frame_buffer[payload_len] = '\0';
        ctx->frame_len = payload_len;
    } else {
        ctx->frame_len = 0;
    }
    
    /* Handle control frames */
    switch (opcode) {
        case WS_OPCODE_PING:
            ws_send_pong(ctx->sock_fd, ctx->frame_buffer, ctx->frame_len);
            return 0;
            
        case WS_OPCODE_PONG:
            ctx->last_pong_recv = time(NULL);
            ctx->awaiting_pong = false;
            return 0;
            
        case WS_OPCODE_CLOSE:
            printf("[bcap_ws] Server sent close frame\n");
            return -1;
            
        case WS_OPCODE_TEXT:
            if (fin && ctx->frame_len > 0) {
                process_json_message(ctx, ctx->frame_buffer);
            }
            return 1; /* Message received */
            
        case WS_OPCODE_BIN:
            /* Binary frames - ignore for now */
            return 0;
            
        default:
            return 0;
    }
}

/* ==========================================================================
 * Reconnection Logic
 * ========================================================================== */

static int attempt_reconnect(bcap_ws_ctx_t *ctx) {
    if (!ctx->config.auto_reconnect) {
        return -1;
    }
    
    if (ctx->reconnect_count >= ctx->max_reconnect_attempts) {
        fprintf(stderr, "[bcap_ws] Max reconnection attempts reached\n");
        return -1;
    }
    
    /* Exponential backoff with jitter */
    int delay = ctx->reconnect_delay_ms * (1 << ctx->reconnect_count);
    if (delay > 30000) delay = 30000; /* Cap at 30 seconds */
    delay += (rand() % 1000); /* Add jitter */
    
    printf("[bcap_ws] Reconnecting in %d ms (attempt %d/%d)...\n",
           delay, ctx->reconnect_count + 1, ctx->max_reconnect_attempts);
    
    usleep(delay * 1000);
    
    ctx->reconnect_count++;
    
    /* Close old socket */
    if (ctx->sock_fd >= 0) {
        close(ctx->sock_fd);
        ctx->sock_fd = -1;
    }
    
    /* Try to connect */
    ctx->sock_fd = socket_connect(ctx->config.host, ctx->config.port, 5000);
    if (ctx->sock_fd < 0) {
        return -1;
    }
    
    /* WebSocket handshake */
    if (ws_handshake(ctx) < 0) {
        close(ctx->sock_fd);
        ctx->sock_fd = -1;
        return -1;
    }
    
    /* Success! */
    printf("[bcap_ws] Reconnected successfully\n");
    ctx->reconnect_count = 0;
    
    pthread_mutex_lock(&ctx->state_lock);
    ctx->state = BCAP_STATE_CONNECTED;
    pthread_mutex_unlock(&ctx->state_lock);
    
    /* Notify callback */
    if (ctx->config.on_state_change) {
        ctx->config.on_state_change(true, ctx->config.user_data);
    }
    
    /* Re-subscribe to events */
    bcap_subscribe(ctx, "wifi.events");
    
    return 0;
}

/* ==========================================================================
 * Background Service Thread
 * ========================================================================== */

static void* service_thread_func(void *arg) {
    bcap_ws_ctx_t *ctx = (bcap_ws_ctx_t *)arg;
    
    printf("[bcap_ws] Service thread started\n");
    
    while (ctx->running) {
        pthread_mutex_lock(&ctx->state_lock);
        bcap_state_t state = ctx->state;
        pthread_mutex_unlock(&ctx->state_lock);
        
        if (state == BCAP_STATE_CONNECTED) {
            /* Receive frames */
            int ret = ws_recv_frame(ctx);
            
            if (ret < 0) {
                /* Connection lost */
                printf("[bcap_ws] Connection lost\n");
                
                pthread_mutex_lock(&ctx->state_lock);
                ctx->state = BCAP_STATE_RECONNECTING;
                pthread_mutex_unlock(&ctx->state_lock);
                
                if (ctx->config.on_state_change) {
                    ctx->config.on_state_change(false, ctx->config.user_data);
                }
                
                /* Try to reconnect */
                if (attempt_reconnect(ctx) < 0) {
                    pthread_mutex_lock(&ctx->state_lock);
                    ctx->state = BCAP_STATE_DISCONNECTED;
                    pthread_mutex_unlock(&ctx->state_lock);
                }
            }
            
            /* Heartbeat check */
            time_t now = time(NULL);
            if (now - ctx->last_ping_sent > (ctx->config.heartbeat_interval_ms / 1000)) {
                ws_send_ping(ctx->sock_fd);
                ctx->last_ping_sent = now;
                ctx->awaiting_pong = true;
            }
            
            /* Pong timeout check */
            if (ctx->awaiting_pong && (now - ctx->last_ping_sent > 10)) {
                printf("[bcap_ws] Pong timeout - connection dead\n");
                pthread_mutex_lock(&ctx->state_lock);
                ctx->state = BCAP_STATE_RECONNECTING;
                pthread_mutex_unlock(&ctx->state_lock);
            }
            
        } else if (state == BCAP_STATE_RECONNECTING) {
            if (attempt_reconnect(ctx) < 0) {
                pthread_mutex_lock(&ctx->state_lock);
                ctx->state = BCAP_STATE_DISCONNECTED;
                pthread_mutex_unlock(&ctx->state_lock);
            }
        } else {
            /* Not connected - sleep */
            usleep(100000);
        }
    }
    
    printf("[bcap_ws] Service thread exiting\n");
    return NULL;
}

/* ==========================================================================
 * Public API
 * ========================================================================== */

bcap_ws_ctx_t* bcap_create(const bcap_config_t *config) {
    bcap_ws_ctx_t *ctx = calloc(1, sizeof(bcap_ws_ctx_t));
    if (!ctx) return NULL;
    
    if (config) {
        ctx->config = *config;
    } else {
        bcap_config_init(&ctx->config);
    }
    
    pthread_mutex_init(&ctx->state_lock, NULL);
    pthread_mutex_init(&ctx->data_lock, NULL);
    
    ctx->rx_capacity = BCAP_RX_BUFFER_SIZE;
    ctx->rx_buffer = malloc(ctx->rx_capacity);
    ctx->frame_capacity = 16384;
    ctx->frame_buffer = malloc(ctx->frame_capacity);
    
    if (!ctx->rx_buffer || !ctx->frame_buffer) {
        free(ctx->rx_buffer);
        free(ctx->frame_buffer);
        free(ctx);
        return NULL;
    }
    
    ctx->sock_fd = -1;
    ctx->state = BCAP_STATE_DISCONNECTED;
    ctx->max_reconnect_attempts = ctx->config.max_reconnect_attempts;
    ctx->reconnect_delay_ms = ctx->config.reconnect_delay_ms;
    
    return ctx;
}

void bcap_destroy(bcap_ws_ctx_t *ctx) {
    if (!ctx) return;
    
    bcap_disconnect(ctx);
    
    pthread_mutex_destroy(&ctx->state_lock);
    pthread_mutex_destroy(&ctx->data_lock);
    
    free(ctx->rx_buffer);
    free(ctx->frame_buffer);
    free(ctx);
}

int bcap_connect(bcap_ws_ctx_t *ctx) {
    if (!ctx) return -1;
    
    printf("[bcap_ws] Connecting to %s:%d%s...\n",
           ctx->config.host, ctx->config.port, ctx->config.path);
    
    pthread_mutex_lock(&ctx->state_lock);
    ctx->state = BCAP_STATE_CONNECTING;
    pthread_mutex_unlock(&ctx->state_lock);
    
    /* TCP connect */
    ctx->sock_fd = socket_connect(ctx->config.host, ctx->config.port, 5000);
    if (ctx->sock_fd < 0) {
        pthread_mutex_lock(&ctx->state_lock);
        ctx->state = BCAP_STATE_DISCONNECTED;
        pthread_mutex_unlock(&ctx->state_lock);
        return -1;
    }
    
    /* WebSocket handshake */
    pthread_mutex_lock(&ctx->state_lock);
    ctx->state = BCAP_STATE_HANDSHAKE;
    pthread_mutex_unlock(&ctx->state_lock);
    
    if (ws_handshake(ctx) < 0) {
        close(ctx->sock_fd);
        ctx->sock_fd = -1;
        pthread_mutex_lock(&ctx->state_lock);
        ctx->state = BCAP_STATE_DISCONNECTED;
        pthread_mutex_unlock(&ctx->state_lock);
        return -1;
    }
    
    pthread_mutex_lock(&ctx->state_lock);
    ctx->state = BCAP_STATE_CONNECTED;
    pthread_mutex_unlock(&ctx->state_lock);
    
    ctx->last_ping_sent = time(NULL);
    ctx->last_pong_recv = time(NULL);
    ctx->reconnect_count = 0;
    
    /* Notify callback */
    if (ctx->config.on_state_change) {
        ctx->config.on_state_change(true, ctx->config.user_data);
    }
    
    return 0;
}

int bcap_connect_async(bcap_ws_ctx_t *ctx) {
    if (!ctx) return -1;
    
    /* Connect synchronously first */
    if (bcap_connect(ctx) < 0) {
        return -1;
    }
    
    /* Start background thread */
    ctx->running = true;
    if (pthread_create(&ctx->service_thread, NULL, service_thread_func, ctx) != 0) {
        bcap_disconnect(ctx);
        return -1;
    }
    ctx->thread_started = true;
    
    return 0;
}

void bcap_disconnect(bcap_ws_ctx_t *ctx) {
    if (!ctx) return;
    
    ctx->running = false;
    
    if (ctx->thread_started) {
        pthread_join(ctx->service_thread, NULL);
        ctx->thread_started = false;
    }
    
    if (ctx->sock_fd >= 0) {
        ws_send_close(ctx->sock_fd);
        close(ctx->sock_fd);
        ctx->sock_fd = -1;
    }
    
    pthread_mutex_lock(&ctx->state_lock);
    ctx->state = BCAP_STATE_DISCONNECTED;
    pthread_mutex_unlock(&ctx->state_lock);
    
    if (ctx->config.on_state_change) {
        ctx->config.on_state_change(false, ctx->config.user_data);
    }
}

bool bcap_is_connected(bcap_ws_ctx_t *ctx) {
    if (!ctx) return false;
    
    pthread_mutex_lock(&ctx->state_lock);
    bool connected = (ctx->state == BCAP_STATE_CONNECTED);
    pthread_mutex_unlock(&ctx->state_lock);
    
    return connected;
}

int bcap_poll(bcap_ws_ctx_t *ctx, int timeout_ms) {
    if (!ctx || ctx->sock_fd < 0) return -1;
    
    /* Use select for timeout */
    fd_set rset;
    struct timeval tv;
    
    FD_ZERO(&rset);
    FD_SET(ctx->sock_fd, &rset);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    
    int ret = select(ctx->sock_fd + 1, &rset, NULL, NULL, &tv);
    if (ret > 0 && FD_ISSET(ctx->sock_fd, &rset)) {
        return ws_recv_frame(ctx);
    }
    
    return 0;
}

int bcap_subscribe(bcap_ws_ctx_t *ctx, const char *stream) {
    if (!ctx || !stream || ctx->sock_fd < 0) return -1;
    
    char cmd[256];
    snprintf(cmd, sizeof(cmd), 
             "{\"cmd\":\"events.stream\",\"args\":{\"filter\":\"%s\"}}", stream);
    
    printf("[bcap_ws] Subscribing to: %s\n", stream);
    return ws_send_text(ctx->sock_fd, cmd);
}

int bcap_send_command(bcap_ws_ctx_t *ctx, const char *cmd) {
    if (!ctx || !cmd || ctx->sock_fd < 0) return -1;
    
    char json[512];
    snprintf(json, sizeof(json), "{\"cmd\":\"%s\"}", cmd);
    
    return ws_send_text(ctx->sock_fd, json);
}

int bcap_refresh_aps(bcap_ws_ctx_t *ctx) {
    return bcap_send_command(ctx, "wifi.show");
}

int bcap_refresh_stations(bcap_ws_ctx_t *ctx) {
    return bcap_send_command(ctx, "wifi.show");
}

/* ==========================================================================
 * State Access (Thread-safe)
 * ========================================================================== */

int bcap_get_ap_count(bcap_ws_ctx_t *ctx) {
    if (!ctx) return 0;
    pthread_mutex_lock(&ctx->data_lock);
    int count = ctx->ap_count;
    pthread_mutex_unlock(&ctx->data_lock);
    return count;
}

int bcap_get_sta_count(bcap_ws_ctx_t *ctx) {
    if (!ctx) return 0;
    pthread_mutex_lock(&ctx->data_lock);
    int count = ctx->sta_count;
    pthread_mutex_unlock(&ctx->data_lock);
    return count;
}

int bcap_get_handshake_count(bcap_ws_ctx_t *ctx) {
    if (!ctx) return 0;
    pthread_mutex_lock(&ctx->data_lock);
    int count = ctx->handshake_count;
    pthread_mutex_unlock(&ctx->data_lock);
    return count;
}

int bcap_get_ap(bcap_ws_ctx_t *ctx, int index, bcap_ap_t *ap) {
    if (!ctx || !ap || index < 0) return -1;
    
    pthread_mutex_lock(&ctx->data_lock);
    if (index >= ctx->ap_count) {
        pthread_mutex_unlock(&ctx->data_lock);
        return -1;
    }
    *ap = ctx->aps[index];
    pthread_mutex_unlock(&ctx->data_lock);
    
    return 0;
}

int bcap_find_ap(bcap_ws_ctx_t *ctx, const mac_addr_t *bssid, bcap_ap_t *ap) {
    if (!ctx || !bssid || !ap) return -1;
    
    pthread_mutex_lock(&ctx->data_lock);
    for (int i = 0; i < ctx->ap_count; i++) {
        if (memcmp(ctx->aps[i].bssid.addr, bssid->addr, 6) == 0) {
            *ap = ctx->aps[i];
            pthread_mutex_unlock(&ctx->data_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&ctx->data_lock);
    
    return -1;
}
