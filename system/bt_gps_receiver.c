/*
 * bt_gps_receiver.c - Bluetooth GPS Receiver for Pwnagotchi
 * 
 * Receives GPS data from PwnHub Android app via Bluetooth SPP
 * Writes GPS coordinates to /tmp/gps.json for Pwnagotchi to read
 * 
 * Compile: gcc -o bt_gps_receiver bt_gps_receiver.c -lbluetooth -lpthread
 * Run: sudo ./bt_gps_receiver
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <math.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <sys/un.h>

#define GPS_OUTPUT_FILE "/tmp/gps.json"
#define PWNAUI_SOCKET_PATH "/var/run/pwnaui.sock"
#define GPS_COORDS_FILE "/tmp/gps_coords.txt"
#define BUFFER_SIZE 2048
#define RFCOMM_CHANNEL 1
#define PWNAUI_GPS_PORT 5000

// SPP UUID: 00001101-0000-1000-8000-00805f9b34fb
static uint8_t spp_uuid[] = {
    0x00, 0x00, 0x11, 0x01, 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb
};

static volatile int running = 1;
static sdp_session_t *sdp_session = NULL;
static int server_sock = -1;

// GPS data structure
typedef struct {
    double latitude;
    double longitude;
    double altitude;
    double accuracy;
    double speed;
    double bearing;
    long timestamp;
    double accel;       // RMS linear acceleration m/s² (from phone accelerometer)
    int steps;          // Cumulative step count (from phone step counter)
    char activity[16];  // Android Activity Recognition: STILL/WALKING/IN_VEHICLE/etc
    int activity_confidence;  // 0-100 confidence from Activity Recognition API
    double displacement_speed; // m/s from GPS position changes (Samsung Doppler fallback)
    char calibration[4];  // Ground truth from app calibration UI: STA/WLK/DRV or empty
    int valid;
} gps_data_t;

static gps_data_t current_gps = {0};
static pthread_mutex_t gps_mutex = PTHREAD_MUTEX_INITIALIZER;
static int udp_sock = -1;
static struct sockaddr_in pwnaui_addr;
static volatile int bt_connected = 0;  // Track BT connection state
static volatile int active_client_fd = -1;  // Current client socket (-1 = none)
static time_t last_connect_log = 0;
static time_t last_disconnect_log = 0;

// Raw NMEA passthrough tracking: when the phone sends real GNSS NMEA
// (with chipset Doppler speed), don't generate synthetic VTG/RMC that
// would overwrite the real speed with Location.getSpeed()=0.
static volatile time_t last_raw_nmea_time = 0;
static volatile uint32_t raw_nmea_fwd_count = 0;
static volatile uint32_t vtg_enriched_count = 0;  // Count of VTG/RMC enriched with Location speed
#define RAW_NMEA_FRESH_SEC  5  // Raw NMEA is "fresh" if received within 5s

// Location API speed (m/s) from Android's sensor-fused Location.getSpeed().
// Updated when bt_gps_receiver receives a Location JSON from the phone.
// Samsung GNSS chipsets report VTG Doppler speed=0 below ~5 km/h, but
// Location.getSpeed() uses accelerometer fusion and detects walking.
static volatile float last_location_speed_mps = 0.0f;

// Send command to pwnaui via UNIX socket
int send_pwnaui_cmd(const char *cmd) {
    int sock;
    struct sockaddr_un addr;
    
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }
    
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, PWNAUI_SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    
    write(sock, cmd, strlen(cmd));
    close(sock);
    return 0;
}

// Update BT status in pwnaui
void update_bt_status(int connected) {
    if (connected) {
        send_pwnaui_cmd("SET_BLUETOOTH BT+\n");
    } else {
        send_pwnaui_cmd("SET_BLUETOOTH BT-\n");
    }
}

// Update GPS status in pwnaui
void update_gps_status(const gps_data_t *gps) {
    if (gps && gps->valid) {
        send_pwnaui_cmd("SET_GPS GPS+\n");
    } else {
        send_pwnaui_cmd("SET_GPS GPS-\n");
    }
}

void signal_handler(int sig) {
    printf("\nReceived signal %d, shutting down...\n", sig);
    running = 0;
}

// Register SPP service with SDP
sdp_session_t *register_service(uint8_t rfcomm_channel) {
    uint32_t svc_uuid_int[] = {0x1101}; // Serial Port Profile
    sdp_list_t *l2cap_list = 0,
               *rfcomm_list = 0,
               *root_list = 0,
               *proto_list = 0,
               *access_proto_list = 0,
               *svc_class_list = 0,
               *profile_list = 0;
    sdp_data_t *channel = 0;
    sdp_profile_desc_t profile;
    sdp_record_t record = {0};
    sdp_session_t *session = 0;
    uuid_t root_uuid, l2cap_uuid, rfcomm_uuid, svc_uuid, svc_class_uuid;

    // Set general service ID
    sdp_uuid128_create(&svc_uuid, &spp_uuid);
    sdp_set_service_id(&record, svc_uuid);

    // Set service class
    sdp_uuid16_create(&svc_class_uuid, SERIAL_PORT_SVCLASS_ID);
    svc_class_list = sdp_list_append(0, &svc_class_uuid);
    sdp_set_service_classes(&record, svc_class_list);

    // Set Bluetooth profile info
    sdp_uuid16_create(&profile.uuid, SERIAL_PORT_PROFILE_ID);
    profile.version = 0x0100;
    profile_list = sdp_list_append(0, &profile);
    sdp_set_profile_descs(&record, profile_list);

    // Make service publicly browsable
    sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
    root_list = sdp_list_append(0, &root_uuid);
    sdp_set_browse_groups(&record, root_list);

    // Set L2CAP info
    sdp_uuid16_create(&l2cap_uuid, L2CAP_UUID);
    l2cap_list = sdp_list_append(0, &l2cap_uuid);
    proto_list = sdp_list_append(0, l2cap_list);

    // Set RFCOMM info
    sdp_uuid16_create(&rfcomm_uuid, RFCOMM_UUID);
    channel = sdp_data_alloc(SDP_UINT8, &rfcomm_channel);
    rfcomm_list = sdp_list_append(0, &rfcomm_uuid);
    sdp_list_append(rfcomm_list, channel);
    sdp_list_append(proto_list, rfcomm_list);

    access_proto_list = sdp_list_append(0, proto_list);
    sdp_set_access_protos(&record, access_proto_list);

    // Set service name
    sdp_set_info_attr(&record, "PwnHub GPS Receiver", "Pwnagotchi", "GPS via Bluetooth SPP");

    // Connect to local SDP server
    session = sdp_connect(BDADDR_ANY, BDADDR_LOCAL, SDP_RETRY_IF_BUSY);
    if (session == NULL) {
        fprintf(stderr, "Failed to connect to SDP server: %s\n", strerror(errno));
        return NULL;
    }

    // Register service
    if (sdp_record_register(session, &record, 0) < 0) {
        fprintf(stderr, "Failed to register SDP record: %s\n", strerror(errno));
        sdp_close(session);
        return NULL;
    }

    // Cleanup
    sdp_data_free(channel);
    sdp_list_free(l2cap_list, 0);
    sdp_list_free(rfcomm_list, 0);
    sdp_list_free(root_list, 0);
    sdp_list_free(access_proto_list, 0);
    sdp_list_free(svc_class_list, 0);
    sdp_list_free(profile_list, 0);

    return session;
}

// Simple JSON string parser — copies value into buf (max buflen-1 chars)
int parse_json_string(const char *json, const char *key, char *buf, int buflen) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    char *pos = strstr(json, search);
    if (!pos) { buf[0] = '\0'; return -1; }
    pos = strchr(pos, ':');
    if (!pos) { buf[0] = '\0'; return -1; }
    pos++;
    while (*pos == ' ' || *pos == '\t') pos++;
    if (*pos != '"') { buf[0] = '\0'; return -1; }
    pos++;  // skip opening quote
    char *end = strchr(pos, '"');
    if (!end) { buf[0] = '\0'; return -1; }
    int len = (int)(end - pos);
    if (len >= buflen) len = buflen - 1;
    memcpy(buf, pos, len);
    buf[len] = '\0';
    return len;
}

// Simple JSON number parser
double parse_json_number(const char *json, const char *key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    
    char *pos = strstr(json, search);
    if (!pos) return 0.0;
    
    pos = strchr(pos, ':');
    if (!pos) return 0.0;
    
    pos++;
    while (*pos == ' ' || *pos == '\t') pos++;
    
    return atof(pos);
}

// Forward raw NMEA sentence from phone to gps.c via UDP
// Returns 1 if this was an NMEA passthrough message, 0 otherwise
int try_forward_raw_nmea(const char *json) {
    if (!json) return 0;
    
    // Look for "nmea" key — this indicates a raw NMEA passthrough
    char *nmea_key = strstr(json, "\"nmea\"");
    if (!nmea_key) return 0;
    
    // Extract the NMEA sentence string value
    char *colon = strchr(nmea_key, ':');
    if (!colon) return 0;
    
    char *quote_start = strchr(colon, '"');
    if (!quote_start) return 0;
    quote_start++;  // skip opening quote
    
    char *quote_end = strchr(quote_start, '"');
    if (!quote_end || (quote_end - quote_start) > 200) return 0;
    
    // Copy the NMEA sentence
    char nmea[256];
    int len = (int)(quote_end - quote_start);
    memcpy(nmea, quote_start, len);
    
    // Ensure proper NMEA line ending
    if (len >= 2 && nmea[len-2] == '\r' && nmea[len-1] == '\n') {
        nmea[len] = '\0';
    } else if (len >= 1 && nmea[len-1] == '\n') {
        // Insert \r before \n
        nmea[len-1] = '\r';
        nmea[len] = '\n';
        nmea[len+1] = '\0';
    } else {
        nmea[len] = '\r';
        nmea[len+1] = '\n';
        nmea[len+2] = '\0';
    }
    
    // Only forward GPS-relevant sentences (accept any talker: $GP, $GN, $GL, $GA)
    if (nmea[0] != '$' || len < 7) return 0;
    const char *type = nmea + 3;  // Skip $xx to get sentence type
    if (strncmp(type, "VTG", 3) == 0 ||
        strncmp(type, "RMC", 3) == 0 ||
        strncmp(type, "GGA", 3) == 0) {

        last_raw_nmea_time = time(NULL);
        raw_nmea_fwd_count++;

        /* ---- VTG/RMC speed enrichment ----
         * Samsung GNSS chipsets report Doppler speed=0 below ~5 km/h in VTG/RMC,
         * but Android's Location.getSpeed() (sensor fusion) IS non-zero when walking.
         * When raw VTG/RMC has speed=0 and Location speed is available, suppress
         * the zero-speed sentence — write_gps_file() will send a synthetic VTG/RMC
         * with the fused Location speed instead. */
        if (strncmp(type, "VTG", 3) == 0 && last_location_speed_mps > 0.3f) {
            /* Parse VTG speed_kmh (field 6): $xxVTG,courseT,T,courseM,M,spdKn,N,spdKmh,K,mode */
            char *pv = nmea + 7;
            int commas = 0;
            while (*pv && commas < 6) { if (*pv == ',') commas++; pv++; }
            float vtg_speed_kmh = (*pv && *pv != ',' && *pv != '*') ? atof(pv) : 0.0f;
            if (vtg_speed_kmh < 0.5f) {
                /* Doppler speed is zero — suppress, let synthetic VTG carry Location speed */
                vtg_enriched_count++;
                if ((vtg_enriched_count % 60) == 1) {
                    printf("[NMEA] Suppressed zero-speed VTG #%u (Location=%.1fm/s), synthetic will override\n",
                           vtg_enriched_count, last_location_speed_mps);
                }
                return 1;  // consumed but not forwarded
            }
        }
        if (strncmp(type, "RMC", 3) == 0 && last_location_speed_mps > 0.3f) {
            /* Parse RMC speed_knots (field 6): $xxRMC,time,status,lat,dir,lon,dir,spdKn,... */
            char *pr = nmea + 7;
            int commas = 0;
            while (*pr && commas < 6) { if (*pr == ',') commas++; pr++; }
            float rmc_speed_kn = (*pr && *pr != ',' && *pr != '*') ? atof(pr) : 0.0f;
            if (rmc_speed_kn < 0.3f) {
                /* Doppler speed is zero — suppress (synthetic RMC will carry Location speed) */
                return 1;
            }
        }

        // Forward raw NMEA (GGA always, VTG/RMC only when Doppler speed is non-zero)
        if (udp_sock >= 0) {
            sendto(udp_sock, nmea, strlen(nmea), 0,
                   (struct sockaddr *)&pwnaui_addr, sizeof(pwnaui_addr));
            if ((raw_nmea_fwd_count % 60) == 1) {
                printf("[NMEA] Forwarded raw NMEA #%u: %.40s...\n",
                       raw_nmea_fwd_count, nmea);
            }
        }
        return 1;
    }
    
    return 0;
}

// Parse GPS JSON data
int parse_gps_json(const char *json, gps_data_t *gps) {
    if (!json || !gps) return -1;
    
    // Check if it looks like JSON
    if (strchr(json, '{') == NULL) return -1;
    
    gps->latitude = parse_json_number(json, "latitude");
    gps->longitude = parse_json_number(json, "longitude");
    gps->altitude = parse_json_number(json, "altitude");
    gps->accuracy = parse_json_number(json, "accuracy");
    gps->speed = parse_json_number(json, "speed");
    gps->bearing = parse_json_number(json, "bearing");
    gps->timestamp = (long)parse_json_number(json, "timestamp");
    gps->accel = parse_json_number(json, "accel");
    gps->steps = (int)parse_json_number(json, "steps");
    parse_json_string(json, "activity", gps->activity, sizeof(gps->activity));
    gps->activity_confidence = (int)parse_json_number(json, "activityConfidence");
    gps->displacement_speed = parse_json_number(json, "displacementSpeed");
    parse_json_string(json, "calibration", gps->calibration, sizeof(gps->calibration));
    
    // Validate - latitude and longitude should be non-zero for valid GPS
    if (gps->latitude != 0.0 || gps->longitude != 0.0) {
        gps->valid = 1;
        return 0;
    }
    
    return -1;
}

/* Forward declaration — write_gps_file defined below */
void write_gps_file(const gps_data_t *gps);

/**
 * Handle sensor-only packets from the companion app.
 * When GPS has no fix, the app sends {"sensorOnly":true, "accel":X, "steps":Y}
 * every 3 seconds. We update accel/steps in current_gps and rewrite gps.json
 * so the mobility detection always has fresh sensor data.
 * Returns 1 if handled, 0 if not a sensor-only packet.
 */
int handle_sensor_only(const char *json) {
    if (!json) return 0;
    if (!strstr(json, "sensorOnly")) return 0;
    
    float accel = (float)parse_json_number(json, "accel");
    int steps = (int)parse_json_number(json, "steps");
    double disp_speed = parse_json_number(json, "displacementSpeed");
    char activity[16] = {0};
    int activity_conf = 0;
    parse_json_string(json, "activity", activity, sizeof(activity));
    activity_conf = (int)parse_json_number(json, "activityConfidence");
    char calibration[4] = {0};
    parse_json_string(json, "calibration", calibration, sizeof(calibration));
    
    pthread_mutex_lock(&gps_mutex);
    current_gps.accel = accel;
    current_gps.steps = steps;
    current_gps.displacement_speed = disp_speed;
    if (activity[0]) {
        strncpy(current_gps.activity, activity, sizeof(current_gps.activity) - 1);
        current_gps.activity_confidence = activity_conf;
    }
    if (calibration[0]) {
        strncpy(current_gps.calibration, calibration, sizeof(current_gps.calibration) - 1);
    } else {
        current_gps.calibration[0] = '\0';
    }
    // Rewrite GPS file with updated sensor data (preserves last known position)
    write_gps_file(&current_gps);
    pthread_mutex_unlock(&gps_mutex);
    
    static unsigned int sensor_only_count = 0;
    sensor_only_count++;
    if ((sensor_only_count % 20) == 1) {
        printf("[SENSOR] Sensor-only update #%u: accel=%.2f steps=%d\n",
               sensor_only_count, accel, steps);
    }
    return 1;
}

// Write GPS data to file
void write_gps_file(const gps_data_t *gps) {
    FILE *f;
    time_t now = time(NULL);
    
    // Write JSON format for Pwnagotchi
    f = fopen(GPS_OUTPUT_FILE, "w");
    if (f) {
        fprintf(f, "{\n");
        fprintf(f, "  \"Latitude\": %.8f,\n", gps->latitude);
        fprintf(f, "  \"Longitude\": %.8f,\n", gps->longitude);
        fprintf(f, "  \"Altitude\": %.2f,\n", gps->altitude);
        fprintf(f, "  \"Accuracy\": %.2f,\n", gps->accuracy);
        fprintf(f, "  \"Speed\": %.2f,\n", gps->speed);
        fprintf(f, "  \"Bearing\": %.2f,\n", gps->bearing);
        fprintf(f, "  \"Accel\": %.3f,\n", gps->accel);
        fprintf(f, "  \"Steps\": %d,\n", gps->steps);
        fprintf(f, "  \"Activity\": \"%s\",\n", gps->activity[0] ? gps->activity : "");
        fprintf(f, "  \"ActivityConfidence\": %d,\n", gps->activity_confidence);
        fprintf(f, "  \"DisplacementSpeed\": %.3f,\n", gps->displacement_speed);
        if (gps->calibration[0]) {
            fprintf(f, "  \"Calibration\": \"%s\",\n", gps->calibration);
        }
        fprintf(f, "  \"Updated\": %ld,\n", now);
        fprintf(f, "  \"FixQuality\": 1\n");
        fprintf(f, "}\n");
        fclose(f);
    }
    
    // Also write simple coords file
    f = fopen(GPS_COORDS_FILE, "w");
    if (f) {
        fprintf(f, "%.8f,%.8f\n", gps->latitude, gps->longitude);
        fclose(f);
    }
    
    // Send NMEA GPGGA to pwnaui GPS plugin via UDP
    if (udp_sock >= 0) {
        char nmea[256];
        struct tm *utc = gmtime(&now);
        
        // Convert decimal degrees to NMEA format (DDMM.MMMM)
        double lat_abs = fabs(gps->latitude);
        double lon_abs = fabs(gps->longitude);
        int lat_deg = (int)lat_abs;
        int lon_deg = (int)lon_abs;
        double lat_min = (lat_abs - lat_deg) * 60.0;
        double lon_min = (lon_abs - lon_deg) * 60.0;
        char lat_dir = gps->latitude >= 0 ? 'N' : 'S';
        char lon_dir = gps->longitude >= 0 ? 'E' : 'W';
        
        // Build GPGGA sentence (without checksum yet)
        char body[200];
        snprintf(body, sizeof(body),
            "GPGGA,%02d%02d%02d.00,%02d%07.4f,%c,%03d%07.4f,%c,1,%02d,%.1f,%.1f,M,0.0,M,,",
            utc->tm_hour, utc->tm_min, utc->tm_sec,
            lat_deg, lat_min, lat_dir,
            lon_deg, lon_min, lon_dir,
            (int)(gps->accuracy < 3 ? 10 : (gps->accuracy < 10 ? 8 : 5)),  // satellites estimate
            gps->accuracy < 1 ? 0.9 : (gps->accuracy < 5 ? 1.2 : 2.0),     // HDOP estimate
            gps->altitude);
        
        // Calculate NMEA checksum (XOR of all chars between $ and *)
        unsigned char checksum = 0;
        for (int i = 0; body[i]; i++) {
            checksum ^= body[i];
        }
        
        snprintf(nmea, sizeof(nmea), "$%s*%02X\r\n", body, checksum);
        
        sendto(udp_sock, nmea, strlen(nmea), 0,
               (struct sockaddr *)&pwnaui_addr, sizeof(pwnaui_addr));
        
        /* ---- GPVTG + GPRMC: Speed sentences ----
         * Generate synthetic VTG/RMC when:
         * (a) No raw NMEA from phone (original fallback), OR
         * (b) Raw NMEA is active but Doppler speed was zero and Location
         *     speed is non-zero (enrichment: Samsung Doppler fails at
         *     walking pace but Location.getSpeed() uses sensor fusion).
         * The zero-speed raw VTG/RMC were suppressed in try_forward_raw_nmea()
         * so this synthetic one won't conflict. */
        int raw_nmea_active = (time(NULL) - last_raw_nmea_time) < RAW_NMEA_FRESH_SEC;
        if (!raw_nmea_active || gps->speed > 0.3) {
            /* No raw NMEA from phone — generate synthetic VTG as fallback */
            double speed_kmh = gps->speed * 3.6;  /* m/s -> km/h */
            double speed_knots = gps->speed * 1.94384;  /* m/s -> knots */
            
            char vtg_body[200];
            snprintf(vtg_body, sizeof(vtg_body),
                "GPVTG,%.1f,T,,M,%.1f,N,%.1f,K,A",
                gps->bearing,
                speed_knots,
                speed_kmh);
            
            unsigned char vtg_cksum = 0;
            for (int i = 0; vtg_body[i]; i++) {
                vtg_cksum ^= vtg_body[i];
            }
            
            snprintf(nmea, sizeof(nmea), "$%s*%02X\r\n", vtg_body, vtg_cksum);
            sendto(udp_sock, nmea, strlen(nmea), 0,
                   (struct sockaddr *)&pwnaui_addr, sizeof(pwnaui_addr));

            /* GPRMC fallback */
            double rmc_speed_knots = gps->speed * 1.94384;
            
            char rmc_body[256];
            snprintf(rmc_body, sizeof(rmc_body),
                "GPRMC,%02d%02d%02d.00,A,%02d%07.4f,%c,%03d%07.4f,%c,%.1f,%.1f,%02d%02d%02d,,,A",
                utc->tm_hour, utc->tm_min, utc->tm_sec,
                lat_deg, lat_min, lat_dir,
                lon_deg, lon_min, lon_dir,
                rmc_speed_knots,
                gps->bearing,
                utc->tm_mday, utc->tm_mon + 1, utc->tm_year % 100);
            
            unsigned char rmc_cksum = 0;
            for (int i = 0; rmc_body[i]; i++) {
                rmc_cksum ^= rmc_body[i];
            }
            
            snprintf(nmea, sizeof(nmea), "$%s*%02X\r\n", rmc_body, rmc_cksum);
            sendto(udp_sock, nmea, strlen(nmea), 0,
                   (struct sockaddr *)&pwnaui_addr, sizeof(pwnaui_addr));
        }
    }
}

// Handle client connection
void *handle_client(void *arg) {
    int client_sock = *(int *)arg;
    free(arg);
    
    char buffer[BUFFER_SIZE];
    char client_addr[18];
    struct sockaddr_rc rem_addr = {0};
    socklen_t opt = sizeof(rem_addr);
    
    getpeername(client_sock, (struct sockaddr *)&rem_addr, &opt);
    ba2str(&rem_addr.rc_bdaddr, client_addr);
    // Rate-limit connect logging (max 1 per 5s)
    time_t now_conn = time(NULL);
    if (now_conn - last_connect_log >= 5) {
        printf("Connected: %s\n", client_addr);
        last_connect_log = now_conn;
    }
    
    // Notify pwnaui of BT connection
    bt_connected = 1;
    active_client_fd = client_sock;
    update_bt_status(1);
    
    while (running) {
        memset(buffer, 0, sizeof(buffer));
        ssize_t bytes_read = read(client_sock, buffer, sizeof(buffer) - 1);
        
        if (bytes_read <= 0) {
            if (bytes_read < 0 && errno != ECONNRESET) {
                perror("Read error");
            }
            break;
        }
        
        buffer[bytes_read] = '\0';
        
        // Process each line (may have multiple JSON objects)
        char *line = strtok(buffer, "\n");
        while (line) {
            // Handle keepalive pings from companion app (no ACK — phone doesn't read inputStream)
            if (strstr(line, "\"keepalive\"")) {
                line = strtok(NULL, "\n");
                continue;
            }

            // Handle sensor-only packets (accel+steps without GPS position)
            if (handle_sensor_only(line)) {
                line = strtok(NULL, "\n");
                continue;
            }

            // Check for raw NMEA passthrough first (from OnNmeaMessageListener)
            if (try_forward_raw_nmea(line)) {
                line = strtok(NULL, "\n");
                continue;
            }
            
            gps_data_t gps = {0};
            
            if (parse_gps_json(line, &gps) == 0) {
                /* Track Location API speed for VTG enrichment */
                last_location_speed_mps = (float)gps.speed;
                static double last_logged_lat = 0, last_logged_lon = 0;
                
                pthread_mutex_lock(&gps_mutex);
                memcpy(&current_gps, &gps, sizeof(gps_data_t));
                write_gps_file(&gps);
                pthread_mutex_unlock(&gps_mutex);
                
                // Notify pwnaui of GPS data
                update_gps_status(&gps);
                
                // Log only when position changes (~11m threshold)
                double lat_diff = fabs(gps.latitude - last_logged_lat);
                double lon_diff = fabs(gps.longitude - last_logged_lon);
                if (lat_diff > 0.0001 || lon_diff > 0.0001) {
                    printf("GPS: %.6f, %.6f\n", gps.latitude, gps.longitude);
                    last_logged_lat = gps.latitude;
                    last_logged_lon = gps.longitude;
                }
                
                // No ACK — phone app doesn't read inputStream.
                // Sending ACKs fills RFCOMM buffer → flow control → link reset.
            }
            
            line = strtok(NULL, "\n");
        }
    }
    
    // Rate-limit disconnect logging (max 1 per 5s)
    time_t now_disc = time(NULL);
    if (now_disc - last_disconnect_log >= 5) {
        printf("Disconnected: %s\n", client_addr);
        last_disconnect_log = now_disc;
    }
    
    // Notify pwnaui of BT disconnection
    bt_connected = 0;
    active_client_fd = -1;
    update_bt_status(0);
    update_gps_status(NULL);  // Also mark GPS as disconnected
    
    close(client_sock);
    return NULL;
}

int main(int argc, char **argv) {
    struct sockaddr_rc loc_addr = {0};
    int opt = 1;
    
    printf("===========================================\n");
    printf("  PwnHub Bluetooth GPS Receiver v1.0\n");
    printf("  Listening on RFCOMM channel %d\n", RFCOMM_CHANNEL);
    printf("===========================================\n\n");
    
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);  // Ignore SIGPIPE — write() to closed RFCOMM socket must not kill us
    
    // Create UDP socket for forwarding GPS to pwnaui
    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock >= 0) {
        memset(&pwnaui_addr, 0, sizeof(pwnaui_addr));
        pwnaui_addr.sin_family = AF_INET;
        pwnaui_addr.sin_port = htons(PWNAUI_GPS_PORT);
        // Try bnep0 IP first (Bluetooth PAN), fallback to localhost
        char bnep_ip[16] = "127.0.0.1";
        FILE *fp = popen("ip addr show bnep0 2>/dev/null | grep 'inet ' | awk '{print $2}' | cut -d'/' -f1", "r");
        if (fp) {
            if (fgets(bnep_ip, sizeof(bnep_ip), fp)) {
                bnep_ip[strcspn(bnep_ip, "\n")] = 0;  // Remove newline
            }
            pclose(fp);
        }
        inet_pton(AF_INET, bnep_ip, &pwnaui_addr.sin_addr);
        printf("UDP forwarding to pwnaui on %s:%d\n", bnep_ip, PWNAUI_GPS_PORT);
    }
    
    // Create RFCOMM socket
    server_sock = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (server_sock < 0) {
        perror("Failed to create socket");
        return 1;
    }
    
    // Allow socket reuse
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Bind to local adapter on RFCOMM channel
    loc_addr.rc_family = AF_BLUETOOTH;
    loc_addr.rc_bdaddr = *BDADDR_ANY;
    loc_addr.rc_channel = RFCOMM_CHANNEL;
    
    if (bind(server_sock, (struct sockaddr *)&loc_addr, sizeof(loc_addr)) < 0) {
        perror("Failed to bind socket");
        close(server_sock);
        return 1;
    }
    
    // Listen for connections
    if (listen(server_sock, 1) < 0) {
        perror("Failed to listen");
        close(server_sock);
        return 1;
    }
    
    // Register SDP service
    printf("Registering SDP service...\n");
    sdp_session = register_service(RFCOMM_CHANNEL);
    if (!sdp_session) {
        fprintf(stderr, "Warning: Could not register SDP service\n");
        fprintf(stderr, "Make sure bluetoothd is running with --compat flag\n");
    } else {
        printf("SDP service registered successfully\n");
    }
    
    printf("Waiting for connections from PwnHub app...\n");
    printf("GPS data will be written to: %s\n\n", GPS_OUTPUT_FILE);
    
    while (running) {
        struct sockaddr_rc rem_addr = {0};
        socklen_t rem_len = sizeof(rem_addr);
        
        // Close stale client before accepting new connection
        if (active_client_fd >= 0) {
            close(active_client_fd);
            active_client_fd = -1;
        }
        
        // Accept connection
        int client_sock = accept(server_sock, (struct sockaddr *)&rem_addr, &rem_len);
        
        if (client_sock < 0) {
            if (errno == EINTR || !running) break;
            perror("Accept failed");
            usleep(500000);  // 500ms debounce on accept errors
            continue;
        }
        
        // Spawn thread to handle client
        pthread_t thread;
        int *client_sock_ptr = malloc(sizeof(int));
        *client_sock_ptr = client_sock;
        
        if (pthread_create(&thread, NULL, handle_client, client_sock_ptr) != 0) {
            perror("Failed to create thread");
            close(client_sock);
            free(client_sock_ptr);
        } else {
            pthread_detach(thread);
        }
    }
    
    // Cleanup
    printf("\nCleaning up...\n");
    
    if (udp_sock >= 0) {
        close(udp_sock);
    }
    
    if (sdp_session) {
        sdp_close(sdp_session);
    }
    
    if (server_sock >= 0) {
        close(server_sock);
    }
    
    printf("Goodbye!\n");
    return 0;
}
