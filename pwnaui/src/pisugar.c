/**
 * pisugar.c - PiSugar3 Battery/Custom Button Integration
 *
 * Custom button: Register 0x08, bit 0 (per PiSugar3 I2C datasheet)
 * Software tap detection: single tap, double tap, long press
 *
 * Single tap  -> toggle AUTO/MANUAL mode (in-process, no restart)
 * Double tap  -> available for channel hop / next target
 * Long press  -> safe shutdown
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <errno.h>
#include <time.h>

#include "pisugar.h"

/* Mode files (persist across restarts) */
#define AUTO_FILE   "/root/.pwnagotchi-auto"
#define MANUAL_FILE "/root/.pwnagotchi-manual"

/* SMBus ioctl support */
#define I2C_SMBUS_READ  1
#define I2C_SMBUS_WRITE 0
#define I2C_SMBUS_BYTE_DATA 2
#define I2C_SMBUS 0x0720

/* ============================================================================
 * Utility: millisecond clock
 * ========================================================================== */

uint64_t pisugar_millis(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* ============================================================================
 * I2C Helpers - SMBus for PiSugar3 compatibility
 * ========================================================================== */

static int smbus_read_byte(int fd, uint8_t addr, uint8_t reg) {
    if (ioctl(fd, I2C_SLAVE, addr) < 0) return -1;

    struct i2c_smbus_ioctl_data args;
    union i2c_smbus_data data;

    args.read_write = I2C_SMBUS_READ;
    args.command = reg;
    args.size = I2C_SMBUS_BYTE_DATA;
    args.data = &data;

    if (ioctl(fd, I2C_SMBUS, &args) < 0) return -1;
    return data.byte & 0xFF;
}

static int smbus_write_byte(int fd, uint8_t addr, uint8_t reg, uint8_t value) {
    if (ioctl(fd, I2C_SLAVE, addr) < 0) return -1;

    struct i2c_smbus_ioctl_data args;
    union i2c_smbus_data data;

    data.byte = value;
    args.read_write = I2C_SMBUS_WRITE;
    args.command = reg;
    args.size = I2C_SMBUS_BYTE_DATA;
    args.data = &data;

    return ioctl(fd, I2C_SMBUS, &args);
}

/* ============================================================================
 * Mode File Management (persist mode across restarts)
 * ========================================================================== */

static pwnagotchi_mode_t read_mode_from_files(void) {
    /* Always start in MANUAL mode on boot */
    (void)MANUAL_FILE;  /* suppress unused warning */
    return MODE_MANUAL;
}

static int write_mode_files(pwnagotchi_mode_t mode) {
    unlink(AUTO_FILE);
    unlink(MANUAL_FILE);

    const char *file = (mode == MODE_AUTO) ? AUTO_FILE : MANUAL_FILE;
    int fd = open(file, O_CREAT | O_WRONLY, 0644);
    if (fd < 0) {
        fprintf(stderr, "[pisugar] failed to create %s: %s\n", file, strerror(errno));
        return -1;
    }
    close(fd);
    return 0;
}

/* ============================================================================
 * Custom Button Reading - Register 0x08, bit 0
 * ========================================================================== */

/**
 * Read custom button state from PiSugar3
 * Register 0x08, bit 0: 1 = pressed, need to write 0 to clear
 * Returns: 1 = pressed, 0 = not pressed, -1 = error
 */
static int read_custom_button(pisugar_ctx_t *ctx) {
    if (!ctx || !ctx->connected) return -1;

    int val = smbus_read_byte(ctx->i2c_fd, ctx->i2c_addr, PS3_REG_CUSTOM_BTN);
    if (val < 0) return -1;

    int pressed = val & 0x01;

    /* Clear the button state by writing 0 to bit 0
     * The PiSugar3 latches button presses - we need to clear after reading */
    if (pressed) {
        smbus_write_byte(ctx->i2c_fd, ctx->i2c_addr, PS3_REG_CUSTOM_BTN,
                         (uint8_t)(val & ~0x01));
    }

    return pressed;
}

/* ============================================================================
 * Software Tap Detection State Machine
 *
 * PiSugar3 custom button at 0x08 bit 0 is a latching flag:
 *   - Reading 1 means the button was pressed since last clear
 *   - We clear it by writing 0 back
 *
 * We poll every ~50ms and classify:
 *   Single tap:  One press event, no second within TAP_DOUBLE_GAP_MS
 *   Double tap:  Two press events within TAP_DOUBLE_GAP_MS
 *   Long press:  Button held continuously for TAP_LONG_THRESH_MS
 *                (detected via repeated reads of 1 without clearing)
 *
 * Since the register is latching (not real-time), we use a simpler model:
 *   - Each "1" read = one button activation event
 *   - Time between consecutive events determines single vs double
 *   - For long press: we check the power button state at 0x02 bit 0
 *     or use custom button with timing heuristic
 * ========================================================================== */

pisugar_tap_t pisugar_poll_tap(pisugar_ctx_t *ctx) {
    if (!ctx || !ctx->connected) return TAP_NONE;

    uint64_t now = pisugar_millis();

    /* Read custom button (latching register) */
    int val = smbus_read_byte(ctx->i2c_fd, ctx->i2c_addr, PS3_REG_CUSTOM_BTN);
    if (val < 0) return TAP_NONE;

    int btn_event = val & 0x01;

    /* If button event detected, clear it immediately */
    if (btn_event) {
        smbus_write_byte(ctx->i2c_fd, ctx->i2c_addr, PS3_REG_CUSTOM_BTN,
                         (uint8_t)(val & ~0x01));
    }

    pisugar_tap_t result = TAP_NONE;

    switch (ctx->btn_state) {
    case BTN_IDLE:
        if (btn_event) {
            /* Button was pressed - record time, move to pressed state */
            ctx->btn_press_time = now;
            ctx->btn_state = BTN_PRESSED;
        }
        break;

    case BTN_PRESSED:
        if (btn_event) {
            /* Another event while we thought it was held = still pressing
             * Check if long press threshold reached */
            if ((now - ctx->btn_press_time) >= TAP_LONG_THRESH_MS) {
                result = TAP_LONG;
                ctx->btn_state = BTN_LONG_FIRED;
                fprintf(stderr, "[pisugar] LONG PRESS detected (%llu ms)\n",
                        (unsigned long long)(now - ctx->btn_press_time));
            }
            /* Otherwise keep waiting */
        } else {
            /* No event = button was released
             * If short enough, treat as a tap */
            uint64_t hold_duration = now - ctx->btn_press_time;

            if (hold_duration < TAP_SHORT_MAX_MS) {
                /* Short press -> might be single or first of double */
                ctx->btn_release_time = now;
                ctx->btn_state = BTN_RELEASED_ONCE;
            } else if (hold_duration >= TAP_LONG_THRESH_MS) {
                /* Was held long but we missed detecting it as held
                 * (e.g., the latch was only set once) */
                result = TAP_LONG;
                ctx->btn_state = BTN_IDLE;
                fprintf(stderr, "[pisugar] LONG PRESS on release (%llu ms)\n",
                        (unsigned long long)hold_duration);
            } else {
                /* Medium press - treat as single tap */
                ctx->btn_release_time = now;
                ctx->btn_state = BTN_RELEASED_ONCE;
            }
        }
        break;

    case BTN_RELEASED_ONCE:
        if (btn_event) {
            /* Second press within the double-tap window! */
            uint64_t gap = now - ctx->btn_release_time;
            if (gap <= TAP_DOUBLE_GAP_MS) {
                result = TAP_DOUBLE;
                ctx->btn_state = BTN_IDLE;
                fprintf(stderr, "[pisugar] DOUBLE TAP detected (gap=%llu ms)\n",
                        (unsigned long long)gap);
            } else {
                /* Too slow for double - report first as single,
                 * start tracking this as new press */
                result = TAP_SINGLE;
                ctx->btn_press_time = now;
                ctx->btn_state = BTN_PRESSED;
                fprintf(stderr, "[pisugar] SINGLE TAP (late 2nd press)\n");
            }
        } else {
            /* Still waiting for possible 2nd press */
            uint64_t wait = now - ctx->btn_release_time;
            if (wait > TAP_DOUBLE_GAP_MS) {
                /* Double-tap window expired -> it's a single tap */
                result = TAP_SINGLE;
                ctx->btn_state = BTN_IDLE;
                fprintf(stderr, "[pisugar] SINGLE TAP detected\n");
            }
        }
        break;

    case BTN_LONG_FIRED:
        /* Long press already reported, wait for no more events */
        if (!btn_event) {
            /* Debounce: wait a bit after last event */
            if ((now - ctx->btn_press_time) > TAP_LONG_THRESH_MS + 500) {
                ctx->btn_state = BTN_IDLE;
            }
        } else {
            /* Still getting events, update press time */
            ctx->btn_press_time = now;
        }
        break;
    }

    return result;
}

/* ============================================================================
 * Public API Implementation
 * ========================================================================== */

pisugar_ctx_t *pisugar_init(void) {
    pisugar_ctx_t *ctx = calloc(1, sizeof(pisugar_ctx_t));
    if (!ctx) return NULL;

    /* Open I2C bus */
    char i2c_dev[20];
    snprintf(i2c_dev, sizeof(i2c_dev), "/dev/i2c-%d", PISUGAR_I2C_BUS);

    ctx->i2c_fd = open(i2c_dev, O_RDWR);
    if (ctx->i2c_fd < 0) {
        fprintf(stderr, "[pisugar] failed to open %s: %s\n", i2c_dev, strerror(errno));
        free(ctx);
        return NULL;
    }

    /* Retry probe - PiSugar3 MCU may boot slower than the Pi */
    int test = -1;
    for (int attempt = 0; attempt < 5; attempt++) {
        test = smbus_read_byte(ctx->i2c_fd, PISUGAR_ADDR, PS3_REG_BATTERY);
        if (test >= 0) break;
        if (attempt < 4) {
            fprintf(stderr, "[pisugar] probe attempt %d/5 failed for 0x%02X, retrying in 3s...\n",
                    attempt + 1, PISUGAR_ADDR);
            sleep(3);
        }
    }
    if (test >= 0) {
        ctx->i2c_addr = PISUGAR_ADDR;
        ctx->connected = true;
        fprintf(stderr, "[pisugar] PiSugar3 connected at 0x%02X (battery=%d%%)\n",
                PISUGAR_ADDR, test);
    }

    /* Try alternate address (0x75 for PiSugar2) */
    if (!ctx->connected) {
        test = smbus_read_byte(ctx->i2c_fd, PISUGAR_ADDR_ALT, PS3_REG_BATTERY);
        if (test >= 0) {
            ctx->i2c_addr = PISUGAR_ADDR_ALT;
            ctx->connected = true;
            fprintf(stderr, "[pisugar] PiSugar connected at 0x%02X (battery=%d%%)\n",
                    PISUGAR_ADDR_ALT, test);
        }
    }

    if (!ctx->connected) {
        fprintf(stderr, "[pisugar] no PiSugar detected on I2C bus\n");
        close(ctx->i2c_fd);
        free(ctx);
        return NULL;
    }

    /* Clear any pending custom button state */
    int btn_val = smbus_read_byte(ctx->i2c_fd, ctx->i2c_addr, PS3_REG_CUSTOM_BTN);
    if (btn_val >= 0 && (btn_val & 0x01)) {
        smbus_write_byte(ctx->i2c_fd, ctx->i2c_addr, PS3_REG_CUSTOM_BTN,
                         (uint8_t)(btn_val & ~0x01));
        fprintf(stderr, "[pisugar] cleared pending custom button event\n");
    }

    /* Initialize tap detection state */
    ctx->btn_state = BTN_IDLE;
    ctx->btn_press_time = 0;
    ctx->btn_release_time = 0;
    ctx->btn_last_raw = 0;
    ctx->btn_last_change = 0;

    /* Read initial mode from files */
    ctx->current_mode = read_mode_from_files();
    fprintf(stderr, "[pisugar] current mode: %s\n",
            ctx->current_mode == MODE_AUTO ? "AUTO" : "MANUAL");

    /* Read initial battery status */
    pisugar_read_battery(ctx);
    pisugar_read_voltage(ctx);
    pisugar_is_charging(ctx);

    fprintf(stderr, "[pisugar] battery: %d%% (%dmV) charging=%s\n",
            ctx->battery_level, ctx->voltage_mv,
            ctx->charging ? "yes" : "no");

    fprintf(stderr, "[pisugar] custom button ready (reg 0x%02X bit 0)\n",
            PS3_REG_CUSTOM_BTN);
    fprintf(stderr, "[pisugar]   single tap = toggle AUTO/MANUAL\n");
    fprintf(stderr, "[pisugar]   double tap = reserved\n");
    fprintf(stderr, "[pisugar]   long press = reserved\n");

    return ctx;
}

void pisugar_destroy(pisugar_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->i2c_fd >= 0) close(ctx->i2c_fd);
    free(ctx);
}

int pisugar_read_battery(pisugar_ctx_t *ctx) {
    if (!ctx || !ctx->connected) return -1;

    int level = smbus_read_byte(ctx->i2c_fd, ctx->i2c_addr, PS3_REG_BATTERY);
    if (level >= 0) ctx->battery_level = level;
    return ctx->battery_level;
}

int pisugar_read_voltage(pisugar_ctx_t *ctx) {
    if (!ctx || !ctx->connected) return -1;

    int high = smbus_read_byte(ctx->i2c_fd, ctx->i2c_addr, PS3_REG_VOLT_HIGH);
    int low  = smbus_read_byte(ctx->i2c_fd, ctx->i2c_addr, PS3_REG_VOLT_LOW);

    if (high >= 0 && low >= 0) {
        ctx->voltage_mv = (high << 8) | low;
    }
    return ctx->voltage_mv;
}

bool pisugar_is_charging(pisugar_ctx_t *ctx) {
    if (!ctx || !ctx->connected) return false;

    int status = smbus_read_byte(ctx->i2c_fd, ctx->i2c_addr, PS3_REG_POWER_STATUS);
    if (status >= 0) {
        ctx->charging = (status & 0x80) != 0;  /* Bit 7 = external power */
    }
    return ctx->charging;
}

pwnagotchi_mode_t pisugar_get_mode(pisugar_ctx_t *ctx) {
    if (!ctx) return MODE_AUTO;
    return ctx->current_mode;
}

int pisugar_toggle_mode(pisugar_ctx_t *ctx) {
    if (!ctx) return -1;
    pwnagotchi_mode_t new_mode = (ctx->current_mode == MODE_AUTO) ? MODE_MANUAL : MODE_AUTO;
    return pisugar_set_mode(ctx, new_mode);
}

/**
 * Set mode WITHOUT restarting the service.
 * Just updates the mode files and internal state.
 * The brain/main loop checks pisugar_get_mode() to adjust behavior.
 */
int pisugar_set_mode(pisugar_ctx_t *ctx, pwnagotchi_mode_t mode) {
    if (!ctx) return -1;

    if (mode == ctx->current_mode) {
        fprintf(stderr, "[pisugar] already in %s mode\n",
                mode == MODE_AUTO ? "AUTO" : "MANUAL");
        return 0;
    }

    fprintf(stderr, "[pisugar] MODE SWITCH: %s -> %s\n",
            ctx->current_mode == MODE_AUTO ? "AUTO" : "MANUAL",
            mode == MODE_AUTO ? "AUTO" : "MANUAL");

    /* Update mode files for persistence */
    write_mode_files(mode);

    ctx->current_mode = mode;

    /* Fire callback (brain will react to this) */
    if (ctx->on_mode_change) {
        ctx->on_mode_change(mode, ctx->callback_user_data);
    }

    return 0;
}

void pisugar_set_callback(pisugar_ctx_t *ctx,
    void (*on_mode_change)(pwnagotchi_mode_t new_mode, void *user_data),
    void *user_data)
{
    if (!ctx) return;
    ctx->on_mode_change = on_mode_change;
    ctx->callback_user_data = user_data;
}
