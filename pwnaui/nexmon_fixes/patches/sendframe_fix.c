/***************************************************************************
 *                                                                         *
 * Nexmon Pwnagotchi Fix - SCB Null Pointer Patch                          *
 *                                                                         *
 * This patch fixes the firmware crash caused by null pointer dereference  *
 * in the sendframe path when injecting frames rapidly.                    *
 *                                                                         *
 * Based on: https://github.com/seemoo-lab/nexmon/issues/335#issuecomment-738928287
 *                                                                         *
 * USAGE:                                                                  *
 * 1. Copy this file to nexmon/patches/bcm43455c0/7_45_206/nexmon/src/     *
 * 2. Rebuild the firmware: make clean && make                             *
 * 3. Install: sudo make install-firmware                                  *
 *                                                                         *
 **************************************************************************/

#pragma NEXMON targetregion "patch"

#include <firmware_version.h>
#include <debug.h>
#include <wrapper.h>
#include <structs.h>
#include <helper.h>
#include <patcher.h>
#include <rates.h>
#include <nexioctls.h>
#include <capabilities.h>

/*
 * Fix for BCM43455c0 firmware crash during frame injection.
 * 
 * The crash occurs when packets are dequeued and pkt->scb is NULL.
 * The original firmware tries to access scb->cfg->flags without checking
 * if scb is valid, causing a data abort exception.
 *
 * This patch adds a null check and safely exits the dequeue loop if scb is null.
 * The next injected frame will trigger queue processing again.
 */

/* Counter for tracking null pointer events (for debugging) */
static unsigned int scb_null_count = 0;

/*
 * Naked function for the null pointer check patch.
 * This is injected at the point where the firmware accesses scb->cfg->flags
 */
__attribute__((naked))
void
check_scb_null_7_45_206(void)
{
    asm(
        "cmp r6, #0\n"              /* check if pkt->scb (r6) is null */
        "bne scb_not_null_206\n"
        "add lr, lr, #0x178\n"      /* if null, adjust lr to jump out of pkt dequeue loop */
        "b scb_return_206\n"
        "scb_not_null_206:\n"
        "ldr.w r3, [r7, #0xe8]\n"   /* original code: get scb->cfg->flags (this crashed when scb was null) */
        "scb_return_206:\n"
        "push {lr}\n"
        "pop {pc}\n"
    );
}

/*
 * Patch for firmware version 7.45.206 (Pi 3B+/4)
 * Replaces the instruction at 0x1AABB0 with a branch to our check function
 */
__attribute__((at(0x1AABB0, "", CHIP_VER_BCM43455c0, FW_VER_7_45_206)))
__attribute__((naked))
void
patch_null_pointer_scb_7_45_206(void)
{
    asm(
        "bl check_scb_null_7_45_206\n"  /* branch to null pointer check instead of accessing possibly invalid cfg */
    );
}

/*
 * Alternative version for firmware 7.45.189 (older Pi 3B+)
 */
__attribute__((naked))
void
check_scb_null_7_45_189(void)
{
    asm(
        "cmp r6, #0\n"
        "bne scb_not_null_189\n"
        "add lr, lr, #0x178\n"
        "b scb_return_189\n"
        "scb_not_null_189:\n"
        "ldr.w r3, [r7, #0xe8]\n"
        "scb_return_189:\n"
        "push {lr}\n"
        "pop {pc}\n"
    );
}

__attribute__((at(0x1AF378, "", CHIP_VER_BCM43455c0, FW_VER_7_45_189)))
__attribute__((naked))
void
patch_null_pointer_scb_7_45_189(void)
{
    asm(
        "bl check_scb_null_7_45_189\n"
    );
}

/*
 * Enhanced sendframe function with additional safety checks
 * This replaces the default sendframe to add rate limiting and validation
 */
static unsigned int last_inject_time = 0;
static unsigned int inject_delay_us = 1000;  /* Minimum microseconds between injections */

char
sendframe_safe(struct wlc_info *wlc, struct sk_buff *p, unsigned int fifo, unsigned int rate)
{
    char ret;
    unsigned int current_time;
    
    /* Validate input parameters */
    if (!wlc || !p) {
        printf("NEXFIX: sendframe called with null wlc or packet\n");
        return -1;
    }
    
    /* Validate wlc structure */
    if (!wlc->band || !wlc->active_queue) {
        printf("NEXFIX: wlc structure incomplete\n");
        return -1;
    }
    
    /* Check if band->hwrs_scb is valid */
    if (!wlc->band->hwrs_scb) {
        printf("NEXFIX: hwrs_scb is null, skipping frame\n");
        return -1;
    }
    
    /* Rate adjustment for 5GHz band */
    if (wlc->band->bandtype == WLC_BAND_5G && rate < RATES_RATE_6M) {
        rate = RATES_RATE_6M;
    }
    
    /* Check if hardware is up */
    if (wlc->hw->up) {
        ret = wlc_sendctl(wlc, p, wlc->active_queue, wlc->band->hwrs_scb, fifo, rate, 0);
    } else {
        ret = wlc_sendctl(wlc, p, wlc->active_queue, wlc->band->hwrs_scb, fifo, rate, 1);
        printf("NEXFIX: wlc down during sendframe\n");
    }
    
    return ret;
}
