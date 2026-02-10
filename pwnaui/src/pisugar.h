/**
 * pisugar.h - PiSugar3 Battery/Button Integration for Pwnagotchi
 *
 * Custom button on PiSugar3: Register 0x08 bit 0
 * Power button state: Register 0x02 bit 0
 *
 * Software tap classification (PiSugar3 has no HW tap detection):
 *   Single tap  = press < 400ms, no 2nd press within 400ms
 *   Double tap  = two presses within 400ms
 *   Long press  = press held > 1000ms
 */

#ifndef PISUGAR_H
#define PISUGAR_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* ============================================================================
 * PiSugar3 I2C Configuration
 * ========================================================================== */

#define PISUGAR_I2C_BUS     1       /* /dev/i2c-1 on Pi */
#define PISUGAR_ADDR        0x57    /* PiSugar3 default address */
#define PISUGAR_ADDR_ALT    0x75    /* PiSugar 2 alternate */

/* PiSugar3 Register Map (from I2C Datasheet) */
#define PS3_REG_POWER_STATUS   0x02    /* bit7=ext_power, bit6=charging_sw,
                                          bit5=output_sw_delay, bit4=auto_resume,
                                          bit3=anti_mistouch, bit2=output_sw,
                                          bit0=power_btn_state */
#define PS3_REG_SHUTDOWN       0x03    /* bit6=auto_hibernate, bit4=soft_shutdown_set,
                                          bit3=soft_shutdown_sign */
#define PS3_REG_TEMP           0x04    /* Chip temp: value + (-40) = Celsius */
#define PS3_REG_WATCHDOG       0x06    /* bit7=sw_watchdog, bit5=feed_sw_wd,
                                          bit4=boot_wd, bit3=feed_boot_wd */
#define PS3_REG_WD_TIMEOUT     0x07    /* Watchdog timeout: val * 2s */
#define PS3_REG_CUSTOM_BTN     0x08    /* bit0 = custom button state (RW) */
#define PS3_REG_DELAY_OFF      0x09    /* Delayed shutdown seconds */
#define PS3_REG_CHARGE_PROT    0x20    /* bit7=charging_protection, bit3=scl_wake */
#define PS3_REG_VOLT_HIGH      0x22    /* Battery voltage high byte */
#define PS3_REG_VOLT_LOW       0x23    /* Battery voltage low byte */
#define PS3_REG_BATTERY        0x2A    /* Battery level 0-100% */
#define PS3_REG_LED            0xE0    /* bit0-3 = LED control */

/* Legacy aliases for backward compat */
#define PISUGAR_REG_BATTERY    PS3_REG_BATTERY
#define PISUGAR_REG_VOLTAGE    PS3_REG_VOLT_HIGH
#define PISUGAR_REG_CHARGING   PS3_REG_POWER_STATUS

/* ============================================================================
 * Tap Classification - Software-based (PiSugar3 has no HW tap detection)
 * ========================================================================== */

/* Timing thresholds (milliseconds) */
#define TAP_SHORT_MAX_MS     400    /* Max press duration for a "tap" */
#define TAP_DOUBLE_GAP_MS    400    /* Max gap between taps for double-tap */
#define TAP_LONG_THRESH_MS  2500    /* Min hold for long press (>2s avoids 1s-poll misdetect) */
#define TAP_DEBOUNCE_MS       50    /* Debounce time */

/* Tap result types */
typedef enum {
    TAP_NONE     = 0,
    TAP_SINGLE   = 1,   /* Single short press -> toggle auto/manual */
    TAP_DOUBLE   = 2,   /* Two quick presses  -> force channel hop */
    TAP_LONG     = 3    /* Hold > 1 second    -> safe shutdown */
} pisugar_tap_t;

/* Internal state machine for tap detection */
typedef enum {
    BTN_IDLE = 0,         /* Waiting for press */
    BTN_PRESSED,          /* Button currently down, timing duration */
    BTN_RELEASED_ONCE,    /* Released once, waiting for possible 2nd press */
    BTN_LONG_FIRED        /* Long press already reported, wait for release */
} btn_state_t;

/* Operating mode */
typedef enum {
    MODE_AUTO   = 0,
    MODE_MANUAL = 1
} pwnagotchi_mode_t;

/* ============================================================================
 * PiSugar Context
 * ========================================================================== */

typedef struct {
    int i2c_fd;                     /* I2C file descriptor */
    uint8_t i2c_addr;               /* Active I2C address */
    bool connected;                 /* PiSugar detected */

    /* Battery state */
    int battery_level;              /* 0-100% */
    int voltage_mv;                 /* Millivolts */
    bool charging;                  /* Currently charging */

    /* Mode state */
    pwnagotchi_mode_t current_mode;

    /* Software tap detection state machine */
    btn_state_t btn_state;
    uint64_t btn_press_time;        /* When button was pressed (ms) */
    uint64_t btn_release_time;      /* When button was released (ms) */
    int btn_last_raw;               /* Last raw button reading (debounce) */
    uint64_t btn_last_change;       /* Last state change time (debounce) */

    /* Callback for mode change */
    void (*on_mode_change)(pwnagotchi_mode_t new_mode, void *user_data);
    void *callback_user_data;
} pisugar_ctx_t;

/* ============================================================================
 * Public API
 * ========================================================================== */

/* Initialize PiSugar I2C connection */
pisugar_ctx_t *pisugar_init(void);

/* Cleanup and close */
void pisugar_destroy(pisugar_ctx_t *ctx);

/* Poll for tap events on CUSTOM BUTTON (call every ~50ms from main loop)
 * Returns tap type detected (or TAP_NONE)
 * Uses software state machine for single/double/long classification */
pisugar_tap_t pisugar_poll_tap(pisugar_ctx_t *ctx);

/* Read battery status */
int pisugar_read_battery(pisugar_ctx_t *ctx);
int pisugar_read_voltage(pisugar_ctx_t *ctx);
bool pisugar_is_charging(pisugar_ctx_t *ctx);

/* Mode management - NO service restart, in-process toggle */
pwnagotchi_mode_t pisugar_get_mode(pisugar_ctx_t *ctx);
int pisugar_toggle_mode(pisugar_ctx_t *ctx);
int pisugar_set_mode(pisugar_ctx_t *ctx, pwnagotchi_mode_t mode);

/* Set callback for mode changes */
void pisugar_set_callback(pisugar_ctx_t *ctx,
    void (*on_mode_change)(pwnagotchi_mode_t new_mode, void *user_data),
    void *user_data);

/* Utility */
uint64_t pisugar_millis(void);

#endif /* PISUGAR_H */
