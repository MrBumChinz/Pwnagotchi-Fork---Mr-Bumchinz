/**
 * channel_bandit.h - Thompson Sampling for Channel Selection
 *
 * UCB1-style channel hopping with Thompson Sampling
 * Prioritizes channels that have yielded handshakes
 */

#ifndef CHANNEL_BANDIT_H
#define CHANNEL_BANDIT_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define CB_MAX_CHANNELS 165  /* WiFi channels 1-14 (2.4GHz) + 36-165 (5GHz) */

typedef struct {
    float alpha;            /* Successes + prior */
    float beta;             /* Failures + prior */
    time_t last_visited;    /* For exploration bonus */
    int visits;             /* Total visits */
    int aps_seen;           /* APs seen on this channel */
    int handshakes;         /* Handshakes captured on this channel */
} cb_channel_t;

typedef struct {
    cb_channel_t channels[CB_MAX_CHANNELS + 1];  /* Index 1-14 (0 unused) */
    float exploration_bonus;                      /* Default 0.2 */
    int current_channel;
} cb_bandit_t;

/**
 * Initialize channel bandit
 */
void cb_init(cb_bandit_t *cb);

/**
 * Select next channel using Thompson Sampling
 * @param cb Channel bandit
 * @param visible_channels Array of channels with APs visible
 * @param num_visible Number of visible channels
 * @param ap_counts Number of APs per visible channel
 * @return Selected channel (1-14)
 */
int cb_select_channel(cb_bandit_t *cb, int *visible_channels, int num_visible, int *ap_counts);

/**
 * Observe outcome for current channel
 * @param cb Channel bandit
 * @param channel Channel number (1-14)
 * @param success True if handshake captured
 */
void cb_observe(cb_bandit_t *cb, int channel, bool success);

/**
 * Update channel metadata
 */
void cb_update_stats(cb_bandit_t *cb, int channel, int ap_count);

/**
 * Sample from Beta distribution (uses thompson.c)
 */
extern float ts_beta_sample(float alpha, float beta);

#endif /* CHANNEL_BANDIT_H */
