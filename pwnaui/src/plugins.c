/*
 * PwnaUI - Native C Plugin Implementations
 * 
 * Replaces Python plugins with native C for lower CPU and jitter.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/sysinfo.h>
#include <sys/statvfs.h>
#include <dirent.h>

#include "plugins.h"

/* ============================================================================
 * UTILITY FUNCTIONS
 * ============================================================================ */

static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ============================================================================
 * MEMTEMP PLUGIN
 * ============================================================================ */

static unsigned long long prev_total = 0;
static unsigned long long prev_idle = 0;

int plugin_memtemp_init(void) {
    prev_total = 0;
    prev_idle = 0;
    return 0;
}

static int read_cpu_usage(void) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return 0;
    
    unsigned long long user, nice, system, idle, iowait, irq, softirq;
    if (fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq) != 7) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    
    unsigned long long total = user + nice + system + idle + iowait + irq + softirq;
    unsigned long long total_diff = total - prev_total;
    unsigned long long idle_diff = idle - prev_idle;
    
    prev_total = total;
    prev_idle = idle;
    
    if (total_diff == 0) return 0;
    
    return (int)(100 * (total_diff - idle_diff) / total_diff);
}

static int read_memory_usage(void) {
    struct sysinfo info;
    if (sysinfo(&info) != 0) return 0;
    
    unsigned long total = info.totalram / 1024;
    unsigned long free = info.freeram / 1024;
    unsigned long buffers = info.bufferram / 1024;
    
    /* Read cached from /proc/meminfo */
    unsigned long cached = 0;
    FILE *fp = fopen("/proc/meminfo", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "Cached:", 7) == 0) {
                sscanf(line, "Cached: %lu", &cached);
                break;
            }
        }
        fclose(fp);
    }
    
    unsigned long used = total - free - buffers - cached;
    return (int)(100 * used / total);
}

static int read_cpu_temp(void) {
    FILE *fp = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!fp) return 0;
    
    int temp_milli;
    if (fscanf(fp, "%d", &temp_milli) != 1) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    
    return temp_milli / 1000;  /* Convert from millidegrees */
}

int plugin_memtemp_update(memtemp_data_t *data) {
    data->mem_percent = read_memory_usage();
    data->cpu_percent = read_cpu_usage();
    data->temp_celsius = read_cpu_temp();
    
    /* Format header and data with fixed-width left-aligned columns */
    /* 4 chars per column = 12 total, fits ~72px */
    char mem_str[8], cpu_str[8], tmp_str[8];
    snprintf(mem_str, sizeof(mem_str), "%d%%", data->mem_percent);
    snprintf(cpu_str, sizeof(cpu_str), "%d%%", data->cpu_percent);
    snprintf(tmp_str, sizeof(tmp_str), "%dC", data->temp_celsius);
    
    snprintf(data->header, sizeof(data->header), "%-4s%-4s%-4s", "mem", "cpu", "tmp");
    snprintf(data->data, sizeof(data->data), "%-4s%-4s%-4s", mem_str, cpu_str, tmp_str);
    
    return 0;
}

void plugin_memtemp_cleanup(void) {
    /* Nothing to cleanup */
}


/* ============================================================================
 * BATTERY PLUGIN - Multi-device support (PiSugar, UPS Lite, etc.)
 * 
 * Supported devices:
 * - PiSugar 2/2Plus: I2C address 0x75
 * - PiSugar 3/3Plus: I2C address 0x57
 * - UPS Lite v1.3:   I2C address 0x62 (CW2015 fuel gauge) + GPIO4 for charging
 * ============================================================================ */

#include <linux/i2c-dev.h>
#include <linux/i2c.h>       /* For SMBus structures */
#include <sys/ioctl.h>
#include <sys/mman.h>

/* I2C addresses for different battery devices */
#define PISUGAR3_I2C_ADDR   0x57   /* PiSugar 3/3Plus */
#define PISUGAR2_I2C_ADDR   0x75   /* PiSugar 2/2Plus */
#define UPSLITE_I2C_ADDR    0x62   /* UPS Lite CW2015 fuel gauge */
#define I2C_BUS             "/dev/i2c-1"

/* UPS Lite CW2015 registers */
#define CW2015_REG_VCELL    0x02   /* Voltage cell */
#define CW2015_REG_SOC      0x04   /* State of charge */
#define CW2015_REG_MODE     0x0A   /* Mode register */

/* GPIO for UPS Lite charging detection */
#define UPSLITE_CHARGE_GPIO 4      /* GPIO4 = BCM pin for charging status */

static int i2c_fd = -1;
static battery_device_t detected_device = BATTERY_NONE;
static volatile uint32_t *gpio_base = NULL;  /* Memory-mapped GPIO */

/* Memory-mapped GPIO access for fast reads */
#define BCM2835_PERI_BASE   0x20000000   /* Pi 1/Zero */
#define BCM2836_PERI_BASE   0x3F000000   /* Pi 2/3 */
#define BCM2711_PERI_BASE   0xFE000000   /* Pi 4 */
#define GPIO_BASE_OFFSET    0x200000
#define GPIO_BLOCK_SIZE     (4*1024)

#define GPIO_INP(g)         (*(gpio_base + ((g)/10)) &= ~(7 << (((g)%10)*3)))
#define GPIO_GET(g)         ((*(gpio_base + 13) >> (g)) & 1)

static int gpio_init(void) {
    if (gpio_base != NULL) return 0;  /* Already initialized */
    
    int mem_fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        /* Fall back to /dev/mem (requires root) */
        mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
        if (mem_fd < 0) return -1;
        
        /* Try different peripheral bases for Pi versions */
        off_t bases[] = { BCM2711_PERI_BASE, BCM2836_PERI_BASE, BCM2835_PERI_BASE };
        for (int i = 0; i < 3 && gpio_base == NULL; i++) {
            gpio_base = (uint32_t *)mmap(NULL, GPIO_BLOCK_SIZE,
                                         PROT_READ | PROT_WRITE,
                                         MAP_SHARED, mem_fd,
                                         bases[i] + GPIO_BASE_OFFSET);
            if (gpio_base == MAP_FAILED) gpio_base = NULL;
        }
        close(mem_fd);
    } else {
        /* /dev/gpiomem is offset 0 */
        gpio_base = (uint32_t *)mmap(NULL, GPIO_BLOCK_SIZE,
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED, mem_fd, 0);
        close(mem_fd);
        if (gpio_base == MAP_FAILED) gpio_base = NULL;
    }
    
    if (gpio_base == NULL) return -1;
    
    /* Set GPIO4 as input for charging detection */
    GPIO_INP(UPSLITE_CHARGE_GPIO);
    return 0;
}

static int gpio_read_charging(void) {
    if (gpio_base == NULL) {
        /* Fallback: read from sysfs */
        FILE *fp = fopen("/sys/class/gpio/gpio4/value", "r");
        if (!fp) return -1;
        int val = fgetc(fp) - '0';
        fclose(fp);
        return val;
    }
    return GPIO_GET(UPSLITE_CHARGE_GPIO);
}

static int i2c_try_address(int addr) {
    if (ioctl(i2c_fd, I2C_SLAVE, addr) < 0) return -1;
    
    /* Try to read a byte to verify device responds */
    uint8_t buf[1];
    if (read(i2c_fd, buf, 1) < 0) return -1;
    
    return 0;
}

int plugin_battery_init(void) {
    detected_device = BATTERY_NONE;
    
    i2c_fd = open(I2C_BUS, O_RDWR);
    if (i2c_fd < 0) {
        printf("Battery: Failed to open %s\n", I2C_BUS);
        return -1;  /* No I2C bus */
    }
    
    /* Try to detect battery device by probing I2C addresses */
    printf("Battery: Probing I2C devices...\n");
    
    /* 1. Try PiSugar 3 (most common now) */
    if (i2c_try_address(PISUGAR3_I2C_ADDR) == 0) {
        detected_device = BATTERY_PISUGAR3;
        printf("Battery: Detected PiSugar 3 at 0x%02X\n", PISUGAR3_I2C_ADDR);
        return 0;
    }
    
    /* 2. Try PiSugar 2 */
    if (i2c_try_address(PISUGAR2_I2C_ADDR) == 0) {
        detected_device = BATTERY_PISUGAR2;
        printf("Battery: Detected PiSugar 2 at 0x%02X\n", PISUGAR2_I2C_ADDR);
        return 0;
    }
    
    /* 3. Try UPS Lite (CW2015) */
    if (i2c_try_address(UPSLITE_I2C_ADDR) == 0) {
        detected_device = BATTERY_UPSLITE;
        printf("Battery: Detected UPS Lite at 0x%02X\n", UPSLITE_I2C_ADDR);
        /* Initialize GPIO for charging detection */
        gpio_init();
        return 0;
    }
    
    /* No battery device found */
    printf("Battery: No device detected (tried 0x%02X, 0x%02X, 0x%02X)\n",
           PISUGAR3_I2C_ADDR, PISUGAR2_I2C_ADDR, UPSLITE_I2C_ADDR);
    close(i2c_fd);
    i2c_fd = -1;
    return -1;
}

/* SMBus read byte data - required for PiSugar 3 which doesn't work with raw I2C */
static int smbus_read_byte_data(uint8_t addr, uint8_t reg) {
    if (i2c_fd < 0) return -1;
    if (ioctl(i2c_fd, I2C_SLAVE, addr) < 0) return -1;
    
    struct i2c_smbus_ioctl_data args;
    union i2c_smbus_data data;
    
    args.read_write = I2C_SMBUS_READ;
    args.command = reg;
    args.size = I2C_SMBUS_BYTE_DATA;
    args.data = &data;
    
    if (ioctl(i2c_fd, I2C_SMBUS, &args) < 0) return -1;
    return data.byte & 0xFF;
}

static int i2c_read_reg(uint8_t addr, uint8_t reg) {
    /* Use SMBus for better device compatibility */
    return smbus_read_byte_data(addr, reg);
}

static int i2c_read_word(uint8_t addr, uint8_t reg) {
    if (i2c_fd < 0) return -1;
    if (ioctl(i2c_fd, I2C_SLAVE, addr) < 0) return -1;
    
    uint8_t buf[2] = { reg, 0 };
    if (write(i2c_fd, buf, 1) != 1) return -1;
    if (read(i2c_fd, buf, 2) != 2) return -1;
    
    /* Return as big-endian word (high byte first) */
    return (buf[0] << 8) | buf[1];
}

/* Read PiSugar 3 battery status */
static int pisugar3_read(battery_data_t *data) {
    /* PiSugar 3: Direct percentage at 0x2A, Voltage at 0x22-0x23, charging at 0x02 bit 7 */
    
    /* Read battery percentage directly from 0x2A register */
    int percent = i2c_read_reg(PISUGAR3_I2C_ADDR, 0x2A);
    if (percent < 0) return -1;
    
    if (percent > 100) percent = 100;
    data->percentage = percent;
    
    /* Read voltage (optional, for display) */
    int low = i2c_read_reg(PISUGAR3_I2C_ADDR, 0x23);
    int high = i2c_read_reg(PISUGAR3_I2C_ADDR, 0x22);
    if (low >= 0 && high >= 0) {
        data->voltage = ((high << 8) | low) / 1000.0f;
    } else {
        data->voltage = 0;
    }
    
    /* Charging status from control register bit 7 (external power connected) */
    int ctrl = i2c_read_reg(PISUGAR3_I2C_ADDR, 0x02);
    data->charging = (ctrl >= 0) && (ctrl & 0x80);
    
    return 0;
}

/* Read PiSugar 2 battery status */
static int pisugar2_read(battery_data_t *data) {
    /* PiSugar 2: Battery level at 0x2a */
    int percent = i2c_read_reg(PISUGAR2_I2C_ADDR, 0x2a);
    if (percent < 0) return -1;
    
    if (percent > 100) percent = 100;
    data->percentage = percent;
    
    /* Charging status (bit 7 of status register) */
    int status = i2c_read_reg(PISUGAR2_I2C_ADDR, 0x02);
    data->charging = (status >= 0) && (status & 0x80);
    
    data->voltage = 0;  /* Not easily readable on PiSugar 2 */
    return 0;
}

/* Read UPS Lite (CW2015 fuel gauge) battery status */
static int upslite_read(battery_data_t *data) {
    /* CW2015: Read voltage from VCELL register (0x02-0x03) */
    int vcell = i2c_read_word(UPSLITE_I2C_ADDR, CW2015_REG_VCELL);
    if (vcell < 0) {
        data->percentage = 0;
        data->charging = false;
        return -1;
    }
    
    /* Convert to voltage: swap bytes (little-endian) and scale */
    /* Formula: ((high << 8) | low) * 1.25 / 1000 / 16 */
    int swapped = ((vcell & 0xFF) << 8) | ((vcell >> 8) & 0xFF);
    data->voltage = swapped * 1.25f / 1000.0f / 16.0f;
    
    /* Read SOC (state of charge) from register 0x04-0x05 */
    int soc = i2c_read_word(UPSLITE_I2C_ADDR, CW2015_REG_SOC);
    if (soc >= 0) {
        /* Swap bytes and divide by 256 to get percentage */
        int swapped_soc = ((soc & 0xFF) << 8) | ((soc >> 8) & 0xFF);
        data->percentage = swapped_soc / 256;
        if (data->percentage > 100) data->percentage = 100;
        if (data->percentage < 0) data->percentage = 0;
    } else {
        data->percentage = 0;
    }
    
    /* Charging status from GPIO4 (HIGH = charging) */
    int gpio_val = gpio_read_charging();
    data->charging = (gpio_val == 1);
    
    return 0;
}

int plugin_battery_update(battery_data_t *data) {
    if (i2c_fd < 0 || detected_device == BATTERY_NONE) {
        data->percentage = -1;
        data->charging = false;
        data->available = false;
        data->device_type = BATTERY_NONE;
        snprintf(data->display, sizeof(data->display), "N/A");
        return -1;
    }
    
    data->available = true;
    data->device_type = detected_device;
    int result = -1;
    
    switch (detected_device) {
        case BATTERY_PISUGAR3:
            result = pisugar3_read(data);
            break;
        case BATTERY_PISUGAR2:
            result = pisugar2_read(data);
            break;
        case BATTERY_UPSLITE:
            result = upslite_read(data);
            break;
        default:
            break;
    }
    
    if (result < 0) {
        snprintf(data->display, sizeof(data->display), "ERR");
        return -1;
    }
    
    /* Format display string based on device type */
    const char *prefix = "";
    switch (detected_device) {
        case BATTERY_UPSLITE:
            prefix = "UPS ";
            break;
        default:
            prefix = "";
            break;
    }
    
    if (data->charging) {
        snprintf(data->display, sizeof(data->display), "%s%d%%+", prefix, data->percentage);
    } else {
        snprintf(data->display, sizeof(data->display), "%s%d%%", prefix, data->percentage);
    }
    
    return 0;
}

void plugin_battery_cleanup(void) {
    if (i2c_fd >= 0) {
        close(i2c_fd);
        i2c_fd = -1;
    }
    detected_device = BATTERY_NONE;
    
    /* Unmap GPIO memory */
    if (gpio_base != NULL) {
        munmap((void *)gpio_base, GPIO_BLOCK_SIZE);
        gpio_base = NULL;
    }
}


/* ============================================================================
 * BLUETOOTH PLUGIN
 * ============================================================================ */

int plugin_bluetooth_init(void) {
    return 0;
}

int plugin_bluetooth_update(bluetooth_data_t *data) {
    /* Check if bnep0 interface exists (BT PAN connected) */
    data->connected = false;
    data->tethered = false;
    strcpy(data->status, "BT-");
    data->device_name[0] = '\0';
    
    /* Check if bnep0 interface exists - if it exists, BT tether is connected */
    FILE *fp = fopen("/sys/class/net/bnep0/operstate", "r");
    if (fp) {
        /* bnep0 exists = connected (operstate can be "up", "unknown", etc) */
        char state[32] = {0};
        if (fgets(state, sizeof(state), fp)) {
            /* Connected unless explicitly "down" */
            if (strncmp(state, "down", 4) != 0) {
                data->connected = true;
                data->tethered = true;
                strcpy(data->status, "BT+");
            }
        }
        fclose(fp);
    }
    
    return 0;
}

void plugin_bluetooth_cleanup(void) {
    /* Nothing to cleanup */
}


/* ============================================================================
 * PLUGIN MANAGER
 * ============================================================================ */

int plugins_init(plugin_state_t *state) {
    memset(state, 0, sizeof(*state));
    
    /* Initialize memtemp (always enabled) */
    if (plugin_memtemp_init() == 0) {
        state->memtemp_enabled = true;
    }
    
    /* Initialize battery (if PiSugar present) */
    if (plugin_battery_init() == 0) {
        state->battery_enabled = true;
    }
    
    /* Initialize bluetooth */
    if (plugin_bluetooth_init() == 0) {
        state->bluetooth_enabled = true;
    }
    
    /* Initialize GPS CNClistener (for phone GPS CNCvia Bluetooth) */
    if (plugin_gps_init(&state->gps) == 0) {
        state->gps_enabled = true;
        printf("GPS: Plugin initialized successfully\n");
    } else {
        printf("GPS: Plugin initialization failed (will retry when bnep0 available)\n");
    }
    
    return 0;
}

#define PLUGIN_UPDATED_MEMTEMP    0x01
#define PLUGIN_UPDATED_BATTERY    0x02
#define PLUGIN_UPDATED_BLUETOOTH  0x04
#define PLUGIN_UPDATED_GPS       0x08

int plugins_update(plugin_state_t *state) {
    uint64_t now = get_time_ms();
    int updated = 0;
    
    /* Update memtemp */
    if (state->memtemp_enabled && 
        (now - state->last_memtemp_update >= PLUGIN_MEMTEMP_INTERVAL_MS)) {
        plugin_memtemp_update(&state->memtemp);
        state->last_memtemp_update = now;
        updated |= PLUGIN_UPDATED_MEMTEMP;
    }
    
    /* Update battery */
    if (state->battery_enabled &&
        (now - state->last_battery_update >= PLUGIN_BATTERY_INTERVAL_MS)) {
        plugin_battery_update(&state->battery);
        state->last_battery_update = now;
        updated |= PLUGIN_UPDATED_BATTERY;
    }
    
    /* Update bluetooth */
    if (state->bluetooth_enabled &&
        (now - state->last_bluetooth_update >= PLUGIN_BLUETOOTH_INTERVAL_MS)) {
        plugin_bluetooth_update(&state->bluetooth);
        state->last_bluetooth_update = now;
        updated |= PLUGIN_UPDATED_BLUETOOTH;
    }
    
    /* Update GPS CNC(timeout handling only - data comes from select loop) */
    if (state->gps_enabled) {
        if (plugin_gps_update(&state->gps)) {
            updated |= PLUGIN_UPDATED_GPS;
        }
    }
    
    return updated;
}

void plugins_cleanup(plugin_state_t *state) {
    if (state->memtemp_enabled) {
        plugin_memtemp_cleanup();
    }
    if (state->battery_enabled) {
        plugin_battery_cleanup();
    }
    if (state->bluetooth_enabled) {
        plugin_bluetooth_cleanup();
    }
    if (state->gps_enabled) {
        plugin_gps_cleanup(&state->gps);
    }
}
