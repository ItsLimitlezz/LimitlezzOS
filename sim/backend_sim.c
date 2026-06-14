/**
 * Simulated radio backend for the desktop simulator.
 *
 * No real RF: outbound text is "delivered" and, for the demo contacts, a
 * canned reply arrives a couple seconds later through the same lz_core_on_*
 * path the real SX1262 driver uses — so the UI exercises the full receive
 * pipeline (store append, unread bump, thread reorder, autoscroll) exactly
 * as it will on hardware.
 */
#include "../src/services/mesh.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>

extern uint32_t lz_tick_ms(void);

static lz_radio_stats_t g_stats = { 412, 1284, 3.4f };

/* pending auto-reply */
static bool     g_reply_armed;
static uint32_t g_reply_at;
static uint32_t g_reply_from;
static char     g_reply_text[80];

static const char *canned_reply(uint32_t from)
{
    switch(from) {
        case 0x7c3a91d0: return "copy that, moving now";
        case 0x9f21de33: return "73, catch you on the next pass";
        case 0xa1b2c3d4: return "[auto] ack - base online";
        default:         return "received, thanks";
    }
}

void lz_backend_init(void) {}

void lz_backend_loop(void)
{
    if(g_reply_armed && lz_tick_ms() >= g_reply_at) {
        g_reply_armed = false;
        lz_core_on_heard(g_reply_from, -6.5f);
        lz_core_on_text(g_reply_from, 0, g_reply_text, 2, -6.5f);
        g_stats.rx_count++;
    }
}

bool lz_backend_send(lz_mt_packet_t *p)
{
    g_stats.tx_count++;
    /* arm a believable reply from messageable Meshtastic contacts */
    if(p->to != 0xFFFFFFFFu && p->to >= 0x10000) {
        g_reply_armed = true;
        g_reply_at = lz_tick_ms() + 2200;
        g_reply_from = p->to;
        snprintf(g_reply_text, sizeof g_reply_text, "%s", canned_reply(p->to));
    }
    return true;
}

void lz_backend_stats(lz_radio_stats_t *out) { *out = g_stats; }
void lz_backend_set_tx_power(int dbm) { (void)dbm; }   /* no real radio in the sim */
void lz_backend_set_networks(bool mt, bool mc) { (void)mt; (void)mc; }  /* no TDM in the sim */
void lz_backend_request_nodeinfo(uint32_t to) { (void)to; }            /* no radio in the sim */
bool lz_backend_mc_advert_now(bool flood) { (void)flood; return true; }  /* sim: pretend sent */
void lz_backend_mc_addr(char *buf, int n) { snprintf(buf, n, "MC-1ec77175"); }
static bool g_sim_companion;
bool lz_mtc_active(void) { return g_sim_companion; }                     /* no real bridge in the sim */
void lz_mtc_set_active(bool on) { g_sim_companion = on; }
bool lz_mtc_any_active(void) { return g_sim_companion; }
void lz_mtc_ble_begin(void) {}
void lz_mtc_ble_poll(void) {}
bool lz_mtc_ble_enabled(void) { return false; }
bool lz_mtc_ble_connected(void) { return false; }
void lz_mtc_ble_set_enabled(bool on) { (void)on; }
int  lz_mtc_ble_status(char *buf, int n) { return snprintf(buf, n, "BLE companion: unavailable in simulator"); }
