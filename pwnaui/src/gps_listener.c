/*
 * gps_listener.c - Native GPS Listener for PwnaUI
 * 
 * Phase 3: Replaces Python gps_listener.py with native C implementation
 * 
 * Features:
 * - Native UDP listener (no socat subprocess)
 * - Native PTY creation (no socat for virtual serial)
 * - Direct integration with PwnaUI display
 * - Feeds NMEA to Bettercap via virtual serial
 * 
 * Build: gcc -o gps_listener gps_listener.c -lpthread
 * Usage: gps_listener [-p port] [-i interface] [-d display_socket]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pty.h>
#include <termios.h>
#include <getopt.h>
#include <time.h>

/* Configuration */
#define DEFAULT_UDP_PORT        5000
#define DEFAULT_INTERFACE       "bnep0"
#define DEFAULT_WRITE_SERIAL    "/dev/ttyUSB1"
#define DEFAULT_READ_SERIAL     "/dev/ttyUSB0"
#define DEFAULT_BAUD_RATE       19200
#define PWNAUI_SOCKET           "/var/run/pwnaui.sock"
#define BUFFER_SIZE             1024
#define NMEA_MAX_LEN            256

/* GPS state */
typedef struct {
    double latitude;
    double longitude;
    double altitude;
    double speed_knots;
    double bearing;
    int fix_quality;
    int satellites;
    char timestamp[16];
    int valid;
    time_t last_update;
} gps_state_t;

/* Global state */
static volatile sig_atomic_t running = 1;
static gps_state_t gps = {0};
static pthread_mutex_t gps_mutex = PTHREAD_MUTEX_INITIALIZER;
static int pty_master = -1;
static int pty_slave = -1;
static int udp_socket = -1;
static int pwnaui_socket = -1;
static char *listen_interface = NULL;

/* Signal handler */
void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

/* Get IP address of interface */
int get_interface_ip(const char *interface, char *ip_out, size_t ip_len) {
    char cmd[256];
    FILE *fp;
    
    snprintf(cmd, sizeof(cmd), 
             "ip addr show %s 2>/dev/null | grep 'inet ' | awk '{print $2}' | cut -d'/' -f1",
             interface);
    
    fp = popen(cmd, "r");
    if (!fp) {
        return -1;
    }
    
    if (fgets(ip_out, ip_len, fp) == NULL) {
        pclose(fp);
        return -1;
    }
    
    pclose(fp);
    
    /* Remove newline */
    size_t len = strlen(ip_out);
    if (len > 0 && ip_out[len-1] == '\n') {
        ip_out[len-1] = '\0';
    }
    
    return strlen(ip_out) > 0 ? 0 : -1;
}

/* Create virtual serial ports using PTY */
int create_virtual_serial(const char *write_path, const char *read_path) {
    char slave_name[256];
    struct termios tty;
    
    /* Remove old symlinks if they exist */
    unlink(write_path);
    unlink(read_path);
    
    /* Create PTY pair */
    if (openpty(&pty_master, &pty_slave, slave_name, NULL, NULL) < 0) {
        perror("openpty failed");
        return -1;
    }
    
    /* Configure as serial port */
    if (tcgetattr(pty_slave, &tty) == 0) {
        cfsetispeed(&tty, B19200);
        cfsetospeed(&tty, B19200);
        tty.c_cflag |= (CLOCAL | CREAD);
        tty.c_cflag &= ~PARENB;
        tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~CSIZE;
        tty.c_cflag |= CS8;
        tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
        tty.c_oflag &= ~OPOST;
        tcsetattr(pty_slave, TCSANOW, &tty);
    }
    
    /* Create symlinks */
    if (symlink(slave_name, write_path) < 0) {
        fprintf(stderr, "Failed to create symlink %s -> %s: %s\n",
                write_path, slave_name, strerror(errno));
        return -1;
    }
    
    if (symlink(slave_name, read_path) < 0) {
        fprintf(stderr, "Failed to create symlink %s -> %s: %s\n",
                read_path, slave_name, strerror(errno));
        unlink(write_path);
        return -1;
    }
    
    /* Make accessible */
    chmod(slave_name, 0777);
    
    printf("[GPS] Virtual serial ports created:\n");
    printf("      Write: %s -> %s\n", write_path, slave_name);
    printf("      Read:  %s -> %s\n", read_path, slave_name);
    
    return 0;
}

/* Parse NMEA GPGGA sentence */
int parse_gpgga(const char *sentence, gps_state_t *state) {
    char type[8];
    char time_str[16] = "";
    char lat_str[16] = "";
    char lat_dir = 'N';
    char lon_str[16] = "";
    char lon_dir = 'E';
    int fix = 0;
    int sats = 0;
    float hdop = 0;
    float alt = 0;
    char alt_unit = 'M';
    
    int n = sscanf(sentence, "$%6[^,],%15[^,],%15[^,],%c,%15[^,],%c,%d,%d,%f,%f,%c",
                   type, time_str, lat_str, &lat_dir, lon_str, &lon_dir,
                   &fix, &sats, &hdop, &alt, &alt_unit);
    
    if (n < 6 || strcmp(type, "GPGGA") != 0) {
        return -1;
    }
    
    /* Parse latitude (DDMM.MMMM) */
    if (strlen(lat_str) >= 4) {
        double deg = 0, min = 0;
        sscanf(lat_str, "%2lf%lf", &deg, &min);
        state->latitude = deg + min / 60.0;
        if (lat_dir == 'S') state->latitude = -state->latitude;
    }
    
    /* Parse longitude (DDDMM.MMMM) */
    if (strlen(lon_str) >= 5) {
        double deg = 0, min = 0;
        sscanf(lon_str, "%3lf%lf", &deg, &min);
        state->longitude = deg + min / 60.0;
        if (lon_dir == 'W') state->longitude = -state->longitude;
    }
    
    state->fix_quality = fix;
    state->satellites = sats;
    state->altitude = alt;
    strncpy(state->timestamp, time_str, sizeof(state->timestamp) - 1);
    state->valid = (fix > 0);
    state->last_update = time(NULL);
    
    return 0;
}

/* Parse NMEA GPVTG sentence (velocity) */
int parse_gpvtg(const char *sentence, gps_state_t *state) {
    char type[8];
    float track = 0, speed_knots = 0, speed_kmh = 0;
    
    int n = sscanf(sentence, "$%6[^,],%f,T,,M,%f,N,%f,K",
                   type, &track, &speed_knots, &speed_kmh);
    
    if (n < 2 || strcmp(type, "GPVTG") != 0) {
        return -1;
    }
    
    state->bearing = track;
    state->speed_knots = speed_knots;
    
    return 0;
}

/* Connect to PwnaUI socket */
int connect_pwnaui(const char *socket_path) {
    struct sockaddr_un addr;
    int fd;
    
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    
    /* Set non-blocking */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    
    return fd;
}

/* Send GPS update to PwnaUI */
void send_gps_to_pwnaui(const gps_state_t *state) {
    char cmd[256];
    
    if (pwnaui_socket < 0) {
        pwnaui_socket = connect_pwnaui(PWNAUI_SOCKET);
        if (pwnaui_socket < 0) {
            return;  /* PwnaUI not running, that's okay */
        }
        printf("[GPS] Connected to PwnaUI\n");
    }
    
    if (state->valid) {
        snprintf(cmd, sizeof(cmd), "SET_GPS %.6f %.6f %.1f %d\n",
                 state->latitude, state->longitude, 
                 state->altitude, state->satellites);
    } else {
        snprintf(cmd, sizeof(cmd), "SET_GPS_STATUS no_fix\n");
    }
    
    if (write(pwnaui_socket, cmd, strlen(cmd)) < 0) {
        if (errno == EPIPE || errno == ECONNRESET) {
            close(pwnaui_socket);
            pwnaui_socket = -1;
        }
    }
}

/* UDP listener thread */
void *udp_listener_thread(void *arg) {
    int port = *(int*)arg;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    char buffer[BUFFER_SIZE];
    char listen_ip[64] = "0.0.0.0";
    
    /* Get interface IP if specified */
    if (listen_interface) {
        if (get_interface_ip(listen_interface, listen_ip, sizeof(listen_ip)) < 0) {
            fprintf(stderr, "[GPS] Warning: Could not get IP for %s, binding to all interfaces\n",
                    listen_interface);
            strcpy(listen_ip, "0.0.0.0");
        } else {
            printf("[GPS] Binding to interface %s (%s)\n", listen_interface, listen_ip);
        }
    }
    
    /* Create UDP socket */
    udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket < 0) {
        perror("[GPS] UDP socket creation failed");
        return NULL;
    }
    
    /* Allow address reuse */
    int opt = 1;
    setsockopt(udp_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    /* Bind to port */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(listen_ip);
    server_addr.sin_port = htons(port);
    
    if (bind(udp_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("[GPS] UDP bind failed");
        close(udp_socket);
        udp_socket = -1;
        return NULL;
    }
    
    printf("[GPS] Listening on UDP %s:%d\n", listen_ip, port);
    
    /* Set receive timeout */
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(udp_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    while (running) {
        ssize_t n = recvfrom(udp_socket, buffer, sizeof(buffer) - 1, 0,
                             (struct sockaddr*)&client_addr, &client_len);
        
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;  /* Timeout, check if still running */
            }
            perror("[GPS] recvfrom error");
            continue;
        }
        
        buffer[n] = '\0';
        
        /* Remove trailing newline */
        if (n > 0 && buffer[n-1] == '\n') {
            buffer[n-1] = '\0';
        }
        
        /* Write to virtual serial port for Bettercap */
        if (pty_master >= 0) {
            dprintf(pty_master, "%s\r\n", buffer);
        }
        
        /* Parse NMEA and update state */
        pthread_mutex_lock(&gps_mutex);
        
        if (strncmp(buffer, "$GPGGA", 6) == 0) {
            parse_gpgga(buffer, &gps);
        } else if (strncmp(buffer, "$GPVTG", 6) == 0) {
            parse_gpvtg(buffer, &gps);
        }
        
        /* Send to PwnaUI */
        send_gps_to_pwnaui(&gps);
        
        pthread_mutex_unlock(&gps_mutex);
    }
    
    close(udp_socket);
    udp_socket = -1;
    
    return NULL;
}

/* Cleanup */
void cleanup(void) {
    printf("\n[GPS] Shutting down...\n");
    
    if (pty_master >= 0) {
        close(pty_master);
    }
    if (pty_slave >= 0) {
        close(pty_slave);
    }
    if (udp_socket >= 0) {
        close(udp_socket);
    }
    if (pwnaui_socket >= 0) {
        close(pwnaui_socket);
    }
    
    /* Remove symlinks */
    unlink(DEFAULT_WRITE_SERIAL);
    unlink(DEFAULT_READ_SERIAL);
    
    printf("[GPS] Cleanup complete\n");
}

/* Print usage */
void usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -p, --port PORT        UDP port to listen on (default: %d)\n", DEFAULT_UDP_PORT);
    printf("  -i, --interface IF     Network interface to bind to (default: %s)\n", DEFAULT_INTERFACE);
    printf("  -w, --write PATH       Write serial path (default: %s)\n", DEFAULT_WRITE_SERIAL);
    printf("  -r, --read PATH        Read serial path (default: %s)\n", DEFAULT_READ_SERIAL);
    printf("  -d, --daemon           Run as daemon\n");
    printf("  -h, --help             Show this help\n");
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_UDP_PORT;
    const char *write_serial = DEFAULT_WRITE_SERIAL;
    const char *read_serial = DEFAULT_READ_SERIAL;
    int daemon_mode = 0;
    
    static struct option long_options[] = {
        {"port",      required_argument, 0, 'p'},
        {"interface", required_argument, 0, 'i'},
        {"write",     required_argument, 0, 'w'},
        {"read",      required_argument, 0, 'r'},
        {"daemon",    no_argument,       0, 'd'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "p:i:w:r:dh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'p':
                port = atoi(optarg);
                break;
            case 'i':
                listen_interface = optarg;
                break;
            case 'w':
                write_serial = optarg;
                break;
            case 'r':
                read_serial = optarg;
                break;
            case 'd':
                daemon_mode = 1;
                break;
            case 'h':
            default:
                usage(argv[0]);
                return (opt == 'h') ? 0 : 1;
        }
    }
    
    /* Default interface if not specified */
    if (!listen_interface) {
        listen_interface = DEFAULT_INTERFACE;
    }
    
    /* Daemonize if requested */
    if (daemon_mode) {
        if (daemon(0, 0) < 0) {
            perror("daemon failed");
            return 1;
        }
    }
    
    printf("╔════════════════════════════════════════════════╗\n");
    printf("║       PwnaUI GPS Listener (C Native)          ║\n");
    printf("║       Phase 3: No Python, No socat            ║\n");
    printf("╚════════════════════════════════════════════════╝\n");
    
    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    
    /* Register cleanup */
    atexit(cleanup);
    
    /* Create virtual serial ports */
    if (create_virtual_serial(write_serial, read_serial) < 0) {
        fprintf(stderr, "[GPS] Failed to create virtual serial ports\n");
        return 1;
    }
    
    /* Start UDP listener thread */
    pthread_t udp_thread;
    if (pthread_create(&udp_thread, NULL, udp_listener_thread, &port) != 0) {
        perror("[GPS] Failed to create UDP thread");
        return 1;
    }
    
    printf("[GPS] Ready. Waiting for NMEA data from phone...\n");
    printf("[GPS] Press Ctrl+C to exit\n\n");
    
    /* Main loop - just wait for signal */
    while (running) {
        sleep(1);
        
        /* Print status periodically */
        pthread_mutex_lock(&gps_mutex);
        if (gps.valid) {
            time_t age = time(NULL) - gps.last_update;
            printf("\r[GPS] Fix: %.6f, %.6f | Alt: %.1fm | Sats: %d | Age: %lds  ",
                   gps.latitude, gps.longitude, gps.altitude, 
                   gps.satellites, age);
            fflush(stdout);
        }
        pthread_mutex_unlock(&gps_mutex);
    }
    
    /* Wait for thread */
    pthread_join(udp_thread, NULL);
    
    return 0;
}
