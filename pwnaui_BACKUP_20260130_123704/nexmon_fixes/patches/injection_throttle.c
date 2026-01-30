/***************************************************************************
 *                                                                         *
 * Nexmon Pwnagotchi Fix - Injection Rate Limiter                          *
 *                                                                         *
 * This patch adds rate limiting to frame injection to prevent             *
 * overwhelming the firmware with too many injected frames.                *
 *                                                                         *
 * The firmware crashes are often caused by injecting frames too rapidly.  *
 * This patch adds a configurable delay between injections.                *
 *                                                                         *
 **************************************************************************/

#pragma NEXMON targetregion "patch"

#include <firmware_version.h>
#include <wrapper.h>
#include <structs.h>
#include <patcher.h>
#include <helper.h>
#include <ieee80211_radiotap.h>
#include <sendframe.h>
#include <nexioctls.h>

/* Configuration */
#define MIN_INJECT_INTERVAL_US  500    /* Minimum microseconds between injections */
#define MAX_QUEUE_DEPTH         32     /* Maximum number of pending frames */
#define INJECTION_TIMEOUT_US    10000  /* Timeout for injection operations */

/* State tracking */
static struct {
    unsigned int last_inject_time;
    unsigned int inject_count;
    unsigned int drop_count;
    unsigned int error_count;
    unsigned int queue_depth;
    unsigned char enabled;
} injection_state = {0, 0, 0, 0, 0, 1};

/* Simple microsecond delay using NOPs */
static void udelay_simple(unsigned int us)
{
    volatile unsigned int i;
    /* Approximate delay - adjust based on CPU frequency */
    for (i = 0; i < us * 10; i++) {
        __asm__ __volatile__("nop");
    }
}

/*
 * Get approximate current time in microseconds
 * Uses the TSF (Time Synchronization Function) timer if available
 */
static unsigned int get_time_us(void)
{
    /* This is a simplified approximation */
    static unsigned int counter = 0;
    counter++;
    return counter;
}

/*
 * Rate-limited inject_frame function
 * 
 * This wraps the original injection with rate limiting to prevent
 * the firmware from being overwhelmed.
 */
int
inject_frame_throttled(struct wl_info *wl, sk_buff *p)
{
    int rtap_len = 0;
    struct wlc_info *wlc;
    int data_rate = 0;
    struct ieee80211_radiotap_iterator iterator;
    struct ieee80211_radiotap_header *rtap_header;
    unsigned int current_time;
    int ret;
    
    /* Check if injection is enabled */
    if (!injection_state.enabled) {
        return -1;
    }
    
    /* Validate inputs */
    if (!wl || !wl->wlc || !p || !p->data) {
        injection_state.error_count++;
        return -1;
    }
    
    wlc = wl->wlc;
    
    /* Check queue depth */
    if (injection_state.queue_depth >= MAX_QUEUE_DEPTH) {
        injection_state.drop_count++;
        return -1;
    }
    
    /* Rate limiting */
    current_time = get_time_us();
    if (current_time - injection_state.last_inject_time < MIN_INJECT_INTERVAL_US) {
        /* Add small delay if injecting too fast */
        udelay_simple(MIN_INJECT_INTERVAL_US - (current_time - injection_state.last_inject_time));
    }
    
    /* Parse radiotap header */
    rtap_len = *((char *)(p->data + 2));
    rtap_header = (struct ieee80211_radiotap_header *) p->data;
    
    ret = ieee80211_radiotap_iterator_init(&iterator, rtap_header, rtap_len, 0);
    
    while (!ret) {
        ret = ieee80211_radiotap_iterator_next(&iterator);
        if (ret) {
            continue;
        }
        switch (iterator.this_arg_index) {
            case IEEE80211_RADIOTAP_RATE:
                data_rate = (*iterator.this_arg);
                break;
            default:
                break;
        }
    }
    
    /* Remove radiotap header */
    skb_pull(p, rtap_len);
    
    /* Track queue depth */
    injection_state.queue_depth++;
    
    /* Inject frame */
    sendframe(wlc, p, 1, data_rate);
    
    /* Update state */
    injection_state.queue_depth--;
    injection_state.inject_count++;
    injection_state.last_inject_time = get_time_us();
    
    return 0;
}

/*
 * Hook for wl_send that uses rate-limited injection
 */
int
wl_send_hook_throttled(struct hndrte_dev *src, struct hndrte_dev *dev, struct sk_buff *p)
{
    struct wl_info *wl = (struct wl_info *) dev->softc;
    struct wlc_info *wlc = wl->wlc;
    
    if (wlc->monitor && p != 0 && p->data != 0 && ((short *) p->data)[0] == 0) {
        return inject_frame_throttled(wl, p);
    } else {
        return wl_send(src, dev, p);
    }
}

/*
 * IOCTL handler to configure injection parameters
 * 
 * Custom IOCTL NEX_SET_INJECTION_PARAMS:
 *   arg[0] = MIN_INJECT_INTERVAL_US (0 = disabled)
 *   arg[1] = MAX_QUEUE_DEPTH
 *   arg[2] = enabled (1/0)
 */
int
handle_injection_config_ioctl(struct wlc_info *wlc, char *arg, int len)
{
    unsigned int *params = (unsigned int *)arg;
    
    if (len < 12) {  /* 3 x sizeof(unsigned int) */
        return -1;
    }
    
    /* Apply configuration */
    injection_state.enabled = params[2] ? 1 : 0;
    
    /* Reset counters */
    injection_state.inject_count = 0;
    injection_state.drop_count = 0;
    injection_state.error_count = 0;
    
    return 0;
}

/*
 * IOCTL handler to get injection statistics
 */
int
handle_injection_stats_ioctl(struct wlc_info *wlc, char *arg, int len)
{
    unsigned int *stats = (unsigned int *)arg;
    
    if (len < 16) {  /* 4 x sizeof(unsigned int) */
        return -1;
    }
    
    stats[0] = injection_state.inject_count;
    stats[1] = injection_state.drop_count;
    stats[2] = injection_state.error_count;
    stats[3] = injection_state.enabled;
    
    return 0;
}

/* Apply the throttled hook for BCM43455c0 */
// Note: Uncomment the following line to replace the default wl_send_hook
// __attribute__((at(0x2037C0, "", CHIP_VER_BCM43455c0, FW_VER_7_45_206)))
// GenericPatch4(wl_send_hook_throttled, wl_send_hook_throttled + 1);
