/* brain_attacks.h — Attack functions extracted from brain.c (Sprint 7 #21) */
#ifndef BRAIN_ATTACKS_H
#define BRAIN_ATTACKS_H

#include "brain.h"
#include "health_monitor.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <unistd.h>     /* useconds_t */

/* Monitor interface for raw frame injection */
#ifndef RAW_INJECT_IFACE
#define RAW_INJECT_IFACE "wlan0mon"
#endif

/* Shared state for raw injection */
extern int g_raw_sock;
extern health_state_t *g_health_state;
extern const uint8_t BCAST_MAC[6];

/* Sequence counters */
uint16_t next_seq_ap(void);
uint16_t next_seq_client(void);
uint16_t next_seq_probe(void);

/* Jitter helper */
useconds_t jitter_usleep(useconds_t base_us);

/* Raw injection */
int attack_raw_inject_open(void);
int attack_raw_send(int sock, const uint8_t *frame, size_t len);

/* Attack phase functions */
int attack_anon_reassoc(int sock, const bcap_ap_t *ap);
int attack_eapol_m1_malformed(int sock, const bcap_ap_t *ap, const bcap_sta_t *sta);
int attack_power_save_spoof(int sock, const bcap_ap_t *ap, const bcap_sta_t *sta);
int attack_disassoc_bidi(int sock, const bcap_ap_t *ap, const bcap_sta_t *sta);
int attack_csa_beacon(int sock, const bcap_ap_t *ap);
int attack_csa_action(int sock, const bcap_ap_t *ap);
int attack_deauth_broadcast(int sock, const bcap_ap_t *ap);
int attack_deauth_bidi(int sock, const bcap_ap_t *ap, const bcap_sta_t *sta);
int attack_probe_undirected(int sock);
int attack_probe_directed(int sock, const bcap_ap_t *ap);
int attack_auth_assoc_pmkid(int sock, const bcap_ap_t *ap);
int attack_rsn_downgrade(int sock, const bcap_ap_t *ap, const bcap_sta_t *sta);
int attack_rogue_m2(int sock, const bcap_ap_t *ap, const bcap_sta_t *sta);

/* Reason code macros (need REASON_POOL arrays) */
extern const uint8_t REASON_POOL_AP[];
extern const size_t REASON_POOL_AP_COUNT;
extern const uint8_t REASON_POOL_STA[];
extern const size_t REASON_POOL_STA_COUNT;
#define RANDOM_REASON_AP()  REASON_POOL_AP[rand() % REASON_POOL_AP_COUNT]
#define RANDOM_REASON_STA() REASON_POOL_STA[rand() % REASON_POOL_STA_COUNT]

#endif /* BRAIN_ATTACKS_H */
