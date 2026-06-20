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
#include <string.h>

void lz_backend_init(void) { sim_radio_init(); }

void lz_backend_loop(void) { sim_radio_loop(); }

bool lz_backend_send(lz_mt_packet_t *p) { return sim_radio_send(p); }

void lz_backend_stats(lz_radio_stats_t *out) { sim_radio_stats(out); }

void lz_backend_set_tx_power(int dbm) { (void)dbm; }   /* no real radio in the sim */

void lz_backend_set_networks(bool mt, bool mc) { sim_radio_set_networks(mt, mc); }

void lz_backend_set_airtime(int mode) { sim_radio_set_airtime(mode); }

void lz_backend_request_nodeinfo(uint32_t to) { (void)to; }   /* no radio in the sim */

bool lz_backend_mc_advert_now(bool flood) { return sim_radio_mc_advert_now(flood); }

void lz_backend_mc_addr(char *buf, int n) { snprintf(buf, n, "MC-1ec77175"); }

bool lz_backend_mc_pubkey(uint8_t out32[32])
{
    if(!out32) return false;
    for(int i = 0; i < 32; i++) out32[i] = (uint8_t)(0x10 + i);
    return true;
}

/* MeshCore outbound API. sim_radio models inbound MeshCore (sim_inject_mc_*);
 * the outbound contract still needs these symbols defined or the native build
 * fails to link on macOS (weak externs in mesh_core.c don't resolve to NULL). */
bool lz_backend_mc_send_public(const char *text) { (void)text; return true; }

static bool g_last_mc_dm;
static uint8_t g_last_mc_dm_key[32];
static char g_last_mc_dm_name[28];
static char g_last_mc_dm_text[160];

bool lz_backend_mc_dm_key(const uint8_t peer_pub[32], const char *name_hint, const char *text)
{
    if(!peer_pub || !text || !text[0]) return false;
    g_last_mc_dm = true;
    memcpy(g_last_mc_dm_key, peer_pub, 32);
    snprintf(g_last_mc_dm_name, sizeof g_last_mc_dm_name, "%s",
             name_hint ? name_hint : "");
    snprintf(g_last_mc_dm_text, sizeof g_last_mc_dm_text, "%s", text);
    return true;
}

bool lz_backend_mc_dm(const char *name, const char *text) { (void)name; (void)text; return false; }

void sim_backend_mc_clear_last_dm(void)
{
    g_last_mc_dm = false;
    memset(g_last_mc_dm_key, 0, sizeof g_last_mc_dm_key);
    g_last_mc_dm_name[0] = 0;
    g_last_mc_dm_text[0] = 0;
}

bool sim_backend_mc_last_dm(uint8_t out32[32], char *name, int name_cap,
                            char *text, int text_cap)
{
    if(!g_last_mc_dm) return false;
    if(out32) memcpy(out32, g_last_mc_dm_key, 32);
    if(name && name_cap > 0) snprintf(name, (size_t)name_cap, "%s", g_last_mc_dm_name);
    if(text && text_cap > 0) snprintf(text, (size_t)text_cap, "%s", g_last_mc_dm_text);
    return true;
}

int  lz_backend_mc_peers(char *buf, int n) { snprintf(buf, n, "no MeshCore peers (sim)\n"); return 0; }

static bool g_sim_companion;
bool lz_mtc_active(void) { return g_sim_companion; }     /* no real bridge in the sim */
void lz_mtc_set_active(bool on) { g_sim_companion = on; }
bool lz_mtc_any_active(void) { return g_sim_companion; }
void lz_mtc_ble_begin(void) {}
void lz_mtc_ble_poll(void) {}
bool lz_mtc_ble_enabled(void) { return false; }
bool lz_mtc_ble_connected(void) { return false; }
void lz_mtc_ble_set_enabled(bool on) { (void)on; }
int  lz_mtc_ble_status(char *buf, int n) { return snprintf(buf, n, "BLE companion: unavailable in simulator"); }
