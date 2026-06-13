/**
 * Wi-Fi backend for the T-Deck: thin wrapper over the Arduino WiFi stack.
 * Async scan + non-blocking connect, polled from lz_wifi_loop().
 */
#ifdef LZ_TARGET_TDECK

#include <Arduino.h>
#include <WiFi.h>
#include "services/wifi.h"
#include <string.h>

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

extern "C" void lz_wifi_init(void)
{
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
}

extern "C" bool lz_wifi_enabled(void) { return g_on; }

extern "C" void lz_wifi_scan(void)
{
    if(!g_on) return;
    WiFi.scanDelete();
    WiFi.scanNetworks(true /* async */);
    g_scan_running = true;
    g_status = LZ_WIFI_SCANNING;
}

extern "C" void lz_wifi_set_enabled(bool on)
{
    g_on = on;
    if(on) { WiFi.mode(WIFI_STA); g_status = LZ_WIFI_IDLE; lz_wifi_scan(); }
    else   { WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
             g_status = LZ_WIFI_OFF; g_connected[0] = 0; g_net_count = 0; }
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
    snprintf(g_pending_ssid, sizeof g_pending_ssid, "%s", ssid);
    snprintf(g_pending_pass, sizeof g_pending_pass, "%s", pass ? pass : "");
    WiFi.begin(g_pending_ssid, g_pending_pass);
    g_status = LZ_WIFI_CONNECTING;
    g_connect_deadline = millis() + 15000;
}

extern "C" void lz_wifi_forget(void)
{
    WiFi.disconnect(true);
    g_connected[0] = 0;
    if(g_on) g_status = LZ_WIFI_IDLE;
}

extern "C" const char *lz_wifi_connected(void) { return g_connected[0] ? g_connected : NULL; }
extern "C" int lz_wifi_status(void) { return g_status; }

extern "C" void lz_wifi_loop(void)
{
    if(!g_on) return;

    if(g_scan_running) {
        int n = WiFi.scanComplete();
        if(n >= 0) {
            g_scan_running = false;
            g_net_count = n > LZ_WIFI_MAX ? LZ_WIFI_MAX : n;
            for(int i = 0; i < g_net_count; i++) {
                snprintf(g_nets[i].ssid, sizeof g_nets[i].ssid, "%s", WiFi.SSID(i).c_str());
                g_nets[i].rssi = WiFi.RSSI(i);
                g_nets[i].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
            }
            WiFi.scanDelete();
            g_status = g_connected[0] ? LZ_WIFI_CONNECTED : LZ_WIFI_IDLE;
        }
    }

    if(g_status == LZ_WIFI_CONNECTING) {
        if(WiFi.status() == WL_CONNECTED) {
            snprintf(g_connected, sizeof g_connected, "%s", g_pending_ssid);
            g_status = LZ_WIFI_CONNECTED;
        } else if(millis() > g_connect_deadline) {
            WiFi.disconnect(true);
            g_status = LZ_WIFI_FAILED;
        }
    } else if(g_status == LZ_WIFI_CONNECTED && WiFi.status() != WL_CONNECTED) {
        g_connected[0] = 0;
        g_status = LZ_WIFI_IDLE;   /* dropped */
    }
}

#endif /* LZ_TARGET_TDECK */
