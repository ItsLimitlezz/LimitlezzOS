/**
 * Simulated Wi-Fi backend for the desktop simulator: a fixed list of nearby
 * networks; connecting succeeds after a short delay (or fails for an obviously
 * wrong password) so the setup UI is fully exercisable without hardware.
 *
 * Mirrors the hardware backend's credential model: one remembered network
 * (SSID + password) persisted via the store, with an auto-connect toggle that
 * rejoins it when Wi-Fi comes on.
 */
#include "../src/services/wifi.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

extern uint32_t lz_tick_ms(void);
/* credential persistence (store.c) */
void lz_store_save_wifi(const char *ssid, const char *pass, int autoconnect);
bool lz_store_load_wifi(char *ssid, int sn, char *pass, int pn, int *autoconnect);

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

static char     g_saved_ssid[33];
static char     g_saved_pass[65];
static bool     g_autoconnect = true;
static bool     g_loaded;

static void load_saved(void)
{
    if(g_loaded) return;
    g_loaded = true;
    int ac = 1;
    if(lz_store_load_wifi(g_saved_ssid, sizeof g_saved_ssid,
                          g_saved_pass, sizeof g_saved_pass, &ac))
        g_autoconnect = ac != 0;
}

static void remember(const char *ssid, const char *pass)
{
    snprintf(g_saved_ssid, sizeof g_saved_ssid, "%s", ssid ? ssid : "");
    snprintf(g_saved_pass, sizeof g_saved_pass, "%s", pass ? pass : "");
    lz_store_save_wifi(g_saved_ssid, g_saved_pass, g_autoconnect ? 1 : 0);
}

void lz_wifi_init(void)
{
    load_saved();
    /* auto-connect on boot if we remember a network and the user left it on */
    if(g_autoconnect && g_saved_ssid[0]) {
        g_on = true;
        g_status = LZ_WIFI_CONNECTING;
        snprintf(g_pending, sizeof g_pending, "%s", g_saved_ssid);
        g_until = lz_tick_ms() + 1400;
    }
}

bool lz_wifi_enabled(void) { return g_on; }

void lz_wifi_set_enabled(bool on)
{
    g_on = on;
    if(on) {
        load_saved();
        /* if we remember a network, rejoin it (when auto-connect is on); else scan */
        if(g_autoconnect && g_saved_ssid[0]) {
            g_status = LZ_WIFI_CONNECTING;
            snprintf(g_pending, sizeof g_pending, "%s", g_saved_ssid);
            g_until = lz_tick_ms() + 1400;
        } else {
            g_status = LZ_WIFI_SCANNING; g_until = lz_tick_ms() + 600;
        }
    } else {
        g_status = LZ_WIFI_OFF; g_connected[0] = 0;
    }
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
    load_saved();
    /* a saved network can be reconnected with no password supplied */
    if((!pass || pass[0] == 0) && g_saved_ssid[0] && strcmp(ssid, g_saved_ssid) == 0)
        pass = g_saved_pass;
    snprintf(g_pending, sizeof g_pending, "%s", ssid);
    /* a secured network with an empty password obviously fails */
    bool ok = !(lz_wifi_is_secure(ssid) && (!pass || pass[0] == 0));
    if(ok) remember(ssid, pass);     /* store creds so we can rejoin later */
    g_status = LZ_WIFI_CONNECTING;
    g_until = lz_tick_ms() + 1300;
    if(!ok) g_pending[0] = 0;         /* mark failure intent */
}

void lz_wifi_disconnect(void)
{
    g_connected[0] = 0;
    if(g_on) g_status = LZ_WIFI_IDLE;
}

void lz_wifi_forget(void)
{
    g_connected[0] = 0;
    g_saved_ssid[0] = 0;
    g_saved_pass[0] = 0;
    lz_store_save_wifi("", "", g_autoconnect ? 1 : 0);
    if(g_on) g_status = LZ_WIFI_IDLE;
}

const char *lz_wifi_connected(void) { return g_connected[0] ? g_connected : NULL; }
int lz_wifi_status(void) { return g_status; }
uint32_t lz_wifi_scan_gen(void) { return 0; }   /* sim networks are static; no async refresh */

const char *lz_wifi_saved_ssid(void) { load_saved(); return g_saved_ssid[0] ? g_saved_ssid : NULL; }
bool lz_wifi_is_saved(const char *ssid)
{
    load_saved();
    return ssid && g_saved_ssid[0] && strcmp(ssid, g_saved_ssid) == 0;
}
bool lz_wifi_autoconnect(void) { load_saved(); return g_autoconnect; }
const char *lz_wifi_credential_store(void) { return "file"; }
void lz_wifi_set_autoconnect(bool on)
{
    load_saved();
    g_autoconnect = on;
    lz_store_save_wifi(g_saved_ssid, g_saved_pass, on ? 1 : 0);
}

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

/* Secret-store stubs for the desktop simulator. On the T-Deck these live in
 * store_secret_tdeck.cpp (ESP32 NVS); store.c declares them __attribute__((weak))
 * and falls back to the file-backed wifi.cfg when they return false / are absent.
 * Linux resolves an undefined weak symbol to NULL, but macOS does not — it fails
 * to link — so define real no-op stubs here to keep the native build file-backed. */
bool lz_store_secret_save_wifi(const char *ssid, const char *pass, int autoconnect)
{ (void)ssid; (void)pass; (void)autoconnect; return false; }
bool lz_store_secret_load_wifi(char *ssid, int sn, char *pass, int pn, int *autoconnect)
{ (void)ssid; (void)sn; (void)pass; (void)pn; (void)autoconnect; return false; }
