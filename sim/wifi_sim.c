/**
 * Simulated Wi-Fi backend for the desktop simulator: a fixed list of nearby
 * networks; connecting succeeds after a short delay (or fails for an obviously
 * wrong password) so the setup UI is fully exercisable without hardware.
 */
#include "../src/services/wifi.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

extern uint32_t lz_tick_ms(void);

static const lz_wifi_net NETS[] = {
    { "Basecamp-2G",   -48, true  },
    { "TrailNet",      -61, true  },
    { "RangerStation", -67, true  },
    { "Festival Free", -72, false },
    { "Summit Lodge",  -80, true  },
};
#define NET_N ((int)(sizeof NETS / sizeof NETS[0]))

static bool     g_on;
static int      g_status = LZ_WIFI_OFF;
static char     g_connected[33];
static char     g_pending[33];
static unsigned g_until;          /* tick when a pending op completes */

void lz_wifi_init(void) {}

bool lz_wifi_enabled(void) { return g_on; }

void lz_wifi_set_enabled(bool on)
{
    g_on = on;
    if(on) { g_status = LZ_WIFI_SCANNING; g_until = lz_tick_ms() + 600; }
    else   { g_status = LZ_WIFI_OFF; g_connected[0] = 0; }
}

void lz_wifi_scan(void)
{
    if(!g_on) return;
    g_status = LZ_WIFI_SCANNING;
    g_until = lz_tick_ms() + 600;
}

int lz_wifi_results(const lz_wifi_net **out)
{
    *out = NETS;
    return (g_on && g_status != LZ_WIFI_SCANNING) ? NET_N : 0;
}

bool lz_wifi_is_secure(const char *ssid)
{
    for(int i = 0; i < NET_N; i++)
        if(strcmp(NETS[i].ssid, ssid) == 0) return NETS[i].secure;
    return true;
}

void lz_wifi_connect(const char *ssid, const char *pass)
{
    snprintf(g_pending, sizeof g_pending, "%s", ssid);
    /* a secured network with an empty password obviously fails */
    bool ok = !(lz_wifi_is_secure(ssid) && (!pass || pass[0] == 0));
    g_status = LZ_WIFI_CONNECTING;
    g_until = lz_tick_ms() + 1300;
    if(!ok) g_pending[0] = 0;     /* mark failure intent */
}

void lz_wifi_forget(void)
{
    g_connected[0] = 0;
    if(g_on) g_status = LZ_WIFI_IDLE;
}

const char *lz_wifi_connected(void) { return g_connected[0] ? g_connected : NULL; }
int lz_wifi_status(void) { return g_status; }

void lz_wifi_loop(void)
{
    if(g_until && lz_tick_ms() >= g_until) {
        g_until = 0;
        if(g_status == LZ_WIFI_SCANNING) {
            g_status = g_connected[0] ? LZ_WIFI_CONNECTED : LZ_WIFI_IDLE;
        } else if(g_status == LZ_WIFI_CONNECTING) {
            if(g_pending[0]) { snprintf(g_connected, sizeof g_connected, "%s", g_pending);
                               g_status = LZ_WIFI_CONNECTED; }
            else g_status = LZ_WIFI_FAILED;
        }
    }
}
