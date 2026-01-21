/*
 * PwnaUI License System
 * 
 * Hardware-bound license validation using Ed25519 signatures.
 * License is tied to Raspberry Pi CPU serial number.
 * 
 * Author: PwnHub Project
 * License: Proprietary - All Rights Reserved
 */

#ifndef PWNAUI_LICENSE_H
#define PWNAUI_LICENSE_H

#include <stdint.h>
#include <stdbool.h>

/* License file location */
#define LICENSE_FILE_PATH       "/etc/pwnaui/license.key"
#define LICENSE_DIR_PATH        "/etc/pwnaui"

/* License status codes */
typedef enum {
    LICENSE_VALID = 0,
    LICENSE_MISSING = 1,
    LICENSE_INVALID = 2,
    LICENSE_EXPIRED = 3,
    LICENSE_WRONG_DEVICE = 4,
    LICENSE_CORRUPTED = 5
} license_status_t;

/* License data structure */
typedef struct {
    char device_serial[32];     /* Pi CPU serial (16 hex chars + null) */
    uint64_t issued_timestamp;  /* Unix timestamp when issued */
    uint64_t expiry_timestamp;  /* 0 = never expires (lifetime) */
    uint8_t features;           /* Feature flags (reserved for future) */
    uint8_t signature[64];      /* Ed25519 signature */
} license_data_t;

/* Feature flags */
#define LICENSE_FEATURE_BASIC       0x01
#define LICENSE_FEATURE_THEMES      0x02
#define LICENSE_FEATURE_PLUGINS     0x04
#define LICENSE_FEATURE_ALL         0xFF

/*
 * Initialize the license system
 * Reads CPU serial and checks for existing license
 * Returns: license status
 */
license_status_t license_init(void);

/*
 * Check if license is valid
 * Returns: true if licensed, false if locked
 */
bool license_is_valid(void);

/*
 * Get current license status
 */
license_status_t license_get_status(void);

/*
 * Get device serial number (for display/activation)
 * Returns: pointer to static buffer with serial string
 */
const char* license_get_device_serial(void);

/*
 * Install a new license from base64-encoded string
 * This is called when receiving license via IPC from mobile app
 * Returns: license status after installation attempt
 */
license_status_t license_install(const char *license_b64);

/*
 * Verify a license data structure
 * Returns: LICENSE_VALID if signature matches, error code otherwise
 */
license_status_t license_verify(const license_data_t *license);

/*
 * Get human-readable status message
 */
const char* license_status_string(license_status_t status);

/*
 * Check if a specific feature is enabled
 */
bool license_has_feature(uint8_t feature);

/*
 * Render the "locked" screen with activation instructions
 * Draws to the provided framebuffer
 */
void license_render_locked_screen(uint8_t *framebuffer, int width, int height);

#endif /* PWNAUI_LICENSE_H */
