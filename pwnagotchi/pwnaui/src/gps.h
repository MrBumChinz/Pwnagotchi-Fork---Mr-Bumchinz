/*
 * PwnaUI - GPS Listener Plugin
 * 
 * Native C implementation of gps_listener.py
 * Receives NMEA sentences from Android phone via Termux over Bluetooth PAN.
 * 
 * Features:
 * - UDP listener on port 5000 (no socat subprocess)
 * - Native PTY creation for virtual serial (no socat for serial)
 * - NMEA parsing to extract lat/lon/alt for display
 * - Feeds raw NMEA to Bettercap for handshake geo-tagging
 * 
 * Protocol:
 * - Android: Termux sends NMEA via UDP to 192.168.44.44:5000
 * - Pi: Receives UDP, parses for display, feeds PTY for Bettercap
 */

#ifndef PWNAUI_GPS_H
#define PWNAUI_GPS_H

#include <stdint.h>
#include <stdbool.h>

/* Configuration */
#define GPS_UDP_PORT            5000
#define GPS_INTERFACE           "bnep0"         /* Bluetooth PAN interface */
#define GPS_PTY_MASTER          "/dev/ttyUSB1"  /* We write to this */
#define GPS_PTY_SLAVE           "/dev/ttyUSB0"  /* Bettercap reads from this */
#define GPS_BAUD_RATE           19200
#define GPS_UPDATE_INTERVAL_MS  1000            /* Display update rate */
#define GPS_NMEA_MAX_LEN        256

/* GPS status codes */
typedef enum {
    GPS_STATUS_DISCONNECTED = 0,    /* '-' No data received */
    GPS_STATUS_CONNECTED,           /* 'C' Receiving data */
    GPS_STATUS_SAVED,               /* 'S' Just saved handshake */
    GPS_STATUS_NO_FIX,              /* 'NF' No GPS fix */
    GPS_STATUS_ERROR                /* 'E' Error state */
} gps_status_t;

/* GPS data structure */
typedef struct {
    /* Coordinates (from NMEA parsing) */
    double latitude;
    double longitude;
    double altitude;
    double speed_knots;
    double speed_kmh;
    double bearing;
    
    /* Quality indicators */
    int satellites;
    int fix_quality;                /* 0=invalid, 1=GPS, 2=DGPS */
    double hdop;                    /* Horizontal dilution of precision */
    
    /* Status */
    gps_status_t status;
    bool has_fix;
    uint64_t last_update_ms;
    uint64_t last_nmea_ms;
    
    /* Display string */
    char display[24];               /* "C" or "-" or lat,lon abbreviated */
    char coords[48];                /* Full coordinates for logging */
    
    /* Internal state */
    int udp_fd;                     /* UDP socket */
    int pty_master_fd;              /* Master PTY fd */
    int pty_slave_fd;               /* Slave PTY fd */
    char pty_master_path[32];       /* Actual master path */
    char pty_slave_path[32];        /* Actual slave path */
    bool initialized;
} gps_data_t;

/*
 * Initialize GPS listener plugin
 * - Gets IP from bnep0 interface
 * - Creates UDP socket on port 5000
 * - Creates PTY pair for Bettercap
 * 
 * Returns 0 on success, -1 on error
 */
int plugin_gps_init(gps_data_t *data);

/*
 * Get the UDP socket fd for select() in main loop
 * Returns -1 if not initialized
 */
int plugin_gps_get_fd(gps_data_t *data);

/*
 * Handle incoming UDP data (call when select() indicates data ready)
 * - Reads NMEA sentence
 * - Parses for coordinates
 * - Writes to PTY for Bettercap
 * 
 * Returns: 1 if data received and parsed, 0 if no data, -1 on error
 */
int plugin_gps_handle_data(gps_data_t *data);

/*
 * Update GPS plugin (call periodically for timeout handling)
 * - Updates status to disconnected if no data for 5 seconds
 * 
 * Returns: 1 if display needs update, 0 otherwise
 */
int plugin_gps_update(gps_data_t *data);

/*
 * Set GPS status (called from handshake handler)
 */
void plugin_gps_set_status(gps_data_t *data, gps_status_t status);

/*
 * Get formatted display string
 */
const char* plugin_gps_get_display(gps_data_t *data);

/*
 * Cleanup GPS plugin
 * - Closes sockets
 * - Removes PTY symlinks
 */
void plugin_gps_cleanup(gps_data_t *data);


/* ============================================================================
 * NMEA Parsing Helpers
 * ============================================================================ */

/*
 * Parse NMEA GPGGA sentence
 * Format: $GPGGA,time,lat,N/S,lon,E/W,quality,sats,hdop,alt,M,geoid,M,dgps_age,dgps_id*checksum
 * 
 * Returns: 1 if valid and parsed, 0 otherwise
 */
int nmea_parse_gpgga(const char *sentence, gps_data_t *data);

/*
 * Parse NMEA GPVTG sentence (velocity)
 * Format: $GPVTG,bearing,T,bearing_mag,M,speed_knots,N,speed_kmh,K*checksum
 * 
 * Returns: 1 if valid and parsed, 0 otherwise
 */
int nmea_parse_gpvtg(const char *sentence, gps_data_t *data);

/*
 * Parse NMEA GPRMC sentence (recommended minimum)
 * Format: $GPRMC,time,status,lat,N/S,lon,E/W,speed,bearing,date,mag_var,E/W*checksum
 * 
 * Returns: 1 if valid and parsed, 0 otherwise
 */
int nmea_parse_gprmc(const char *sentence, gps_data_t *data);

/*
 * Validate NMEA checksum
 * Returns: 1 if valid, 0 if invalid
 */
int nmea_validate_checksum(const char *sentence);

#endif /* PWNAUI_GPS_H */
