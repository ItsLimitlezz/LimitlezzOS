#ifdef LZ_TARGET_TDECK

#include <Arduino.h>
#include <string.h>
#include "services/mesh.h"
#include "ui/ui.h"

static bool g_mc_usb;
static char g_mc_line[512];
static uint16_t g_mc_len;

extern "C" bool lz_mcc_usb_active(void)
{
    return g_mc_usb;
}

extern "C" void lz_mcc_usb_set_active(bool on)
{
    if(on) {
        if(lz_mtc_active()) lz_mtc_set_active(false);
        if(lz_mtc_ble_enabled()) lz_mtc_ble_set_enabled(false);
    }
    g_mc_usb = on;
    g_mc_len = 0;
}

extern "C" int lz_mcc_usb_status(char *buf, int n)
{
    return snprintf(buf, (size_t)n, "MeshCore USB companion: %s%s",
                    g_mc_usb ? "MC0 attached" : "off",
                    g_mc_usb ? " (send MC0 <id> EXIT for console)" : "");
}

extern "C" int lz_mcc_usb_selftest(char *buf, int n)
{
    char svc[80];
    int written = lz_svc_mc_companion_selftest(svc, sizeof svc);
    (void)written;
    bool ok = strstr(svc, "PASS") != NULL;
    return snprintf(buf, (size_t)n, "MeshCore USB MC0 selftest: %s | %s",
                    ok ? "PASS" : "FAIL", svc);
}

static void mc_usb_handle_line(void)
{
    char out[1200];
    bool exit_mode = false;
    g_mc_line[g_mc_len] = 0;
    lz_svc_mc_companion_handle_line(g_mc_line, out, sizeof out, &exit_mode);
    if(out[0]) Serial.print(out);
    g_mc_len = 0;
    if(exit_mode) {
        g_mc_usb = false;
        Serial.print("\nlz> ");
    }
}

extern "C" void lz_mcc_usb_poll(void)
{
    while(Serial.available()) {
        lz_note_activity();
        char c = (char)Serial.read();
        if(c == '\r') continue;
        if(c == '\n') {
            mc_usb_handle_line();
        } else if(g_mc_len < sizeof(g_mc_line) - 1) {
            g_mc_line[g_mc_len++] = c;
        } else {
            static const char msg[] =
                "MC0 0 ERR code=bad_request retry=0 message=line%20too%20long\n";
            Serial.print(msg);
            g_mc_len = 0;
        }
    }
}

#endif /* LZ_TARGET_TDECK */
