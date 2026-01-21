/*
 * PwnaUI - GPS Listener Plugin Implementation
 * 
 * Native C implementation of gps_listener.py
 * Receives NMEA sentences from Android phone via Termux over Bluetooth PAN.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <math.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <pty.h>
#include <termios.h>

#include "gps.h"

/* Timeout for marking GPS as disconnected (ms) */
#define GPS_TIMEOUT_MS          5000

/* Forward declarations */
static int get_interface_ip(const char *interface, char *ip_out, size_t ip_size);
static int create_pty_pair(gps_data_t *data);
static void remove_pty_links(gps_data_t *data);
static uint64_t get_time_ms(void);

/*
 * Get current time in milliseconds
 */
static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * Get IP address for network interface
 */
static int get_interface_ip(const char *interface, char *ip_out, size_t ip_size) {
    int fd;
    struct ifreq ifr;
    
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, interface, IFNAMSIZ - 1);
    
    if (ioctl(fd, SIOCGIFADDR, &ifr) < 0) {
        close(fd);
        return -1;
    }
    
    close(fd);
    
    struct sockaddr_in *addr = (struct sockaddr_in *)&ifr.ifr_addr;
    if (inet_ntop(AF_INET, &addr->sin_addr, ip_out, ip_size) == NULL) {
        return -1;
    }
    
    return 0;
}

/*
 * Create PTY pair for Bettercap
 * Creates symlinks at GPS_PTY_MASTER and GPS_PTY_SLAVE
 */
static int create_pty_pair(gps_data_t *data) {
    int master_fd, slave_fd;
    char slave_name[256];
    
    /* Open PTY master/slave pair */
    if (openpty(&master_fd, &slave_fd, slave_name, NULL, NULL) < 0) {
        fprintf(stderr, "GPS: Failed to create PTY pair: %s\n", strerror(errno));
        return -1;
    }
    
    /* Set PTY master to non-blocking so writes don't hang if nothing reads */
    int flags = fcntl(master_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);
    }
    
    /* Configure serial settings to match GPS */
    struct termios tty;
    if (tcgetattr(slave_fd, &tty) == 0) {
        cfsetospeed(&tty, B19200);
        cfsetispeed(&tty, B19200);
        tty.c_cflag = CS8 | CREAD | CLOCAL;
        tty.c_iflag = IGNPAR;
        tty.c_oflag = 0;
        tty.c_lflag = 0;
        tcsetattr(slave_fd, TCSANOW, &tty);
    }
    
    /* Store the actual PTY paths */
    snprintf(data->pty_master_path, sizeof(data->pty_master_path), "/dev/pts/%d",
             ptsname_r(master_fd, data->pty_master_path, sizeof(data->pty_master_path)) == 0 ? 
             atoi(data->pty_master_path + 9) : -1);
    strncpy(data->pty_slave_path, slave_name, sizeof(data->pty_slave_path) - 1);
    
    /* Remove old symlinks if they exist */
    remove_pty_links(data);
    
    /* Create symlinks for well-known paths */
    /* Note: We write to master (GPS_PTY_MASTER) and Bettercap reads from slave (GPS_PTY_SLAVE) */
    if (symlink(slave_name, GPS_PTY_SLAVE) < 0 && errno != EEXIST) {
        fprintf(stderr, "GPS: Failed to create symlink %s -> %s: %s\n", 
                GPS_PTY_SLAVE, slave_name, strerror(errno));
        /* Continue anyway, Bettercap might use actual pts path */
    }
    
    /* Make the slave PTY accessible */
    chmod(slave_name, 0666);
    chmod(GPS_PTY_SLAVE, 0666);
    
    data->pty_master_fd = master_fd;
    data->pty_slave_fd = slave_fd;
    
    printf("GPS: Created PTY pair - master: fd=%d, slave: %s -> %s\n",
           master_fd, GPS_PTY_SLAVE, slave_name);
    
    return 0;
}

/*
 * Remove PTY symlinks
 */
static void remove_pty_links(gps_data_t *data) {
    unlink(GPS_PTY_MASTER);
    unlink(GPS_PTY_SLAVE);
}

/*
 * Initialize GPS listener plugin
 */
int plugin_gps_init(gps_data_t *data) {
    char bind_ip[INET_ADDRSTRLEN] = "0.0.0.0";
    struct sockaddr_in addr;
    int opt = 1;
    
    if (!data) return -1;
    
    /* Clear state */
    memset(data, 0, sizeof(gps_data_t));
    data->udp_fd = -1;
    data->pty_master_fd = -1;
    data->pty_slave_fd = -1;
    data->status = GPS_STATUS_DISCONNECTED;
    strcpy(data->display, "GPS-");
    
    /* Try to get IP from bnep0 (Bluetooth PAN) */
    if (get_interface_ip(GPS_INTERFACE, bind_ip, sizeof(bind_ip)) == 0) {
        printf("GPS: Binding to %s (%s)\n", GPS_INTERFACE, bind_ip);
    } else {
        printf("GPS: %s not available, binding to 0.0.0.0\n", GPS_INTERFACE);
        strcpy(bind_ip, "0.0.0.0");
    }
    
    /* Create UDP socket */
    data->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (data->udp_fd < 0) {
        fprintf(stderr, "GPS: Failed to create UDP socket: %s\n", strerror(errno));
        return -1;
    }
    
    /* Allow address reuse */
    setsockopt(data->udp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    /* Set non-blocking */
    int flags = fcntl(data->udp_fd, F_GETFL, 0);
    fcntl(data->udp_fd, F_SETFL, flags | O_NONBLOCK);
    
    /* Bind to port */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(GPS_UDP_PORT);
    inet_pton(AF_INET, bind_ip, &addr.sin_addr);
    
    if (bind(data->udp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "GPS: Failed to bind to port %d: %s\n", GPS_UDP_PORT, strerror(errno));
        close(data->udp_fd);
        data->udp_fd = -1;
        return -1;
    }
    
    /* Create PTY pair for Bettercap */
    if (create_pty_pair(data) < 0) {
        fprintf(stderr, "GPS: Failed to create PTY pair, continuing without Bettercap feed\n");
        /* Continue anyway - display will still work */
    }
    
    data->initialized = true;
    data->last_update_ms = get_time_ms();
    
    printf("GPS: Initialized - listening on UDP port %d\n", GPS_UDP_PORT);
    printf("GPS: Bettercap should use: %s at %d baud\n", GPS_PTY_SLAVE, GPS_BAUD_RATE);
    
    return 0;
}

/*
 * Get UDP socket fd for select()
 */
int plugin_gps_get_fd(gps_data_t *data) {
    if (!data || !data->initialized) return -1;
    return data->udp_fd;
}

/*
 * Validate NMEA checksum
 * Sentence format: $GPGGA,...*XX where XX is hex checksum
 */
int nmea_validate_checksum(const char *sentence) {
    if (!sentence || sentence[0] != '$') return 0;
    
    const char *checksum_ptr = strchr(sentence, '*');
    if (!checksum_ptr || strlen(checksum_ptr) < 3) return 0;
    
    /* Calculate checksum (XOR of all chars between $ and *) */
    unsigned char calc_checksum = 0;
    const char *p = sentence + 1;  /* Skip $ */
    while (*p && *p != '*') {
        calc_checksum ^= (unsigned char)*p;
        p++;
    }
    
    /* Parse expected checksum */
    unsigned int expected_checksum;
    if (sscanf(checksum_ptr + 1, "%02X", &expected_checksum) != 1) {
        return 0;
    }
    
    return (calc_checksum == (unsigned char)expected_checksum);
}

/*
 * Parse NMEA latitude/longitude
 * Format: DDMM.MMMM for lat, DDDMM.MMMM for lon
 */
static double nmea_parse_coord(const char *coord, const char *dir) {
    if (!coord || !dir || strlen(coord) < 4) return 0.0;
    
    double raw_value = atof(coord);
    int degrees = (int)(raw_value / 100);
    double minutes = raw_value - (degrees * 100);
    double result = degrees + (minutes / 60.0);
    
    if (dir[0] == 'S' || dir[0] == 'W') {
        result = -result;
    }
    
    return result;
}

/*
 * Parse NMEA GPGGA sentence
 * $GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,47.0,M,,*47
 */
int nmea_parse_gpgga(const char *sentence, gps_data_t *data) {
    if (!sentence || !data) return 0;
    if (strncmp(sentence, "$GPGGA", 6) != 0) return 0;
    
    /* Validate checksum */
    if (!nmea_validate_checksum(sentence)) {
        return 0;
    }
    
    /* Parse fields */
    char time_str[16] = {0};
    char lat_str[16] = {0}, lat_dir[4] = {0};
    char lon_str[16] = {0}, lon_dir[4] = {0};
    int fix_quality = 0;
    int satellites = 0;
    double hdop = 0.0;
    double altitude = 0.0;
    char alt_unit[4] = {0};
    
    /* $GPGGA,time,lat,N,lon,E,quality,sats,hdop,alt,M,geoid,M,dgps_age,dgps_id*checksum */
    int parsed = sscanf(sentence, "$GPGGA,%15[^,],%15[^,],%3[^,],%15[^,],%3[^,],%d,%d,%lf,%lf,%3[^,]",
                        time_str, lat_str, lat_dir, lon_str, lon_dir,
                        &fix_quality, &satellites, &hdop, &altitude, alt_unit);
    
    if (parsed < 5) return 0;
    
    /* Check for valid fix */
    if (fix_quality == 0 || strlen(lat_str) == 0 || strlen(lon_str) == 0) {
        data->has_fix = false;
        return 0;
    }
    
    /* Parse coordinates */
    data->latitude = nmea_parse_coord(lat_str, lat_dir);
    data->longitude = nmea_parse_coord(lon_str, lon_dir);
    data->altitude = altitude;
    data->fix_quality = fix_quality;
    data->satellites = satellites;
    data->hdop = hdop;
    data->has_fix = true;
    
    return 1;
}

/*
 * Parse NMEA GPVTG sentence (velocity/track)
 * $GPVTG,054.7,T,034.4,M,005.5,N,010.2,K*48
 */
int nmea_parse_gpvtg(const char *sentence, gps_data_t *data) {
    if (!sentence || !data) return 0;
    if (strncmp(sentence, "$GPVTG", 6) != 0) return 0;
    
    if (!nmea_validate_checksum(sentence)) {
        return 0;
    }
    
    double bearing = 0.0;
    double speed_knots = 0.0;
    double speed_kmh = 0.0;
    char t_indicator[4], m_indicator[4], n_indicator[4], k_indicator[4];
    double bearing_mag = 0.0;
    
    int parsed = sscanf(sentence, "$GPVTG,%lf,%3[^,],%lf,%3[^,],%lf,%3[^,],%lf,%3[^,]",
                        &bearing, t_indicator, &bearing_mag, m_indicator,
                        &speed_knots, n_indicator, &speed_kmh, k_indicator);
    
    if (parsed >= 7) {
        data->bearing = bearing;
        data->speed_knots = speed_knots;
        data->speed_kmh = speed_kmh;
        return 1;
    }
    
    return 0;
}

/*
 * Parse NMEA GPRMC sentence (recommended minimum)
 * $GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A
 */
int nmea_parse_gprmc(const char *sentence, gps_data_t *data) {
    if (!sentence || !data) return 0;
    if (strncmp(sentence, "$GPRMC", 6) != 0) return 0;
    
    if (!nmea_validate_checksum(sentence)) {
        return 0;
    }
    
    char time_str[16] = {0};
    char status = 'V';  /* V = void, A = active */
    char lat_str[16] = {0}, lat_dir[4] = {0};
    char lon_str[16] = {0}, lon_dir[4] = {0};
    double speed_knots = 0.0;
    double bearing = 0.0;
    
    int parsed = sscanf(sentence, "$GPRMC,%15[^,],%c,%15[^,],%3[^,],%15[^,],%3[^,],%lf,%lf",
                        time_str, &status, lat_str, lat_dir, lon_str, lon_dir,
                        &speed_knots, &bearing);
    
    if (parsed >= 6 && status == 'A') {
        data->latitude = nmea_parse_coord(lat_str, lat_dir);
        data->longitude = nmea_parse_coord(lon_str, lon_dir);
        data->speed_knots = speed_knots;
        data->speed_kmh = speed_knots * 1.852;
        data->bearing = bearing;
        data->has_fix = true;
        return 1;
    }
    
    return 0;
}

/*
 * Handle incoming UDP data
 */
int plugin_gps_handle_data(gps_data_t *data) {
    if (!data || !data->initialized || data->udp_fd < 0) return -1;
    
    char buffer[GPS_NMEA_MAX_LEN];
    struct sockaddr_in sender;
    socklen_t sender_len = sizeof(sender);
    
    /* Receive UDP packet */
    ssize_t len = recvfrom(data->udp_fd, buffer, sizeof(buffer) - 1, 0,
                          (struct sockaddr *)&sender, &sender_len);
    
    if (len < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  /* No data available */
        }
        return -1;
    }
    
    if (len == 0) return 0;
    
    buffer[len] = '\0';
    
    /* Remove trailing whitespace */
    while (len > 0 && (buffer[len-1] == '\n' || buffer[len-1] == '\r')) {
        buffer[--len] = '\0';
    }
    
    /* Update timestamp */
    data->last_nmea_ms = get_time_ms();
    data->status = GPS_STATUS_CONNECTED;
    
    /* Write raw NMEA to PTY for Bettercap */
    if (data->pty_master_fd >= 0) {
        /* Re-add newline for serial */
        char nmea_line[GPS_NMEA_MAX_LEN + 2];
        snprintf(nmea_line, sizeof(nmea_line), "%s\r\n", buffer);
        ssize_t written = write(data->pty_master_fd, nmea_line, strlen(nmea_line));
        if (written < 0 && errno != EAGAIN) {
            fprintf(stderr, "GPS: Failed to write to PTY: %s\n", strerror(errno));
        }
    }
    
    /* Parse NMEA for display */
    int parsed = 0;
    if (strncmp(buffer, "$GPGGA", 6) == 0) {
        parsed = nmea_parse_gpgga(buffer, data);
    } else if (strncmp(buffer, "$GPVTG", 6) == 0) {
        parsed = nmea_parse_gpvtg(buffer, data);
    } else if (strncmp(buffer, "$GPRMC", 6) == 0) {
        parsed = nmea_parse_gprmc(buffer, data);
    }
    
    /* Update display string */
    if (data->has_fix) {
        data->status = GPS_STATUS_CONNECTED;
        strcpy(data->display, "GPS+");  /* Connected with fix */
        
        /* Full coordinates for logging */
        snprintf(data->coords, sizeof(data->coords), "%.6f,%.6f,%.1f",
                 data->latitude, data->longitude, data->altitude);
    } else {
        strcpy(data->display, "GPS?");  /* Connected but no fix yet */
    }
    
    return parsed ? 1 : 0;
}

/*
 * Update GPS plugin (timeout handling)
 */
int plugin_gps_update(gps_data_t *data) {
    if (!data || !data->initialized) return 0;
    
    uint64_t now = get_time_ms();
    
    /* Check for timeout */
    if (data->status == GPS_STATUS_CONNECTED) {
        if (now - data->last_nmea_ms > GPS_TIMEOUT_MS) {
            data->status = GPS_STATUS_DISCONNECTED;
            data->has_fix = false;
            strcpy(data->display, "GPS-");
            return 1;  /* Display needs update */
        }
    }
    
    /* Check if we should update display */
    if (now - data->last_update_ms >= GPS_UPDATE_INTERVAL_MS) {
        data->last_update_ms = now;
        return 1;
    }
    
    return 0;
}

/*
 * Set GPS status
 */
void plugin_gps_set_status(gps_data_t *data, gps_status_t status) {
    if (!data) return;
    
    data->status = status;
    
    switch (status) {
        case GPS_STATUS_DISCONNECTED:
            strcpy(data->display, "GPS-");
            break;
        case GPS_STATUS_CONNECTED:
            strcpy(data->display, "GPS+");
            break;
        case GPS_STATUS_SAVED:
            strcpy(data->display, "GPS S");
            break;
        case GPS_STATUS_NO_FIX:
            strcpy(data->display, "GPS?");
            break;
        case GPS_STATUS_ERROR:
            strcpy(data->display, "GPS-");
            break;
    }
}

/*
 * Get formatted display string
 */
const char* plugin_gps_get_display(gps_data_t *data) {
    if (!data) return "-";
    return data->display;
}

/*
 * Cleanup GPS plugin
 */
void plugin_gps_cleanup(gps_data_t *data) {
    if (!data) return;
    
    if (data->udp_fd >= 0) {
        close(data->udp_fd);
        data->udp_fd = -1;
    }
    
    if (data->pty_master_fd >= 0) {
        close(data->pty_master_fd);
        data->pty_master_fd = -1;
    }
    
    if (data->pty_slave_fd >= 0) {
        close(data->pty_slave_fd);
        data->pty_slave_fd = -1;
    }
    
    remove_pty_links(data);
    
    data->initialized = false;
    data->status = GPS_STATUS_DISCONNECTED;
    strcpy(data->display, "GPS-");
    
    printf("GPS: Cleanup complete\n");
}
