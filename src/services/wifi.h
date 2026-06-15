/**
 * Wi-Fi setup service — scan, connect (with password), status.
 *
 * Two backends behind one API, like the radio: wifi_sim.c (mock networks for
 * the desktop sim) and wifi_tdeck.cpp (Arduino WiFi on the ESP32-S3). The
 * App Store / OTA features (later) connect through this.
 */
#ifndef LZ_WIFI_H
#define LZ_WIFI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LZ_WIFI_OFF,         /* radio off                       */
    LZ_WIFI_IDLE,        /* on, not connected               */
    LZ_WIFI_SCANNING,    /* scan in progress                */
    LZ_WIFI_CONNECTING,  /* association/DHCP in progress    */
    LZ_WIFI_CONNECTED,   /* joined a network                */
    LZ_WIFI_FAILED,      /* last connect attempt failed     */
};

typedef struct {
    char ssid[33];
    int  rssi;           /* dBm */
    bool secure;         /* needs a password */
} lz_wifi_net;

void        lz_wifi_init(void);
void        lz_wifi_loop(void);                 /* pump the state machine */
bool        lz_wifi_enabled(void);
void        lz_wifi_set_enabled(bool on);       /* also kicks a scan when turning on */
void        lz_wifi_scan(void);
uint32_t    lz_wifi_scan_gen(void);             /* bumps when a scan completes (UI refresh) */
int         lz_wifi_results(const lz_wifi_net **out);
bool        lz_wifi_is_secure(const char *ssid);
void        lz_wifi_connect(const char *ssid, const char *pass);
void        lz_wifi_disconnect(void);           /* drop link, keep saved creds */
void        lz_wifi_forget(void);               /* clear saved creds + disconnect */
const char *lz_wifi_connected(void);            /* connected SSID, or NULL */
int         lz_wifi_status(void);
/* saved network + auto-connect */
const char *lz_wifi_saved_ssid(void);           /* remembered SSID, or NULL */
bool        lz_wifi_is_saved(const char *ssid);
bool        lz_wifi_autoconnect(void);
void        lz_wifi_set_autoconnect(bool on);

#ifdef __cplusplus
}
#endif

#endif
