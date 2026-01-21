/*
 * PwnaUI - Display Implementation
 * Hardware abstraction layer for e-ink and framebuffer displays
 * 
 * Supports:
 *   - Waveshare e-ink displays (via SPI)
 *   - Linux framebuffer (/dev/fb0)
 *   - Dummy display (for testing)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

#include "display.h"

/* SPI and GPIO headers (Linux-specific) */
#ifdef __linux__
#include <linux/spi/spidev.h>

/* BCM2835 GPIO registers (direct access for speed) */
#define BCM2835_PERI_BASE    0x20000000  /* RPi1/Zero */
#define BCM2835_PERI_BASE_2  0x3F000000  /* RPi2/3 */
#define BCM2835_PERI_BASE_4  0xFE000000  /* RPi4 */
#define GPIO_BASE_OFFSET     0x200000
#define BLOCK_SIZE           4096

/* GPIO pin definitions for Waveshare HAT */
#define EPD_RST_PIN   17
#define EPD_DC_PIN    25
#define EPD_CS_PIN    8
#define EPD_BUSY_PIN  24
#define EPD_PWR_PIN   18

/* E-ink commands (common across Waveshare displays) */
#define EPD_CMD_DRIVER_OUTPUT       0x01
#define EPD_CMD_GATE_VOLTAGE        0x03
#define EPD_CMD_SOURCE_VOLTAGE      0x04
#define EPD_CMD_DEEP_SLEEP          0x10
#define EPD_CMD_DATA_ENTRY          0x11
#define EPD_CMD_SOFT_RESET          0x12
#define EPD_CMD_TEMP_SENSOR         0x18
#define EPD_CMD_MASTER_ACTIVATE     0x20
#define EPD_CMD_DISPLAY_UPDATE1     0x21
#define EPD_CMD_DISPLAY_UPDATE2     0x22
#define EPD_CMD_WRITE_RAM           0x24
#define EPD_CMD_WRITE_RAM_RED       0x26
#define EPD_CMD_READ_RAM            0x27
#define EPD_CMD_VCOM_SENSE          0x28
#define EPD_CMD_VCOM_DURATION       0x29
#define EPD_CMD_WRITE_VCOM          0x2C
#define EPD_CMD_WRITE_LUT           0x32
#define EPD_CMD_OTP_READ            0x36
#define EPD_CMD_OTP_PROGRAM         0x37
#define EPD_CMD_BORDER_WAVEFORM     0x3C
#define EPD_CMD_SET_RAM_X           0x44
#define EPD_CMD_SET_RAM_Y           0x45
#define EPD_CMD_SET_RAM_X_ADDR      0x4E
#define EPD_CMD_SET_RAM_Y_ADDR      0x4F

#endif /* __linux__ */

/* Display state */
static display_type_t g_display_type = DISPLAY_DUMMY;
static int g_display_width = 250;
static int g_display_height = 122;
static int g_spi_fd = -1;
static int g_fb_fd = -1;
static uint8_t *g_fb_map = NULL;
static size_t g_fb_size = 0;
static volatile uint32_t *g_gpio_base = NULL;
static int g_gpio_mem_fd = -1;
static int g_spi_speed = 4000000;  /* 4 MHz default */

/* Internal framebuffer for compositing */
static uint8_t g_internal_fb[400 * 300 / 8];

/*
 * GPIO Functions (for Waveshare e-ink)
 */
#ifdef __linux__
static int gpio_init(void) {
    /* Detect Raspberry Pi model */
    FILE *f = fopen("/proc/device-tree/model", "r");
    uint32_t peri_base = BCM2835_PERI_BASE;
    
    if (f) {
        char model[128];
        if (fgets(model, sizeof(model), f)) {
            if (strstr(model, "Pi 4") || strstr(model, "Pi 5")) {
                peri_base = BCM2835_PERI_BASE_4;
            } else if (strstr(model, "Pi 2") || strstr(model, "Pi 3")) {
                peri_base = BCM2835_PERI_BASE_2;
            }
        }
        fclose(f);
    }
    
    g_gpio_mem_fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
    if (g_gpio_mem_fd < 0) {
        /* Try /dev/mem (requires root) */
        g_gpio_mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
        if (g_gpio_mem_fd < 0) {
            perror("Failed to open GPIO");
            return -1;
        }
        g_gpio_base = (volatile uint32_t *)mmap(NULL, BLOCK_SIZE,
                                                 PROT_READ | PROT_WRITE,
                                                 MAP_SHARED, g_gpio_mem_fd,
                                                 peri_base + GPIO_BASE_OFFSET);
    } else {
        g_gpio_base = (volatile uint32_t *)mmap(NULL, BLOCK_SIZE,
                                                 PROT_READ | PROT_WRITE,
                                                 MAP_SHARED, g_gpio_mem_fd, 0);
    }
    
    if (g_gpio_base == MAP_FAILED) {
        perror("GPIO mmap failed");
        close(g_gpio_mem_fd);
        return -1;
    }
    
    return 0;
}

static void gpio_cleanup(void) {
    if (g_gpio_base && g_gpio_base != MAP_FAILED) {
        munmap((void *)g_gpio_base, BLOCK_SIZE);
    }
    if (g_gpio_mem_fd >= 0) {
        close(g_gpio_mem_fd);
    }
}

static void gpio_set_mode(int pin, int mode) {
    if (!g_gpio_base) return;
    
    int reg = pin / 10;
    int shift = (pin % 10) * 3;
    
    g_gpio_base[reg] = (g_gpio_base[reg] & ~(7 << shift)) | (mode << shift);
}

static void gpio_write(int pin, int value) {
    if (!g_gpio_base) return;
    
    if (value) {
        g_gpio_base[7] = 1 << pin;   /* GPSET0 */
    } else {
        g_gpio_base[10] = 1 << pin;  /* GPCLR0 */
    }
}

static int gpio_read(int pin) {
    if (!g_gpio_base) return 0;
    return (g_gpio_base[13] >> pin) & 1;  /* GPLEV0 */
}
#endif /* __linux__ */

/*
 * SPI Functions
 */
#ifdef __linux__
static int spi_init(void) {
    g_spi_fd = open("/dev/spidev0.0", O_RDWR);
    if (g_spi_fd < 0) {
        perror("Failed to open SPI device");
        return -1;
    }
    
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t speed = g_spi_speed;
    
    ioctl(g_spi_fd, SPI_IOC_WR_MODE, &mode);
    ioctl(g_spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(g_spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
    
    return 0;
}

static void spi_cleanup(void) {
    if (g_spi_fd >= 0) {
        close(g_spi_fd);
        g_spi_fd = -1;
    }
}

static void spi_transfer(const uint8_t *tx, uint8_t *rx, size_t len) {
    if (g_spi_fd < 0) return;
    
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)tx,
        .rx_buf = (unsigned long)rx,
        .len = len,
        .speed_hz = g_spi_speed,
        .bits_per_word = 8,
    };
    
    ioctl(g_spi_fd, SPI_IOC_MESSAGE(1), &tr);
}

static void spi_write_byte(uint8_t data) {
    spi_transfer(&data, NULL, 1);
}
#endif /* __linux__ */

/*
 * E-ink display functions
 */
#ifdef __linux__
static void epd_reset(void) {
    gpio_write(EPD_RST_PIN, 1);
    usleep(20000);
    gpio_write(EPD_RST_PIN, 0);
    usleep(2000);
    gpio_write(EPD_RST_PIN, 1);
    usleep(20000);
}

static void epd_send_command(uint8_t cmd) {
    gpio_write(EPD_DC_PIN, 0);  /* Command mode */
    gpio_write(EPD_CS_PIN, 0);  /* Select */
    spi_write_byte(cmd);
    gpio_write(EPD_CS_PIN, 1);  /* Deselect */
    usleep(10);  /* Give controller time to process command */
}

static void epd_send_data(uint8_t data) {
    gpio_write(EPD_DC_PIN, 1);  /* Data mode */
    gpio_write(EPD_CS_PIN, 0);
    spi_write_byte(data);
    gpio_write(EPD_CS_PIN, 1);
    usleep(5);  /* Small delay between data bytes */
}

static void epd_send_data_burst(const uint8_t *data, size_t len) {
    gpio_write(EPD_DC_PIN, 1);
    gpio_write(EPD_CS_PIN, 0);
    spi_transfer(data, NULL, len);
    gpio_write(EPD_CS_PIN, 1);
    usleep(100);  /* Let controller process bulk data */
}

static void epd_wait_busy(void) {
    while (gpio_read(EPD_BUSY_PIN) == 1) {
        usleep(10000);
    }
    usleep(10000);
}

/*
 * Waveshare 2.13" V2 initialization
 */
static int epd_init_2in13_v2(int full_update) {
    /* Setup GPIO pins */
    gpio_set_mode(EPD_RST_PIN, 1);   /* Output */
    gpio_set_mode(EPD_DC_PIN, 1);    /* Output */
    gpio_set_mode(EPD_CS_PIN, 1);    /* Output */
    gpio_set_mode(EPD_BUSY_PIN, 0);  /* Input */
    
    gpio_write(EPD_CS_PIN, 1);
    
    epd_reset();
    epd_wait_busy();
    
    epd_send_command(0x12);  /* Software reset */
    epd_wait_busy();
    
    epd_send_command(0x01);  /* Driver output control */
    epd_send_data(0xF9);
    epd_send_data(0x00);
    epd_send_data(0x00);
    
    epd_send_command(0x11);  /* Data entry mode */
    epd_send_data(0x03);     /* X increment, Y increment */
    
    /* Set RAM X address */
    epd_send_command(0x44);
    epd_send_data(0x00);
    epd_send_data(0x0F);  /* (122-1)/8 = 15 */
    
    /* Set RAM Y address */
    epd_send_command(0x45);
    epd_send_data(0x00);
    epd_send_data(0x00);
    epd_send_data(0xF9);  /* 250-1 */
    epd_send_data(0x00);
    
    epd_send_command(0x3C);  /* Border waveform */
    epd_send_data(0x05);
    
    epd_send_command(0x21);  /* Display update control */
    epd_send_data(0x00);
    epd_send_data(0x80);
    
    epd_send_command(0x18);  /* Temperature sensor */
    epd_send_data(0x80);
    
    /* Set RAM position */
    epd_send_command(0x4E);
    epd_send_data(0x00);
    epd_send_command(0x4F);
    epd_send_data(0x00);
    epd_send_data(0x00);
    
    epd_wait_busy();
    
    return 0;
}

/*
 * Transpose framebuffer from logical (250×122) row-major to e-ink (122×250) format
 * The e-ink panel is physically 122 wide × 250 tall
 * Our renderer draws in landscape 250×122 (width×height)
 * We need to rotate 90° clockwise for correct display
 */
static void transpose_framebuffer_for_epd(const uint8_t *src, uint8_t *dst,
                                          int src_width, int src_height) {
    /* Source: 250×122, packed as rows of 250 bits (32 bytes/row), 122 rows
     * Dest: 122×250, packed as rows of 122 bits (16 bytes/row), 250 rows
     * We rotate 90° CW: src(x,y) -> dst(src_height-1-y, x)
     */
    int dst_width = src_height;   /* 122 */
    int dst_height = src_width;   /* 250 */
    int dst_row_bytes = (dst_width + 7) / 8;  /* 16 bytes */
    
    /* Clear destination */
    memset(dst, 0xFF, dst_row_bytes * dst_height);
    
    for (int sy = 0; sy < src_height; sy++) {
        for (int sx = 0; sx < src_width; sx++) {
            /* Get source pixel */
            int src_byte = (sy * src_width + sx) / 8;
            int src_bit = 7 - (sx % 8);
            int pixel = (src[src_byte] >> src_bit) & 1;
            
            /* 90° CW rotation: (sx, sy) -> (src_height - 1 - sy, sx) */
            int dx = src_height - 1 - sy;
            int dy = sx;
            
            /* Set destination pixel */
            int dst_byte = dy * dst_row_bytes + dx / 8;
            int dst_bit = 7 - (dx % 8);
            
            if (pixel) {
                dst[dst_byte] |= (1 << dst_bit);
            } else {
                dst[dst_byte] &= ~(1 << dst_bit);
            }
        }
    }
}

static void epd_display_2in13_v2(const uint8_t *image, int partial) {
    int epd_width = 122;
    int epd_height = 250;
    int epd_row_bytes = (epd_width + 7) / 8;  /* 16 bytes per row */
    
    /* Transpose from our 250×122 framebuffer to e-ink's 122×250 format */
    static uint8_t transposed[16 * 250];  /* 4000 bytes */
    transpose_framebuffer_for_epd(image, transposed, 250, 122);
    
    /* Set RAM position */
    epd_send_command(0x4E);
    epd_send_data(0x00);
    epd_send_command(0x4F);
    epd_send_data(0x00);
    epd_send_data(0x00);
    
    /* Write image data to RAM */
    epd_send_command(0x24);
    epd_send_data_burst(transposed, epd_row_bytes * epd_height);
    
    /* Update display */
    epd_send_command(0x22);
    if (partial) {
        /* Partial refresh - faster, no blink but may ghost */
        epd_send_data(0xFF);
    } else {
        /* Full refresh - slower, blinks but clears ghosting */
        epd_send_data(0xF7);
    }
    epd_send_command(0x20);
    epd_wait_busy();
    
    /* Double display trick for cleaner partial (reduces ghosting) */
    if (partial) {
        epd_send_command(0x4E);
        epd_send_data(0x00);
        epd_send_command(0x4F);
        epd_send_data(0x00);
        epd_send_data(0x00);
        epd_send_command(0x24);
        epd_send_data_burst(transposed, epd_row_bytes * epd_height);
        epd_send_command(0x22);
        epd_send_data(0xFF);
        epd_send_command(0x20);
        epd_wait_busy();
    }
}

static void epd_clear_2in13_v2(void) {
    int w_bytes = (122 + 7) / 8;
    int h = 250;
    
    epd_send_command(0x4E);
    epd_send_data(0x00);
    epd_send_command(0x4F);
    epd_send_data(0x00);
    epd_send_data(0x00);
    
    epd_send_command(0x24);
    for (int i = 0; i < w_bytes * h; i++) {
        epd_send_data(0xFF);
    }
    
    epd_send_command(0x22);
    epd_send_data(0xF7);
    epd_send_command(0x20);
    epd_wait_busy();
}

static void epd_sleep_2in13_v2(void) {
    epd_send_command(0x10);
    epd_send_data(0x01);
    usleep(100000);
}

/*
 * Waveshare 2.13" V4 specific functions
 * V4 requires base image initialization for proper partial refresh
 */
static int g_v4_base_initialized = 0;

static void epd_init_base_image_v4(const uint8_t *image) {
    int epd_width = 122;
    int epd_height = 250;
    int epd_row_bytes = (epd_width + 7) / 8;
    
    static uint8_t transposed[16 * 250];
    transpose_framebuffer_for_epd(image, transposed, 250, 122);
    
    /* Write to RAM Black (0x24) */
    epd_send_command(0x24);
    epd_send_data_burst(transposed, epd_row_bytes * epd_height);
    
    /* Write to RAM Red/Old (0x26) - this is the "base" for partial */
    epd_send_command(0x26);
    epd_send_data_burst(transposed, epd_row_bytes * epd_height);
    
    /* Full refresh to establish base */
    epd_send_command(0x22);
    epd_send_data(0xF7);
    epd_send_command(0x20);
    epd_wait_busy();
    
    g_v4_base_initialized = 1;
}

static void epd_display_2in13_v4(const uint8_t *image, int partial) {
    int epd_width = 122;
    int epd_height = 250;
    int epd_row_bytes = (epd_width + 7) / 8;  /* 16 bytes per row */
    
    /* If base not initialized, do full refresh first */
    if (!g_v4_base_initialized) {
        epd_init_base_image_v4(image);
        return;
    }
    
    /* Transpose from our 250×122 framebuffer to e-ink's 122×250 format */
    static uint8_t transposed[16 * 250];  /* 4000 bytes */
    transpose_framebuffer_for_epd(image, transposed, 250, 122);
    
    if (partial) {
        /* V4 Partial refresh - exact sequence from Waveshare Python driver */
        /* Quick reset pulse */
        gpio_write(EPD_RST_PIN, 0);
        usleep(1000);
        gpio_write(EPD_RST_PIN, 1);
        
        epd_send_command(0x3C);  /* Border waveform */
        epd_send_data(0x80);
        
        epd_send_command(0x01);  /* Driver output control */
        epd_send_data(0xF9);
        epd_send_data(0x00);
        epd_send_data(0x00);
        
        epd_send_command(0x11);  /* Data entry mode */
        epd_send_data(0x03);
        
        /* Set window */
        epd_send_command(0x44);
        epd_send_data(0x00);
        epd_send_data((epd_width - 1) >> 3);
        epd_send_command(0x45);
        epd_send_data(0x00);
        epd_send_data(0x00);
        epd_send_data((epd_height - 1) & 0xFF);
        epd_send_data((epd_height - 1) >> 8);
        
        /* Set cursor */
        epd_send_command(0x4E);
        epd_send_data(0x00);
        epd_send_command(0x4F);
        epd_send_data(0x00);
        epd_send_data(0x00);
        
        /* Write ONLY to RAM Black (0x24), not to 0x26 */
        epd_send_command(0x24);
        epd_send_data_burst(transposed, epd_row_bytes * epd_height);
        
        /* Partial update - NO BLINK */
        epd_send_command(0x22);
        epd_send_data(0xFF);  /* Partial mode */
        epd_send_command(0x20);
        epd_wait_busy();
    } else {
        /* Full refresh - updates both RAM buffers and blinks */
        epd_send_command(0x4E);
        epd_send_data(0x00);
        epd_send_command(0x4F);
        epd_send_data(0x00);
        epd_send_data(0x00);
        
        /* Write to both RAM buffers */
        epd_send_command(0x24);
        epd_send_data_burst(transposed, epd_row_bytes * epd_height);
        epd_send_command(0x26);
        epd_send_data_burst(transposed, epd_row_bytes * epd_height);
        
        epd_send_command(0x22);
        epd_send_data(0xF7);  /* Full refresh */
        epd_send_command(0x20);
        epd_wait_busy();
    }
}
#endif /* __linux__ */

/*
 * Framebuffer display functions
 */
static int fb_init(void) {
#ifdef __linux__
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    
    g_fb_fd = open("/dev/fb0", O_RDWR);
    if (g_fb_fd < 0) {
        perror("Failed to open framebuffer");
        return -1;
    }
    
    if (ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("FBIOGET_VSCREENINFO");
        close(g_fb_fd);
        return -1;
    }
    
    if (ioctl(g_fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("FBIOGET_FSCREENINFO");
        close(g_fb_fd);
        return -1;
    }
    
    g_display_width = vinfo.xres;
    g_display_height = vinfo.yres;
    g_fb_size = finfo.smem_len;
    
    g_fb_map = mmap(NULL, g_fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);
    if (g_fb_map == MAP_FAILED) {
        perror("Framebuffer mmap failed");
        close(g_fb_fd);
        return -1;
    }
    
    return 0;
#else
    return -1;
#endif
}

static void fb_cleanup(void) {
#ifdef __linux__
    if (g_fb_map && g_fb_map != MAP_FAILED) {
        munmap(g_fb_map, g_fb_size);
    }
    if (g_fb_fd >= 0) {
        close(g_fb_fd);
    }
#endif
}

static void fb_update(const uint8_t *framebuffer) {
#ifdef __linux__
    if (!g_fb_map) return;
    
    /* Convert 1-bit to framebuffer format (assuming 16bpp or 32bpp) */
    struct fb_var_screeninfo vinfo;
    ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &vinfo);
    
    int bpp = vinfo.bits_per_pixel / 8;
    
    for (int y = 0; y < g_display_height; y++) {
        for (int x = 0; x < g_display_width; x++) {
            int src_byte = (y * g_display_width + x) / 8;
            int src_bit = 7 - (x % 8);
            int pixel = (framebuffer[src_byte] >> src_bit) & 1;
            
            int dst_offset = (y * vinfo.xres + x) * bpp;
            
            if (bpp == 2) {
                /* 16bpp RGB565 */
                uint16_t color = pixel ? 0xFFFF : 0x0000;
                *(uint16_t *)(g_fb_map + dst_offset) = color;
            } else if (bpp == 4) {
                /* 32bpp RGBA */
                uint32_t color = pixel ? 0xFFFFFFFF : 0x000000FF;
                *(uint32_t *)(g_fb_map + dst_offset) = color;
            }
        }
    }
#endif
}

/* Bits per pixel for each display type */
static int g_display_bpp = 1;
static int g_sleeping = 0;

/*
 * Public API
 */
int display_init(display_type_t type, int width, int height) {
    g_display_type = type;
    g_display_bpp = 1;
    g_sleeping = 0;
    
    if (width > 0) g_display_width = width;
    if (height > 0) g_display_height = height;
    
    switch (type) {
        case DISPLAY_DUMMY:
            if (width <= 0) g_display_width = 250;
            if (height <= 0) g_display_height = 122;
            return 0;
            
        case DISPLAY_FRAMEBUFFER:
            return fb_init();
            
#ifdef __linux__
        case DISPLAY_WAVESHARE_2IN13_V2:
        case DISPLAY_WAVESHARE_2IN13_V3:
        case DISPLAY_WAVESHARE_2IN13_V4:
            if (width <= 0) g_display_width = 250;
            if (height <= 0) g_display_height = 122;
            
            if (gpio_init() < 0) return -1;
            if (spi_init() < 0) {
                gpio_cleanup();
                return -1;
            }
            
            return epd_init_2in13_v2(1);
            
        case DISPLAY_WAVESHARE_2IN7:
            if (width <= 0) g_display_width = 264;
            if (height <= 0) g_display_height = 176;
            
            if (gpio_init() < 0) return -1;
            if (spi_init() < 0) {
                gpio_cleanup();
                return -1;
            }
            /* TODO: Implement 2.7" init */
            return 0;
            
        case DISPLAY_WAVESHARE_1IN54:
            if (width <= 0) g_display_width = 200;
            if (height <= 0) g_display_height = 200;
            /* TODO: Implement 1.54" init */
            return 0;
            
        case DISPLAY_INKY_PHAT:
            if (width <= 0) g_display_width = 212;
            if (height <= 0) g_display_height = 104;
            /* TODO: Implement Inky driver */
            return 0;
#endif
            
        default:
            /* Fall back to dummy */
            g_display_type = DISPLAY_DUMMY;
            if (width <= 0) g_display_width = 250;
            if (height <= 0) g_display_height = 122;
            return 0;
    }
}

display_type_t display_get_type(void) {
    return g_display_type;
}

void display_cleanup(void) {
    switch (g_display_type) {
        case DISPLAY_FRAMEBUFFER:
            fb_cleanup();
            break;
            
#ifdef __linux__
        case DISPLAY_WAVESHARE_2IN13_V2:
        case DISPLAY_WAVESHARE_2IN13_V3:
        case DISPLAY_WAVESHARE_2IN13_V4:
        case DISPLAY_WAVESHARE_2IN7:
            epd_sleep_2in13_v2();
            spi_cleanup();
            gpio_cleanup();
            break;
#endif
            
        default:
            break;
    }
    
    g_display_type = DISPLAY_DUMMY;
    g_sleeping = 0;
}

int display_clear(int color) {
    uint8_t fill = color ? 0x00 : 0xFF;
    memset(g_internal_fb, fill, sizeof(g_internal_fb));
    
    switch (g_display_type) {
        case DISPLAY_FRAMEBUFFER:
            fb_update(g_internal_fb);
            break;
            
#ifdef __linux__
        case DISPLAY_WAVESHARE_2IN13_V2:
        case DISPLAY_WAVESHARE_2IN13_V3:
        case DISPLAY_WAVESHARE_2IN13_V4:
            epd_clear_2in13_v2();
            break;
#endif
            
        default:
            break;
    }
    
    return 0;
}

int display_update(const uint8_t *framebuffer) {
    if (!framebuffer) return -1;
    
    switch (g_display_type) {
        case DISPLAY_DUMMY:
            /* No-op */
            break;
            
        case DISPLAY_FRAMEBUFFER:
            fb_update(framebuffer);
            break;
            
#ifdef __linux__
        case DISPLAY_WAVESHARE_2IN13_V2:
        case DISPLAY_WAVESHARE_2IN13_V3:
            epd_display_2in13_v2(framebuffer, 0);
            break;
        case DISPLAY_WAVESHARE_2IN13_V4:
            epd_display_2in13_v4(framebuffer, 0);
            break;
#endif
            
        default:
            break;
    }
    
    return 0;
}

int display_partial_update(const uint8_t *framebuffer, int x, int y, int w, int h) {
    if (!framebuffer) return -1;
    if (!display_supports_partial()) return display_update(framebuffer);
    
    /* Ignore x,y,w,h for now - update whole screen with partial refresh */
    (void)x; (void)y; (void)w; (void)h;
    
    switch (g_display_type) {
#ifdef __linux__
        case DISPLAY_WAVESHARE_2IN13_V2:
        case DISPLAY_WAVESHARE_2IN13_V3:
            epd_display_2in13_v2(framebuffer, 1);  /* partial = 1 */
            break;
        case DISPLAY_WAVESHARE_2IN13_V4:
            epd_display_2in13_v4(framebuffer, 1);  /* partial = 1 */
            break;
#endif
        default:
            return display_update(framebuffer);
    }
    
    return 0;
}

int display_get_width(void) {
    return g_display_width;
}

int display_get_height(void) {
    return g_display_height;
}

int display_supports_partial(void) {
    switch (g_display_type) {
        case DISPLAY_WAVESHARE_2IN13_V2:
        case DISPLAY_WAVESHARE_2IN13_V3:
        case DISPLAY_WAVESHARE_2IN13_V4:
            return 1;
        default:
            return 0;
    }
}

int display_supports_grayscale(void) {
    switch (g_display_type) {
        case DISPLAY_FRAMEBUFFER:
            return 1;
        default:
            return 0;
    }
}

int display_get_bpp(void) {
    return g_display_bpp;
}

size_t display_calc_buffer_size(int width, int height, int bpp) {
    if (width <= 0 || height <= 0 || bpp <= 0) return 0;
    
    if (bpp == 1) {
        return (size_t)((width + 7) / 8) * height;
    }
    return (size_t)width * height * ((bpp + 7) / 8);
}

int display_set_spi_speed(int speed_hz) {
    if (speed_hz <= 0) return -1;
    g_spi_speed = speed_hz;
    return 0;
}

int display_sleep(void) {
    if (g_sleeping) return 0;
    
    switch (g_display_type) {
#ifdef __linux__
        case DISPLAY_WAVESHARE_2IN13_V2:
        case DISPLAY_WAVESHARE_2IN13_V3:
        case DISPLAY_WAVESHARE_2IN13_V4:
            epd_sleep_2in13_v2();
            break;
#endif
        default:
            break;
    }
    
    g_sleeping = 1;
    return 0;
}

int display_wake(void) {
    if (!g_sleeping) return 0;
    
    switch (g_display_type) {
#ifdef __linux__
        case DISPLAY_WAVESHARE_2IN13_V2:
        case DISPLAY_WAVESHARE_2IN13_V3:
        case DISPLAY_WAVESHARE_2IN13_V4:
            epd_reset();
            epd_init_2in13_v2(1);
            break;
#endif
        default:
            break;
    }
    
    g_sleeping = 0;
    return 0;
}

const char *display_type_name(display_type_t type) {
    switch (type) {
        case DISPLAY_DUMMY:            return "dummy";
        case DISPLAY_FRAMEBUFFER:      return "framebuffer";
        case DISPLAY_WAVESHARE_2IN13_V2: return "waveshare_2in13_v2";
        case DISPLAY_WAVESHARE_2IN13_V3: return "waveshare_2in13_v3";
        case DISPLAY_WAVESHARE_2IN13_V4: return "waveshare_2in13_v4";
        case DISPLAY_WAVESHARE_2IN7:   return "waveshare_2in7";
        case DISPLAY_WAVESHARE_1IN54:  return "waveshare_1in54";
        case DISPLAY_INKY_PHAT:        return "inky_phat";
        default:                       return "unknown";
    }
}
