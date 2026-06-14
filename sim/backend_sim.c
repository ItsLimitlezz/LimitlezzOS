/**
 * Simulated radio backend for the desktop simulator.
 *
 * This is the thin firmware<->backend contract layer (mesh.h). All the real
 * work — a virtual mesh of Meshtastic + MeshCore peers, REAL frame building
 * and decoding through the same path as the SX1262 driver, loopback ACKs,
 * auto-replies, ambient traffic and the TDM gate — lives in sim_radio.c.
 *
 * No real RF: outbound text is routed to the addressed virtual peer, ACKed and
 * sometimes answered, so the UI exercises the full receive pipeline (decode,
 * store append, unread bump, thread reorder, dedup, self-echo, delivery status)
 * exactly as it will on hardware.
 */
#include "../src/services/mesh.h"
#include "sim_radio.h"
#include <stdio.h>
#include <stdint.h>

void lz_backend_init(void) { sim_radio_init(); }

void lz_backend_loop(void) { sim_radio_loop(); }

bool lz_backend_send(lz_mt_packet_t *p) { return sim_radio_send(p); }

void lz_backend_stats(lz_radio_stats_t *out) { sim_radio_stats(out); }

void lz_backend_set_tx_power(int dbm) { (void)dbm; }   /* no real radio in the sim */

void lz_backend_set_networks(bool mt, bool mc) { sim_radio_set_networks(mt, mc); }

void lz_backend_request_nodeinfo(uint32_t to) { (void)to; }   /* no radio in the sim */

bool lz_backend_mc_advert_now(bool flood) { return sim_radio_mc_advert_now(flood); }

void lz_backend_mc_addr(char *buf, int n) { snprintf(buf, n, "MC-1ec77175"); }

/* MeshCore outbound API. sim_radio models inbound MeshCore (sim_inject_mc_*);
 * the outbound contract still needs these symbols defined or the native build
 * fails to link on macOS (weak externs in mesh_core.c don't resolve to NULL). */
bool lz_backend_mc_send_public(const char *text) { (void)text; return true; }
bool lz_backend_mc_dm(const char *name, const char *text) { (void)name; (void)text; return true; }
int  lz_backend_mc_peers(char *buf, int n) { snprintf(buf, n, "no MeshCore peers (sim)\n"); return 0; }

static bool g_sim_companion;
bool lz_mtc_active(void) { return g_sim_companion; }     /* no real bridge in the sim */
void lz_mtc_set_active(bool on) { g_sim_companion = on; }
