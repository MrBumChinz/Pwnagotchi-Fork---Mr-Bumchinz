/*
 * PwnaUI - Native C Plugin System
 * 
 * Replaces CPU-intensive Python plugins with native C implementations.
 * These run in the pwnaui daemon and communicate via the existing IPC.
 * 
 * Implemented plugins:
 * - memtemp: System memory, CPU, temperature monitoring
 * - battery: Multi-device support (PiSugar 2/3, UPS Lite v1.3)
 * - bluetooth: BT-Tether connection status
 * - gps: GPS via phone Bluetooth (gps_listener replacement)
 * 
 * Future plugins:
 * - grid: Peer tracking
 */

#ifndef PWNAUI_PLUGINS_H
#define PWNAUI_PLUGINS_H

#include <stdint.h>
#include <stdbool.h>
#include "gps.h"

/* Plugin update interval in milliseconds */
#define PLUGIN_MEMTEMP_INTERVAL_MS     1000   /* 1 second */
#define PLUGIN_BATTERY_INTERVAL_MS     5000   /* 5 seconds */
#define PLUGIN_BLUETOOTH_INTERVAL_MS   2000   /* 2 seconds */

/* ============================================================================
 * MEMTEMP PLUGIN - System metrics
 * ============================================================================ */

typedef struct {
    int mem_percent;      /* Memory usage 0-100 */
    int cpu_percent;      /* CPU usage 0-100 */
    int temp_celsius;     /* CPU temperature */
    char header[32];      /* "mem cpu tmp" */
    char data[32];        /* " 45%  12%  52C" */
} memtemp_data_t;

/*
 * Initialize memtemp plugin
 * Returns 0 on success, -1 on error
 */
int plugin_memtemp_init(void);

/*
 * Update memtemp readings
 * Call this periodically (every 1 second)
 */
int plugin_memtemp_update(memtemp_data_t *data);

/*
 * Cleanup memtemp plugin
 */
void plugin_memtemp_cleanup(void);


/* ============================================================================
 * BATTERY PLUGIN - Multi-device support (PiSugar, UPS Lite, etc.)
 * 
 * Automatically detects and supports:
 * - PiSugar 2/2Plus (I2C 0x75)
 * - PiSugar 3/3Plus (I2C 0x57) 
 * - UPS Lite v1.3 (I2C 0x62 CW2015 + GPIO4)
 * ============================================================================ */

/* Battery device types */
typedef enum {
    BATTERY_NONE = 0,
    BATTERY_PISUGAR2,
    BATTERY_PISUGAR3,
    BATTERY_UPSLITE
} battery_device_t;

typedef struct {
    int percentage;           /* Battery 0-100 */
    bool charging;            /* Is charging */
    bool available;           /* Battery device detected */
    float voltage;            /* Battery voltage (if available) */
    float current;            /* Charge/discharge current (if available) */
    battery_device_t device_type;  /* Detected device type */
    char display[24];         /* Formatted display string "UPS 85%+" or "92%" */
} battery_data_t;

/*
 * Initialize battery plugin (probes I2C for supported devices)
 * Returns 0 on success, -1 if no battery device found
 */
int plugin_battery_init(void);

/*
 * Update battery readings
 */
int plugin_battery_update(battery_data_t *data);

/*
 * Cleanup battery plugin
 */
void plugin_battery_cleanup(void);


/* ============================================================================
 * BLUETOOTH PLUGIN - Connection status
 * ============================================================================ */

typedef struct {
    bool connected;       /* BT connected */
    bool tethered;        /* Network tethered */
    char status[8];       /* "BT ✓" or "BT ✗" */
    char device_name[64]; /* Connected device */
} bluetooth_data_t;

/*
 * Initialize bluetooth plugin
 */
int plugin_bluetooth_init(void);

/*
 * Update bluetooth status
 */
int plugin_bluetooth_update(bluetooth_data_t *data);

/*
 * Cleanup bluetooth plugin
 */
void plugin_bluetooth_cleanup(void);


/* ============================================================================
 * PLUGIN MANAGER
 * ============================================================================ */

typedef struct {
    bool memtemp_enabled;
    bool battery_enabled;
    bool bluetooth_enabled;
    bool gps_enabled;
    
    memtemp_data_t memtemp;
    battery_data_t battery;
    bluetooth_data_t bluetooth;
    gps_data_t gps;
    
    uint64_t last_memtemp_update;
    uint64_t last_battery_update;
    uint64_t last_bluetooth_update;
} plugin_state_t;

/*
 * Initialize all enabled plugins
 */
int plugins_init(plugin_state_t *state);

/*
 * Update all plugins (call from main loop)
 * Returns bitmask of which plugins updated
 */
int plugins_update(plugin_state_t *state);

/*
 * Cleanup all plugins
 */
void plugins_cleanup(plugin_state_t *state);

#endif /* PWNAUI_PLUGINS_H */
