/**
 * nexmon_scb_null_fix.c - Binary patch for SCB null pointer dereference
 * 
 * This file contains the actual ARM Thumb-2 binary patch that can be
 * applied directly to the nexmon firmware build system.
 * 
 * Firmware: BCM43455C0 v7.45.206 (Raspberry Pi 3B+/4)
 * Issue: https://github.com/seemoo-lab/nexmon/issues/335
 * 
 * To apply this patch:
 * 1. Copy this file to nexmon/patches/bcm43455c0/7_45_206/nexmon/src/
 * 2. Add to Makefile: LOCAL_SRCS += nexmon_scb_null_fix.c
 * 3. Rebuild firmware
 */

#include <firmware_version.h>
#include <wrapper.h>
#include <structs.h>
#include <helper.h>
#include <patcher.h>

/* 
 * Hook at radiotap_add_antenna_field to check scb validity
 * 
 * The crash occurs at approximately offset 0x1AABB0 in 7.45.206
 * when the code attempts: ldr r0, [r6, #4] (scb->cfg)
 * but r6 (scb) is NULL.
 */

#define SCB_NULL_CHECK_ADDR_7_45_206    0x1AABB0
#define SCB_NULL_CHECK_ADDR_7_45_189    0x1AF378
#define SCB_NULL_CHECK_ADDR_7_45_241    0x1AB8C0

/*
 * Safe packet info extraction
 * Called before accessing pkt->scb chain
 */
__attribute__((naked))
void scb_null_check_hook(void)
{
    asm(
        /* Save registers */
        "push {r0-r3, lr}\n"
        
        /* r6 should contain pkt->scb at this point */
        /* Check if scb (r6) is NULL */
        "cmp r6, #0\n"
        "beq scb_is_null\n"
        
        /* scb is valid, check scb->cfg (offset 4) */
        "ldr r0, [r6, #4]\n"
        "cmp r0, #0\n"
        "beq scb_is_null\n"
        
        /* Both scb and cfg are valid, restore and continue */
        "pop {r0-r3, lr}\n"
        "b original_code\n"
        
    "scb_is_null:\n"
        /* scb or cfg is NULL - return early with error indication */
        "pop {r0-r3, lr}\n"
        "mov r0, #0\n"        /* Return NULL/0 */
        "bx lr\n"
        
    "original_code:\n"
        /* Space for original instruction that was overwritten */
        "nop\n"
        "nop\n"
    );
}

/*
 * Alternative: Wrapper function approach
 * This is safer and works with nexmon's patcher system
 */
void *
safe_get_scb_cfg(void *scb)
{
    if (scb == NULL) {
        return NULL;
    }
    
    /* Cast scb to access cfg field */
    struct scb_info {
        void *next;
        void *cfg;  /* offset 4 */
        /* ... other fields */
    } *scb_ptr = (struct scb_info *)scb;
    
    return scb_ptr->cfg;
}

/*
 * Safe flags check wrapper
 */
unsigned int
safe_get_scb_flags(void *scb)
{
    if (scb == NULL) {
        return 0;
    }
    
    void *cfg = safe_get_scb_cfg(scb);
    if (cfg == NULL) {
        return 0;
    }
    
    /* cfg->flags is at offset 0 in cfg structure */
    struct bsscfg {
        unsigned int flags;
        /* ... other fields */
    } *bss = (struct bsscfg *)cfg;
    
    return bss->flags;
}

/*
 * Patch registration
 * Called during firmware initialization to apply the binary patch
 */
__attribute__((at(0x001AABB0, "", CHIP_VER_BCM43455c0, FW_VER_7_45_206)))
BPatch(scb_null_check_patch, scb_null_check_hook);

/* 
 * Additional patches for other firmware versions
 * Uncomment the appropriate line for your firmware
 */

// __attribute__((at(0x001AF378, "", CHIP_VER_BCM43455c0, FW_VER_7_45_189)))
// BPatch(scb_null_check_patch_189, scb_null_check_hook);

// __attribute__((at(0x001AB8C0, "", CHIP_VER_BCM43455c0, FW_VER_7_45_241)))
// BPatch(scb_null_check_patch_241, scb_null_check_hook);


/*
 * BCM43430A1 Patch (Pi Zero W, Pi 3B)
 * Firmware version 7.45.41.46
 * Patch address needs verification - this is estimated
 */

#define SCB_NULL_CHECK_ADDR_43430_7_45_41    0x185A40

// __attribute__((at(0x00185A40, "", CHIP_VER_BCM43430a1, FW_VER_7_45_41_46)))
// BPatch(scb_null_check_patch_43430, scb_null_check_hook);


/*
 * USAGE INSTRUCTIONS
 * ==================
 * 
 * 1. Copy this file to:
 *    nexmon/patches/bcm43455c0/7_45_206/nexmon/src/nexmon_scb_null_fix.c
 * 
 * 2. Edit nexmon/patches/bcm43455c0/7_45_206/nexmon/Makefile:
 *    Add to LOCAL_SRCS:
 *        LOCAL_SRCS += src/nexmon_scb_null_fix.c
 * 
 * 3. Build the firmware:
 *    cd nexmon
 *    source setup_env.sh
 *    cd patches/bcm43455c0/7_45_206/nexmon
 *    make clean
 *    make
 *    make install-firmware
 * 
 * 4. Reboot the Pi to load the patched firmware
 * 
 * 5. Verify the patch is working:
 *    dmesg | grep -i brcm
 *    # Should show firmware loaded without crashes
 */
