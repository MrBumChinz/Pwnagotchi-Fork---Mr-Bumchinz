/**
 * channel_bandit.c - Thompson Sampling for Channel Selection
 *
 * Instead of static "most APs first" ordering, use Thompson Sampling
 * to prioritize channels that have historically yielded handshakes.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "channel_bandit.h"

void cb_init(cb_bandit_t *cb) {
    memset(cb, 0, sizeof(cb_bandit_t));
    cb->exploration_bonus = 0.2f;
    
    /* Initialize 2.4GHz channels (1-14) with neutral prior */
    for (int i = 1; i <= 14; i++) {
        cb->channels[i].alpha = 1.0f;
        cb->channels[i].beta = 1.0f;
    }
    /* Initialize 5GHz channels with neutral prior */
    {
        static const int ch5g[] = {36,40,44,48,52,56,60,64,
                                   100,104,108,112,116,120,124,128,132,136,140,144,
                                   149,153,157,161,165};
        for (int i = 0; i < (int)(sizeof(ch5g)/sizeof(ch5g[0])); i++) {
            cb->channels[ch5g[i]].alpha = 1.0f;
            cb->channels[ch5g[i]].beta = 1.0f;
        }
    }
}

int cb_select_channel(cb_bandit_t *cb, int *visible_channels, int num_visible, int *ap_counts) {
    if (num_visible == 0) return 0;
    if (num_visible == 1) return visible_channels[0];

    time_t now = time(NULL);
    int best_ch = visible_channels[0];
    float best_score = -1.0f;

    for (int i = 0; i < num_visible; i++) {
        int ch = visible_channels[i];
        if (ch < 1 || ch > CB_MAX_CHANNELS) continue;

        cb_channel_t *c = &cb->channels[ch];
        
        /* Thompson sample for this channel */
        float success_prob = ts_beta_sample(c->alpha, c->beta);
        
        /* AP density bonus - more APs = more targets */
        float ap_factor = 1.0f + (0.1f * (float)ap_counts[i]);
        
        /* Exploration bonus for channels not visited recently */
        float explore = 0.0f;
        if (c->last_visited > 0) {
            float hours_since = (float)(now - c->last_visited) / 3600.0f;
            explore = cb->exploration_bonus * fminf(hours_since, 2.0f) / 2.0f;
        } else {
            explore = cb->exploration_bonus;  /* Never visited = explore! */
        }
        
        /* Total score */
        float score = (success_prob + explore) * ap_factor;
        
        if (score > best_score) {
            best_score = score;
            best_ch = ch;
        }
    }

    return best_ch;
}

void cb_observe(cb_bandit_t *cb, int channel, bool success) {
    if (channel < 1 || channel > CB_MAX_CHANNELS) return;
    
    cb_channel_t *c = &cb->channels[channel];
    
    if (success) {
        c->alpha += 1.0f;
        c->handshakes++;
        fprintf(stderr, "[channel_bandit] ch%d: handshake! (alpha=%.1f)\n", channel, c->alpha);
    } else {
        /* Small beta increment for visit without handshake */
        c->beta += 0.2f;
    }
    
    c->visits++;
    c->last_visited = time(NULL);
}

void cb_update_stats(cb_bandit_t *cb, int channel, int ap_count) {
    if (channel < 1 || channel > CB_MAX_CHANNELS) return;
    
    cb->channels[channel].aps_seen = ap_count;
    cb->channels[channel].last_visited = time(NULL);
    cb->current_channel = channel;
}
