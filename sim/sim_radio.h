/**
 * sim_radio — a simulated radio environment for the LimitlezzOS desktop sim.
 *
 * This replaces the old "canned reply" stub with a small VIRTUAL MESH of
 * Meshtastic + MeshCore peers that talk to the firmware over the SAME path the
 * real SX1262 backend uses:
 *
 *   - Inbound Meshtastic traffic is built as REAL on-air frames with mtproto
 *     (mt_build_text / mt_data_encode + mt_crypt + mt_header_write) and then
 *     DECODED back through mt_header_read / mt_crypt / mt_data_decode, exactly
 *     like backend_sx1262.cpp::handle_rx_mt — so the real header framing,
 *     channel hash, AES-CTR crypto, protobuf decode, dedup and self-echo
 *     filtering are all exercised, not bypassed.
 *   - Inbound MeshCore adverts are built as REAL ADVERT frames and decoded with
 *     mc_parse / mc_advert_decode, like handle_rx_mc, before reaching
 *     lz_core_on_mc_node.
 *   - MeshCore Public-channel chat and DMs are delivered through the firmware's
 *     MeshCore ingress hook(s) with realistic content (this branch's MeshCore
 *     codec only decodes ADVERTs on-air; group/DM crypto lands in a later
 *     stage, so chat is injected at the hook boundary the backend would call).
 *
 * Outbound: when the firmware calls lz_backend_send (Meshtastic) the sim
 * routes the plaintext to the addressed virtual peer, generates the ROUTING
 * ACK (so sent-DM status goes DELIVERED) and sometimes auto-replies — so a full
 * two-way conversation can happen without hardware.
 *
 * TDM: we track which networks are enabled (lz_backend_set_networks) and only
 * deliver a network's inbound traffic while that network is "tuned in", so the
 * message-flow effect of the split airtime is visible in the sim. (Real RF slot
 * timing is hardware-only; this models the *effect*, not the microsecond TDM.)
 *
 * Everything here is sim-only (sim/). The firmware in src/ is untouched.
 */
#ifndef LZ_SIM_RADIO_H
#define LZ_SIM_RADIO_H

#include <stdint.h>
#include <stdbool.h>
#include "../src/services/mesh.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- lifecycle (called from backend_sim.c) ---- */
void sim_radio_init(void);
void sim_radio_loop(void);                 /* periodic traffic pump + deferred replies */
bool sim_radio_send(lz_mt_packet_t *p);    /* firmware -> air: route, ACK, maybe reply */
void sim_radio_set_networks(bool mt, bool mc);
void sim_radio_stats(lz_radio_stats_t *out);
bool sim_radio_mc_advert_now(bool flood);  /* our self-advert (sim: pretend sent) */

/* ---- on-demand injection (sim keyboard controls + scenario) ---- */
/* Each returns true if the event was delivered (i.e. that network is tuned in). */
bool sim_inject_mc_dm_from_limitlezz(const char *text);  /* MeshCore DM -> us */
bool sim_inject_mc_public(const char *who, const char *text); /* MeshCore Public chat */
bool sim_inject_mc_advert(void);             /* a fresh MeshCore node appears (ADVERT) */
bool sim_inject_mt_channel_text(const char *text);  /* Meshtastic LongFast broadcast */
bool sim_inject_mt_dm_to_us(const char *peer_name, const char *text);
bool sim_inject_mt_nodeinfo(void);           /* a Meshtastic node announces itself */
bool sim_inject_mt_position(void);           /* a Meshtastic node sends POSITION */
bool sim_inject_mt_telemetry(void);          /* a Meshtastic node sends TELEMETRY */
void sim_inject_burst(void);                 /* one of everything (stress the inbox) */

/* ambient traffic pump on/off (live sim toggle) */
void sim_set_auto_traffic(bool on);
bool sim_get_auto_traffic(void);

/* current TDM view, for a status line / scenario asserts */
bool sim_net_mt_tuned(void);
bool sim_net_mc_tuned(void);

/* ---- deterministic scenario / regression harness (program --simtest) ---- */
int  sim_scenario_run(void);   /* returns number of FAILED assertions (0 == pass) */

#ifdef __cplusplus
}
#endif

#endif
