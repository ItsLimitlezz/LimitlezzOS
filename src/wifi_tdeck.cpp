/**
 * Wi-Fi backend for the T-Deck: thin wrapper over the Arduino WiFi stack.
 * Async scan + non-blocking connect, polled from lz_wifi_loop().
 *
 * Credentials: one remembered network (SSID + password) is persisted to the
 * store. When auto-connect is on, the device rejoins it on boot and after a
 * scan whenever it's in range; turning auto-connect off leaves the saved
 * password in place but never connects on its own.
 */
#ifdef LZ_TARGET_TDECK

#include <Arduino.h>
#include <WiFi.h>
#include "services/wifi.h"
#include <string.h>

extern "C" {
    void lz_store_save_wifi(const char *ssid, const char *pass, int autoconnect);
    bool lz_store_load_wifi(char *ssid, int sn, char *pass, int pn, int *autoconnect);
    /* BLE companion and WiFi share internal DMA RAM and can't both be resident;
     * turning WiFi on tears the BLE controller down first (frees that RAM). */
    bool lz_mtc_ble_enabled(void);
    void lz_mtc_ble_set_enabled(bool on);
}

#define LZ_WIFI_MAX 16

static bool        g_on;
static int         g_status = LZ_WIFI_OFF;
static lz_wifi_net g_nets[LZ_WIFI_MAX];
static int         g_net_count;
static char        g_connected[33];
static char        g_pending_ssid[33];
static char        g_pending_pass[64];
static uint32_t    g_connect_deadline;
static bool        g_scan_running;
static uint32_t    g_scan_gen;       /* bumps when a scan completes — UI refresh trigger */
static uint32_t    g_scan_due;       /* millis() to kick a deferred scan (0 = none) */
static int         g_scan_retries;   /* bounded retries when a scan errors (-2) */

static char        g_saved_ssid[33];
static char        g_saved_pass[64];
static bool        g_autoconnect = true;
static bool        g_loaded;

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

static void begin_connect(const char *ssid, const char *pass)
{
    snprintf(g_pending_ssid, sizeof g_pending_ssid, "%s", ssid);
    snprintf(g_pending_pass, sizeof g_pending_pass, "%s", pass ? pass : "");
    WiFi.begin(g_pending_ssid, g_pending_pass);
    g_status = LZ_WIFI_CONNECTING;
    g_connect_deadline = millis() + 15000;
}

extern "C" void lz_wifi_init(void)
{
    load_saved();
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    /* auto-connect on boot: come up and join the saved network. The scan is
     * kicked from lz_wifi_loop() once we're CONNECTED (scanning mid-association
     * returns 0), so the list is ready by the time Wi-Fi settings is opened. */
    if(g_autoconnect && g_saved_ssid[0]) {
        g_on = true;
        begin_connect(g_saved_ssid, g_saved_pass);
    }
}

extern "C" bool lz_wifi_enabled(void) { return g_on; }

extern "C" void lz_wifi_scan(void)
{
    if(!g_on) return;
    /* A scan started during association returns 0 results (the radio is busy
     * joining). Skip it — lz_wifi_loop() kicks a fresh scan the moment we reach
     * CONNECTED, so the list still fills in. */
    if(g_status == LZ_WIFI_CONNECTING) return;
    WiFi.scanDelete();
    WiFi.scanNetworks(true /* async */);
    g_scan_running = true;
    /* keep CONNECTED / FAILED visible while a background scan runs */
    if(g_status != LZ_WIFI_CONNECTED && g_status != LZ_WIFI_FAILED)
        g_status = LZ_WIFI_SCANNING;
}

extern "C" void lz_wifi_set_enabled(bool on)
{
    g_on = on;
    if(on) {
        /* free the BT controller's internal RAM first or esp_wifi_init NO_MEMs */
        if(lz_mtc_ble_enabled()) lz_mtc_ble_set_enabled(false);
        load_saved();
        WiFi.mode(WIFI_STA);
        /* rejoin the saved network if auto-connect is on, otherwise just scan */
        if(g_autoconnect && g_saved_ssid[0]) begin_connect(g_saved_ssid, g_saved_pass);
        else g_status = LZ_WIFI_IDLE;
        lz_wifi_scan();
    } else {
        WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
        g_status = LZ_WIFI_OFF; g_connected[0] = 0; g_net_count = 0;
        g_scan_running = false; g_scan_due = 0; g_scan_retries = 0;   /* cancel pending scans */
    }
}

extern "C" int lz_wifi_results(const lz_wifi_net **out)
{
    *out = g_nets;
    return g_net_count;
}

extern "C" bool lz_wifi_is_secure(const char *ssid)
{
    for(int i = 0; i < g_net_count; i++)
        if(strcmp(g_nets[i].ssid, ssid) == 0) return g_nets[i].secure;
    return true;
}

extern "C" void lz_wifi_connect(const char *ssid, const char *pass)
{
    load_saved();
    /* reconnecting a saved network with no password: reuse the stored one */
    if((!pass || pass[0] == 0) && g_saved_ssid[0] && strcmp(ssid, g_saved_ssid) == 0)
        pass = g_saved_pass;
    remember(ssid, pass);     /* store creds now so a drop can auto-rejoin */
    begin_connect(ssid, pass);
}

extern "C" void lz_wifi_disconnect(void)
{
    WiFi.disconnect(true);
    g_connected[0] = 0;
    if(g_on) g_status = LZ_WIFI_IDLE;
}

extern "C" void lz_wifi_forget(void)
{
    WiFi.disconnect(true);
    g_connected[0] = 0;
    g_saved_ssid[0] = 0;
    g_saved_pass[0] = 0;
    lz_store_save_wifi("", "", g_autoconnect ? 1 : 0);
    if(g_on) g_status = LZ_WIFI_IDLE;
}

extern "C" const char *lz_wifi_connected(void) { return g_connected[0] ? g_connected : NULL; }
extern "C" int lz_wifi_status(void) { return g_status; }
extern "C" uint32_t lz_wifi_scan_gen(void) { return g_scan_gen; }

extern "C" const char *lz_wifi_saved_ssid(void) { load_saved(); return g_saved_ssid[0] ? g_saved_ssid : NULL; }
extern "C" bool lz_wifi_is_saved(const char *ssid)
{
    load_saved();
    return ssid && g_saved_ssid[0] && strcmp(ssid, g_saved_ssid) == 0;
}
extern "C" bool lz_wifi_autoconnect(void) { load_saved(); return g_autoconnect; }
extern "C" void lz_wifi_set_autoconnect(bool on)
{
    load_saved();
    g_autoconnect = on;
    lz_store_save_wifi(g_saved_ssid, g_saved_pass, on ? 1 : 0);
}

/* Scanning right at WL_CONNECTED returns WIFI_SCAN_FAILED — the radio needs a
 * moment after association. Defer the post-connect scan a couple seconds. */
static void scan_soon(void) { g_scan_due = millis() + 2000; if(!g_scan_due) g_scan_due = 1; }

extern "C" void lz_wifi_loop(void)
{
    if(!g_on) return;

    if(g_scan_due && (int32_t)(millis() - g_scan_due) >= 0) {
        g_scan_due = 0;
        lz_wifi_scan();
    }

    if(g_scan_running) {
        int n = WiFi.scanComplete();
        if(n == WIFI_SCAN_FAILED) {          /* -2: scan errored (often too soon after join) */
            g_scan_running = false;
            if(g_scan_retries < 4) { g_scan_retries++; scan_soon(); }   /* retry shortly */
        } else if(n >= 0) {
            g_scan_running = false;
            g_scan_retries = 0;
            g_scan_gen++;        /* results are ready — let the WiFi screen refresh */
            g_net_count = n > LZ_WIFI_MAX ? LZ_WIFI_MAX : n;
            bool saw_saved = false;
            for(int i = 0; i < g_net_count; i++) {
                snprintf(g_nets[i].ssid, sizeof g_nets[i].ssid, "%s", WiFi.SSID(i).c_str());
                g_nets[i].rssi = WiFi.RSSI(i);
                g_nets[i].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
                if(g_saved_ssid[0] && strcmp(g_nets[i].ssid, g_saved_ssid) == 0) saw_saved = true;
            }
            WiFi.scanDelete();
            if(g_status == LZ_WIFI_SCANNING)
                g_status = g_connected[0] ? LZ_WIFI_CONNECTED : LZ_WIFI_IDLE;
            /* auto-connect: if we're idle and the saved network just appeared, join it */
            if(g_autoconnect && saw_saved && !g_connected[0] &&
               g_status != LZ_WIFI_CONNECTING)
                begin_connect(g_saved_ssid, g_saved_pass);
        }
    }

    if(g_status == LZ_WIFI_CONNECTING) {
        if(WiFi.status() == WL_CONNECTED) {
            snprintf(g_connected, sizeof g_connected, "%s", g_pending_ssid);
            remember(g_pending_ssid, g_pending_pass);   /* confirm saved creds */
            g_status = LZ_WIFI_CONNECTED;
            g_scan_retries = 0;
            scan_soon();       /* settle, then scan for the network list (mid-join fails) */
        } else if(millis() > g_connect_deadline) {
            WiFi.disconnect(true);
            g_status = LZ_WIFI_FAILED;
            g_scan_retries = 0;
            scan_soon();       /* connect failed — still show nearby networks to pick from */
        }
    } else if(g_status == LZ_WIFI_CONNECTED && WiFi.status() != WL_CONNECTED) {
        g_connected[0] = 0;
        g_status = LZ_WIFI_IDLE;   /* dropped */
        /* auto-rejoin: kick a scan; the handler above reconnects when in range */
        if(g_autoconnect && g_saved_ssid[0]) lz_wifi_scan();
    }
}

#endif /* LZ_TARGET_TDECK */
