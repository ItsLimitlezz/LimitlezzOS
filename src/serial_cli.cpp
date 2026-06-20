/**
 * Serial command console for the T-Deck (USB CDC).
 *
 * A line-oriented control + diagnostics shell: type `help` over the serial
 * monitor. Lets you drive and inspect the OS without touching the keyboard —
 * set the clock/timezone, flip networks, send a mesh message, list heard
 * nodes, watch radio airtime, and (Stage 2) inspect the Meshtastic/MeshCore
 * time-division schedule. Doubles as the hardware test harness.
 */
#ifdef LZ_TARGET_TDECK

#include <Arduino.h>
#include <string.h>
#include "services/emergency_guard.h"
#include "services/mesh.h"
#include "services/feedback.h"
#include "services/power_policy.h"
#include "services/ota_boot.h"
#include "services/wifi.h"
#include "ui/ui.h"

/* TDM / radio-profile hooks (implemented in backend_sx1262.cpp). Declared weak
 * so the console links even on builds without the scheduler. */
extern "C" void lz_backend_set_networks(bool mt, bool mc) __attribute__((weak));
extern "C" void lz_backend_set_airtime(int mode)          __attribute__((weak));
extern "C" int  lz_backend_tdm_info(char *buf, int n)     __attribute__((weak));
extern "C" void lz_backend_mc_tune(float freq, float bw, int sf, int cr, int sync) __attribute__((weak));
extern "C" int  lz_backend_mc_id(char *buf, int n)        __attribute__((weak));
extern "C" bool lz_backend_mc_advert_now(bool flood)      __attribute__((weak));
extern "C" bool lz_backend_mc_send_public(const char *text) __attribute__((weak));
extern "C" bool lz_backend_mc_dm(const char *name, const char *text) __attribute__((weak));
extern "C" int  lz_backend_mc_peers(char *buf, int n)     __attribute__((weak));
extern "C" int  lz_backend_mc_selftest(char *buf, int n)  __attribute__((weak));
extern "C" int  lz_mtc_selftest(char *buf, int n)         __attribute__((weak));
extern "C" int  lz_mtc_ble_status(char *buf, int n)       __attribute__((weak));
extern "C" int  lz_mtc_ble_selftest(char *buf, int n)     __attribute__((weak));
extern "C" void lz_touch_set_transform(int swap, int invx, int invy) __attribute__((weak));
extern "C" void lz_touch_set_debug(bool on)               __attribute__((weak));
extern "C" int  lz_touch_info(char *buf, int n)           __attribute__((weak));
extern "C" int  lz_mtpki_selftest(char *buf, int n)       __attribute__((weak));
extern "C" void lz_backend_set_rxlog(bool on)             __attribute__((weak));
extern "C" bool lz_backend_rxlog_enabled(void)            __attribute__((weak));

static char    g_line[160];
static uint8_t g_len;
static lz_emergency_guard_t g_emergency_guard;

static void cmd_feedback(char *args);   /* cmd_app's "notify test" echoes feedback status */
static void ota_print_decision(const lz_ota_boot_signals_t *s);  /* defined below cmd_ota */

static void prompt(void) { Serial.print("\nlz> "); }

static void cmd_help(void)
{
    Serial.println(F(
        "commands:\n"
        "  help                 this list\n"
        "  time                 show clock, date, timezone, DST\n"
        "  settime Y-M-D H:M    set the clock (local wall time)\n"
        "  tz                   list timezones\n"
        "  tz <name|abbr|idx>   set timezone (e.g. tz Eastern, tz PST, tz 3)\n"
        "  net                  show network enable state\n"
        "  net mt|mc on|off     enable/disable a network (drives TDM)\n"
        "  airtime [mt|balanced|mc]  show/set split preference\n"
        "  rf                   show radio profile + TDM schedule\n"
        "  rf mc <f> <bw> <sf> <cr> [sync]  tune MeshCore RF (e.g. rf mc 910.525 62.5 7 5)\n"
        "  mc                   show our MeshCore identity (pubkey)\n"
        "  mc advert [local]    broadcast a MeshCore self-advert (flood, or zero-hop)\n"
        "  mc send <text>       send a message on the MeshCore Public channel\n"
        "  mc dm <name> <text>  send a MeshCore direct message (encrypted)\n"
        "  mc peers             list heard MeshCore peers (dm targets)\n"
        "  mc test              build+verify our advert (proves nodes will accept it)\n"
        "  companion on|off     USB acts as a Meshtastic-app companion radio\n"
        "  companion ble on|off|test  BLE Meshtastic-app companion advertising\n"
        "  companion mc hello|status|nodes|threads|send|dm|test  MeshCore companion v0\n"
        "  companion mc usb on|off|status|test  USB speaks MeshCore MC0\n"
        "  companion test       loopback-verify the companion protocol\n"
        "  feedback status|test  feedback/app-notification diagnostics\n"
        "  app notify test      request a test app notification\n"
        "  app catalog status|test|install-test  app catalog diagnostics\n"
        "  app catalog install <id> [package_path]\n"
        "  app package test|install <id> <path> <sha256> <bytes>\n"
        "  touch [cal|debug|S X Y]  touch: 'cal' runs on-screen calibration, 'debug' logs taps, 'S X Y' sets transform\n"
        "  feedback             show DND/priority feedback policy\n"
        "  emergency [arm|confirm|cancel]  diagnostic emergency trigger guard\n"
        "  dm status            show pending sent-DM delivery state\n"
        "  dm test|req <sc>|send <sc> <text>   PKI DM: self-test / request a node's key / send a DM\n"
        "  rxlog [on|off]       show/toggle inbound radio decode logging\n"
        "  nodes [test]         list heard nodes / test node DB schema\n"
        "  send <text>          broadcast text on the channel\n"
        "  stats                radio TX/RX + airtime utilization\n"
        "  ota [status|test]    cached OTA firmware manifest diagnostics\n"
        "  security [status|test|set <pin>|check <pin>|clear <pin>]\n"
        "  wifi [scan|on|off]   wifi status / control\n"
        "  settings [test]      persisted settings schema diagnostics\n"
        "  ota boot-policy|boot-test  OTA rollback policy diagnostics\n"
        "  sys                  battery, uptime, memory\n"
        "  power                battery warning policy and current action\n"
        "  id                   this node's identity\n"
        "  reboot               restart the device"));
}

static void cmd_time(void)
{
    char d[40], t[16];
    lz_fmt_date(d, sizeof d);
    lz_fmt_now(t, sizeof t);
    int off = lz_svc_tz();
    Serial.printf("%s  %s  %s (UTC%+d:%02d)%s  [%s]\n",
                  d, t, lz_svc_tz_abbrev(), off / 60, (off < 0 ? -off : off) % 60,
                  lz_svc_dst_active() ? " DST" : "",
                  lz_svc_time_synced() ? "synced" : "not set");
}

static void cmd_settime(char *args)
{
    int y, mo, d, h, mi;
    if(args && sscanf(args, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) == 5) {
        lz_svc_set_clock(y, mo, d, h, mi);
        Serial.println("[ok] clock set");
        cmd_time();
    } else {
        Serial.println("usage: settime 2026-06-13 01:40");
    }
}

static void cmd_tz(char *args)
{
    if(!args || !args[0]) {
        Serial.println("timezones:");
        for(int i = 0; i < lz_tz_count(); i++) Serial.printf("  %2d  %s\n", i, lz_tz_name(i));
        return;
    }
    int idx = -1;
    if(args[0] >= '0' && args[0] <= '9') idx = atoi(args);
    else idx = lz_tz_find(args);
    if(idx < 0 || idx >= lz_tz_count()) { Serial.println("[err] unknown timezone"); return; }
    S.settings.tz_idx = idx;
    lz_tz_apply(idx);
    lz_settings_save();
    lz_rebuild();
    Serial.printf("[ok] timezone = %s\n", lz_tz_name(idx));
    cmd_time();
}

static void apply_networks(void)
{
    if(lz_backend_set_networks) lz_backend_set_networks(S.net_mt, S.net_mc);
    lz_settings_save();
    lz_rebuild();
}

static void cmd_net(char *args)
{
    char which[8] = {0}, state[8] = {0};
    if(args && sscanf(args, "%7s %7s", which, state) == 2) {
        bool on = strcmp(state, "on") == 0;
        if(strcmp(which, "mt") == 0)      S.net_mt = on;
        else if(strcmp(which, "mc") == 0) {
            if(!LZ_MESHCORE_ENABLED) { Serial.println("[err] MeshCore is gated in this build"); return; }
            S.net_mc = on;
        }
        else { Serial.println("usage: net mt|mc on|off"); return; }
        apply_networks();
    }
    Serial.printf("networks: Meshtastic %s, MeshCore %s\n",
                  S.net_mt ? "on" : "off", S.net_mc ? "on" : "off");
}

static void cmd_airtime(char *args)
{
    if(args && args[0]) {
        int mode = -1;
        if(strcmp(args, "mt") == 0 || strcmp(args, "meshtastic") == 0 || strcmp(args, "mt-first") == 0)
            mode = LZ_AIRTIME_MT_FIRST;
        else if(strcmp(args, "balanced") == 0 || strcmp(args, "balance") == 0 || strcmp(args, "50") == 0)
            mode = LZ_AIRTIME_BALANCED;
        else if(strcmp(args, "mc") == 0 || strcmp(args, "meshcore") == 0 || strcmp(args, "mc-first") == 0)
            mode = LZ_AIRTIME_MC_FIRST;
        if(mode < 0) { Serial.println("usage: airtime [mt|balanced|mc]"); return; }
        S.settings.airtime = mode;
        if(lz_backend_set_airtime) lz_backend_set_airtime(mode);
        lz_settings_save();
        lz_rebuild();
        Serial.println("[ok] airtime updated");
    }
    int mt_pct, mc_pct;
    lz_airtime_split_pct(S.settings.airtime, &mt_pct, &mc_pct);
    Serial.printf("airtime: %s  MT %d%% / MC %d%%\n",
                  lz_airtime_mode_label(S.settings.airtime), mt_pct, mc_pct);
}

static void cmd_rf(char *args)
{
    /* `rf mc <freq> <bw> <sf> <cr> [sync]` live-tunes the MeshCore profile */
    if(args && args[0] && lz_backend_mc_tune) {
        char sub[8] = {0}; float freq = 0, bw = 0; int sf = 0, cr = 0;
        unsigned sync = 0; int got = sscanf(args, "%7s %f %f %d %d %x", sub, &freq, &bw, &sf, &cr, &sync);
        if(got >= 1 && strcmp(sub, "mc") == 0) {
            lz_backend_mc_tune(freq, bw, sf, cr, got >= 6 ? (int)sync : -1);
            Serial.println("[ok] MeshCore RF tuned");
        }
    }
    if(lz_backend_tdm_info) {
        char buf[420];
        lz_backend_tdm_info(buf, sizeof buf);
        Serial.println(buf);
    } else {
        Serial.println("[--] TDM scheduler not present in this build");
    }
}

static void cmd_mc(char *args)
{
    if(args && strncmp(args, "advert", 6) == 0) {
        bool flood = strstr(args, "local") == NULL;   /* "advert local" = zero-hop */
        if(lz_backend_mc_advert_now && lz_backend_mc_advert_now(flood))
            Serial.printf("[ok] MeshCore %s advert sent\n", flood ? "flood" : "zero-hop");
        else
            Serial.println("[err] advert not sent (radio off?)");
        return;
    }
    if(args && strcmp(args, "test") == 0) {
        if(lz_backend_mc_selftest) { char b[160]; lz_backend_mc_selftest(b, sizeof b); Serial.println(b); }
        else Serial.println("[--] not present");
        return;
    }
    if(args && strncmp(args, "send ", 5) == 0) {       /* mc send <text> -> Public channel */
        const char *text = args + 5;
        if(lz_backend_mc_send_public && lz_backend_mc_send_public(text))
            Serial.println("[ok] sent on MeshCore Public");
        else
            Serial.println("[err] not sent (MeshCore off / radio down)");
        return;
    }
    if(args && strncmp(args, "dm ", 3) == 0) {         /* mc dm <peer-name> <text> */
        char *p = args + 3; char *sp = strchr(p, ' ');
        if(!sp) { Serial.println("usage: mc dm <peer-name> <text>"); return; }
        *sp = 0; const char *text = sp + 1;
        if(lz_backend_mc_dm && lz_backend_mc_dm(p, text))
            Serial.printf("[ok] DM sent to %s\n", p);
        else
            Serial.println("[err] DM not sent (unknown peer? try 'mc peers')");
        return;
    }
    if(args && strcmp(args, "peers") == 0) {
        if(lz_backend_mc_peers) { char b[700]; lz_backend_mc_peers(b, sizeof b); Serial.print(b); }
        else Serial.println("[--] MeshCore peers not present");
        return;
    }
    if(lz_backend_mc_id) {
        char buf[100]; lz_backend_mc_id(buf, sizeof buf);
        Serial.println(buf);
        Serial.println("(use 'mc advert' to broadcast a self-advert now)");
    } else {
        Serial.println("[--] MeshCore not present in this build");
    }
}

static void cmd_dm(char *args)
{
    if(args && strcmp(args, "test") == 0) {
        if(lz_mtpki_selftest) { char b[160]; lz_mtpki_selftest(b, sizeof b); Serial.println(b); }
        else Serial.println("[--] PKI not present");
        return;
    }
    if(args && strcmp(args, "status") == 0) {
        char b[700];
        lz_svc_delivery_diag(b, sizeof b);
        Serial.print(b);
        return;
    }
    if(args && strncmp(args, "req ", 4) == 0) {           /* dm req <shortcode> */
        lz_node_rt *n = lz_svc_node_by_shortcode(args + 4);
        if(n) { lz_backend_request_nodeinfo(n->num); Serial.printf("[ok] requested NodeInfo from %s\n", n->name); }
        else Serial.println("[err] node not found");
        return;
    }
    if(args && strncmp(args, "send ", 5) == 0) {          /* dm send <shortcode> <text> */
        char *p = args + 5; char *sp = strchr(p, ' ');
        if(!sp) { Serial.println("usage: dm send <shortcode> <text>"); return; }
        *sp = 0; const char *text = sp + 1;
        lz_node_rt *n = lz_svc_node_by_shortcode(p);
        if(!n) { Serial.println("[err] node not found"); return; }
        lz_thread_rt *t = lz_svc_thread_for_node(n);
        uint8_t k[32];
        bool keyed = lz_svc_node_pubkey(n->num, k);
        if(t && lz_svc_send_text(t, text))
            Serial.printf("[ok] DM sent to %s (%s)\n", n->name, keyed ? "PKI" : "PSK fallback - no key yet");
        else Serial.println("[err] send failed");
        return;
    }
    Serial.println("usage: dm status | dm test | dm req <shortcode> | dm send <shortcode> <text>");
}

static void cmd_touch(char *args)
{
    if(args && strcmp(args, "cal") == 0) {
        S.cal_step = 0; lz_go(LZ_V_TOUCHCAL); lz_rebuild();
        Serial.println("[ok] calibration: tap the 3 green dots on the screen");
        return;
    }
    if(args && strcmp(args, "debug") == 0) {
        static bool on = false; on = !on;
        if(lz_touch_set_debug) lz_touch_set_debug(on);
        Serial.printf("[ok] touch debug %s (tap the screen to see coords)\n", on ? "on" : "off");
        return;
    }
    int sw, ix, iy;
    if(args && sscanf(args, "%d %d %d", &sw, &ix, &iy) == 3) {
        if(lz_touch_set_transform) lz_touch_set_transform(sw, ix, iy);
        Serial.println("[ok] touch transform set");
    }
    if(lz_touch_info) { char b[120]; lz_touch_info(b, sizeof b); Serial.println(b); }
    else Serial.println("[--] touch not present");
}

static void cmd_emergency(char *args)
{
    uint32_t now = millis();
    if(args && strcmp(args, "arm") == 0) {
        if(lz_emergency_guard_arm(&g_emergency_guard, now, LZ_EMERGENCY_HOLD_MS))
            Serial.println("[ok] emergency guard armed; run 'emergency confirm' within the window");
        else
            Serial.println("[err] emergency guard did not arm");
    } else if(args && strcmp(args, "confirm") == 0) {
        if(lz_emergency_guard_confirm(&g_emergency_guard, now))
            Serial.println("[ok] emergency guard would trigger beacon; SOS TX is not wired yet");
        else
            Serial.println("[err] emergency guard not armed or expired");
    } else if(args && strcmp(args, "cancel") == 0) {
        lz_emergency_guard_reset(&g_emergency_guard);
        Serial.println("[ok] emergency guard cancelled");
    }
    char b[220];
    lz_emergency_guard_diag(&g_emergency_guard, now, b, sizeof b);
    Serial.print(b);
}

static void cmd_companion(char *args)
{
    if(args && strncmp(args, "mc", 2) == 0 && (args[2] == 0 || args[2] == ' ')) {
        char *sub = args + 2;
        while(*sub == ' ') sub++;
        if(strncmp(sub, "usb", 3) == 0 && (sub[3] == 0 || sub[3] == ' ')) {
            char *state = sub + 3;
            while(*state == ' ') state++;
            if(!state[0] || strcmp(state, "status") == 0) {
                char b[140]; lz_mcc_usb_status(b, sizeof b); Serial.println(b);
                return;
            }
            if(strcmp(state, "test") == 0) {
                char b[180]; lz_mcc_usb_selftest(b, sizeof b); Serial.println(b);
                return;
            }
            if(strcmp(state, "on") == 0) {
                lz_mcc_usb_set_active(true);
                if(lz_mcc_usb_active()) {
                    Serial.println("[ok] MeshCore MC0 USB companion mode ON");
                    Serial.println("     send 'MC0 1 EXIT' to return to the text console");
                } else {
                    Serial.println("[err] MeshCore MC0 USB companion could not allocate line buffer");
                }
                return;
            }
            if(strcmp(state, "off") == 0) {
                lz_mcc_usb_set_active(false);
                Serial.println("[ok] MeshCore MC0 USB companion mode OFF");
                return;
            }
            Serial.println("usage: companion mc usb on|off|status|test");
            return;
        }
        if(!sub[0] || strcmp(sub, "status") == 0) {
            char b[220]; lz_svc_mc_companion_status(b, sizeof b); Serial.print(b);
            return;
        }
        if(strcmp(sub, "hello") == 0) {
            char b[220]; lz_svc_mc_companion_hello(b, sizeof b); Serial.print(b);
            return;
        }
        if(strcmp(sub, "nodes") == 0) {
            char b[760]; lz_svc_mc_companion_nodes(b, sizeof b); Serial.print(b);
            return;
        }
        if(strcmp(sub, "threads") == 0) {
            char b[760]; lz_svc_mc_companion_threads(b, sizeof b); Serial.print(b);
            return;
        }
        if(strcmp(sub, "test") == 0) {
            char h[220], st[220], nodes[220];
            char proto[120];
            lz_svc_mc_companion_hello(h, sizeof h);
            lz_svc_mc_companion_status(st, sizeof st);
            lz_svc_mc_companion_nodes(nodes, sizeof nodes);
            lz_svc_mc_companion_selftest(proto, sizeof proto);
            bool ok = strstr(h, "mccomp: hello") && strstr(st, "mccomp: status") &&
                      strstr(nodes, "mccomp-node:") && strstr(proto, "PASS");
            Serial.printf("MeshCore companion v0 selftest: %s\n", ok ? "PASS" : "FAIL");
            Serial.println(proto);
            return;
        }
        if(strncmp(sub, "send ", 5) == 0) {
            const char *text = sub + 5;
            if(lz_svc_mc_companion_send_public(text))
                Serial.println("[ok] mc companion public send queued");
            else
                Serial.println("[err] mc companion public send failed");
            return;
        }
        if(strncmp(sub, "dm ", 3) == 0) {
            char *p = sub + 3; char *sp = strchr(p, ' ');
            if(!sp) { Serial.println("usage: companion mc dm <peer-name> <text>"); return; }
            *sp = 0; const char *text = sp + 1;
            if(lz_svc_mc_companion_send_dm(p, text))
                Serial.printf("[ok] mc companion DM queued to %s\n", p);
            else
                Serial.println("[err] mc companion DM failed");
            return;
        }
        Serial.println("usage: companion mc hello|status|nodes|threads|send <text>|dm <peer> <text>|test|usb on|off|status|test");
        return;
    }
    if(args && strncmp(args, "ble", 3) == 0) {
        char state[8] = {0};
        if(sscanf(args, "ble %7s", state) == 1) {
            if(strcmp(state, "on") == 0)  { lz_mcc_usb_set_active(false); lz_mtc_ble_set_enabled(true); }
            if(strcmp(state, "off") == 0) lz_mtc_ble_set_enabled(false);
            if(strcmp(state, "test") == 0 && lz_mtc_ble_selftest) {
                char b[120]; lz_mtc_ble_selftest(b, sizeof b); Serial.println(b);
            }
        }
        if(lz_mtc_ble_status) { char b[240]; lz_mtc_ble_status(b, sizeof b); Serial.println(b); }
        else Serial.println("[--] BLE companion not present");
        return;
    }
    if(args && strcmp(args, "test") == 0) {
        if(lz_mtc_selftest) { char b[160]; lz_mtc_selftest(b, sizeof b); Serial.println(b); }
        if(lz_mtc_ble_selftest) { char b[120]; lz_mtc_ble_selftest(b, sizeof b); Serial.println(b); }
        if(lz_mtc_ble_status) { char b[240]; lz_mtc_ble_status(b, sizeof b); Serial.println(b); }
        else Serial.println("[--] not present");
        return;
    }
    if(args && strcmp(args, "on") == 0) {
        lz_mcc_usb_set_active(false);
        Serial.println("[ok] companion mode ON - USB now speaks the Meshtastic app protocol");
        Serial.println("     (text console returns after 'companion off' or reboot)");
        lz_mtc_set_active(true);
        return;
    }
    if(args && strcmp(args, "off") == 0) { lz_mtc_set_active(false); Serial.println("[ok] companion mode OFF"); return; }
    Serial.printf("USB companion mode: %s  (on|off|test)\n", lz_mtc_active() ? "ON" : "off");
    if(lz_mtc_ble_status) { char b[240]; lz_mtc_ble_status(b, sizeof b); Serial.println(b); }
}

static void cmd_app(char *args)
{
    if(args && strcmp(args, "notify test") == 0) {
        lz_svc_feedback_notify("serial-app", "App notification", "SDK notify smoke");
        Serial.println("[ok] app notification requested");
        cmd_feedback((char *)"status");
        return;
    }
    if(args && strcmp(args, "catalog test") == 0) {
        char b[180];
        lz_svc_app_catalog_selftest(b, sizeof b);
        Serial.print(b);
        return;
    }
    if(args && strcmp(args, "catalog install-test") == 0) {
        char b[180];
        lz_svc_app_catalog_install_selftest(b, sizeof b);
        Serial.print(b);
        return;
    }
    if(args && strncmp(args, "catalog install ", 16) == 0) {
        char id[24] = {0}, path[112] = {0};
        int parsed = sscanf(args + 16, "%23s %111s", id, path);
        if(parsed < 1) {
            Serial.println("usage: app catalog install <id> [package_path]");
            return;
        }
        lz_app_package_install_t r;
        memset(&r, 0, sizeof r);
        if(lz_svc_install_app_catalog_entry(id, parsed >= 2 ? path : NULL, &r)) {
            Serial.printf("[ok] app catalog install id=%s version=%s files=%u source=%s package=%lu extracted=%lu\n",
                          r.id, r.version, (unsigned)r.file_count,
                          r.fetched ? "download" : "local",
                          (unsigned long)r.package_bytes,
                          (unsigned long)r.extracted_bytes);
        } else {
            Serial.printf("[err] app catalog install: %s\n",
                          r.error[0] ? r.error : "install failed");
        }
        return;
    }
    if(args && strcmp(args, "package test") == 0) {
        char b[180];
        lz_svc_app_package_selftest(b, sizeof b);
        Serial.print(b);
        return;
    }
    if(args && strncmp(args, "package install ", 16) == 0) {
        char id[24] = {0}, path[80] = {0}, sha[65] = {0};
        unsigned long bytes = 0;
        if(sscanf(args + 16, "%23s %79s %64s %lu", id, path, sha, &bytes) != 4 ||
           bytes == 0 || bytes > LZ_APP_PACKAGE_MAX_BYTES) {
            Serial.println("usage: app package install <id> <path> <sha256> <bytes>");
            return;
        }
        lz_app_package_install_t r;
        memset(&r, 0, sizeof r);
        if(lz_svc_install_app_package(id, path, sha, (uint32_t)bytes, &r)) {
            Serial.printf("[ok] app package install id=%s version=%s files=%u package=%lu extracted=%lu\n",
                          r.id, r.version, (unsigned)r.file_count,
                          (unsigned long)r.package_bytes,
                          (unsigned long)r.extracted_bytes);
        } else {
            Serial.printf("[err] app package install: %s\n",
                          r.error[0] ? r.error : "install failed");
        }
        return;
    }
    if(!args || !args[0] || strcmp(args, "catalog") == 0 ||
       strcmp(args, "catalog status") == 0) {
        char b[180];
        lz_svc_app_catalog_diag(b, sizeof b);
        Serial.print(b);
        return;
    }
    Serial.println("usage: app notify test | app catalog status|test|install-test|install <id> [package_path] | app package test");
}

static void cmd_nodes(char *args)
{
    if(args && strcmp(args, "test") == 0) {
        char err[64];
        bool ok = lz_store_nodes_selftest(err, sizeof err);
        Serial.printf("Node DB schema selftest: %s version=%u",
                      ok ? "PASS" : "FAIL", (unsigned)LZ_NODE_DB_SCHEMA_VERSION);
        if(err[0]) Serial.printf(" %s", err);
        Serial.println();
        return;
    }
    const lz_node_rt *nodes;
    int n = lz_svc_nodes(&nodes);
    if(n == 0) { Serial.println("(no nodes heard yet)"); return; }
    Serial.printf("%d node(s):\n", n);
    char ago[12];
    for(int i = 0; i < n; i++) {
        const lz_node_rt *nd = &nodes[i];
        lz_fmt_ago(nd->last_heard, ago, sizeof ago);
        char extra[48] = {0};
        size_t ex = 0;
        if(nd->pos_flags & LZ_NODE_POS_VALID)
            ex += snprintf(extra + ex, sizeof extra - ex, " gps");
        if((nd->telem_flags & LZ_NODE_TEL_VOLT) && ex < sizeof extra)
            ex += snprintf(extra + ex, sizeof extra - ex, " %.2fV",
                           (double)nd->voltage_mv / 1000.0);
        if((nd->telem_flags & LZ_NODE_TEL_TEMP) && ex < sizeof extra)
            ex += snprintf(extra + ex, sizeof extra - ex, " %.1fC",
                           (double)nd->temp_c10 / 10.0);
        Serial.printf("  [%-4s] %-10s %-15s %s SNR %.1f %s%s%s%s\n",
                      nd->shortcode, nd->id, nd->name, nd->net == LZ_NET_MC ? "MC" : "MT",
                      nd->snr, ago, nd->has_key ? " key" : "",
                      nd->contact ? " *contact" : "", extra);
    }
}

static void cmd_send(char *args)
{
    if(!args || !args[0]) { Serial.println("usage: send <text>"); return; }
    lz_thread_rt *t = lz_svc_channel_thread();
    if(t && lz_svc_send_text(t, args)) Serial.println("[ok] sent on channel");
    else Serial.println("[err] send failed");
}

static void cmd_stats(void)
{
    lz_radio_stats_t st;
    lz_svc_radio_stats(&st);
    Serial.printf("radio: TX %u  RX %u  airtime util %.2f%%\n",
                  (unsigned)st.tx_count, (unsigned)st.rx_count, st.util_pct);
}

static void cmd_ota(char *args)
{
    if(args && strcmp(args, "boot-test") == 0) {
        char err[64];
        bool ok = lz_ota_boot_selftest(err, sizeof err);
        Serial.printf("OTA boot policy selftest: %s", ok ? "PASS" : "FAIL");
        if(err[0]) Serial.printf(" %s", err);
        Serial.println();
        return;
    }
    if(args && strncmp(args, "boot-policy", 11) == 0) {
        char *mode = args + 11;
        while(*mode == ' ') mode++;
        lz_ota_boot_signals_t s = {0};
        if(strcmp(mode, "pending") == 0) {
            s.pending_verify = true;
            s.boot_selftest_passed = true;
        } else if(strcmp(mode, "confirmed") == 0) {
            s.pending_verify = true;
            s.boot_selftest_passed = true;
            s.user_confirmed = true;
        } else if(strcmp(mode, "fault") == 0) {
            s.pending_verify = true;
            s.boot_selftest_passed = true;
            s.critical_fault = true;
        } else if(strcmp(mode, "selftest-fail") == 0) {
            s.pending_verify = true;
        }
        ota_print_decision(&s);
        return;
    }
    if(args && strcmp(args, "test") == 0) {
        char b[160];
        lz_svc_ota_manifest_selftest(b, sizeof b);
        Serial.println(b);
        return;
    }
    if(args && args[0] && strcmp(args, "status") != 0) {
        Serial.println("usage: ota [status|test|boot-policy ...|boot-test]");
        return;
    }

    lz_ota_manifest_t m;
    lz_svc_ota_manifest_status(&m);
    if(!m.found) {
        Serial.println("ota manifest: no cached manifest");
    } else if(!m.valid) {
        Serial.printf("ota manifest: invalid source=%s error=\"%s\"\n",
                      m.source, m.error);
    } else {
        Serial.printf("ota manifest: valid version=%s channel=%s board=%s size=%lu source=%s\n",
                      m.version, m.channel, m.board,
                      (unsigned long)m.size_bytes, m.source);
        Serial.printf("firmware: %s\n", m.firmware_url);
    }
    }
static void cmd_security(char *args)
{
    if(args && strcmp(args, "test") == 0) {
        char b[120];
        lz_svc_security_selftest(b, sizeof b);
        Serial.println(b);
        return;
    }
    if(args && strncmp(args, "set ", 4) == 0) {
        char err[64] = {0};
        if(lz_svc_security_set_pin(args + 4, err, sizeof err))
            Serial.println("[ok] device PIN verifier set (data encryption not enabled yet)");
        else
            Serial.printf("[err] %s\n", err[0] ? err : "PIN not set");
        return;
    }
    if(args && strncmp(args, "check ", 6) == 0) {
        Serial.println(lz_svc_security_check_pin(args + 6) ? "[ok] PIN accepted" : "[err] PIN rejected");
        return;
    }
    if(args && strncmp(args, "clear ", 6) == 0) {
        char err[64] = {0};
        if(lz_svc_security_clear_pin(args + 6, err, sizeof err))
            Serial.println("[ok] device PIN verifier cleared");
        else
            Serial.printf("[err] %s\n", err[0] ? err : "PIN not cleared");
        return;
    }
    if(args && args[0] && strcmp(args, "status") != 0) {
        Serial.println("usage: security [status|test|set <pin>|check <pin>|clear <pin>]");
        return;
    }

    lz_security_status_t st;
    lz_svc_security_status(&st);
    if(st.configured && st.valid) {
        Serial.printf("security: PIN set rounds=%lu salt=%s encrypted-store=not-enabled\n",
                      (unsigned long)st.rounds, st.salt);
    } else if(st.configured && !st.valid) {
        Serial.printf("security: invalid verifier error=\"%s\"\n", st.error);
    } else {
        Serial.printf("security: no device PIN set (%s); encrypted-store=not-enabled\n",
                      st.error[0] ? st.error : "not configured");
    }
}

static void print_wifi_text(const char *text)
{
    if(!text || !text[0]) { Serial.print("(none)"); return; }
    for(int i = 0; i < 32 && text[i]; i++) Serial.write((uint8_t)text[i]);
}

static void cmd_wifi(char *args)
{
    if(args && strcmp(args, "scan") == 0) { if(!lz_wifi_enabled()) lz_wifi_set_enabled(true); else lz_wifi_scan(); Serial.println("[ok] scanning"); return; }
    if(args && strcmp(args, "on") == 0)   { lz_wifi_set_enabled(true);  Serial.println("[ok] wifi on");  return; }
    if(args && strcmp(args, "off") == 0)  { lz_wifi_set_enabled(false); Serial.println("[ok] wifi off"); return; }
    static const char *S_[] = { "off", "idle", "scanning", "connecting", "connected", "failed" };
    int s = lz_wifi_status();
    const char *conn = lz_wifi_connected();
    const char *saved = lz_wifi_saved_ssid();
    const lz_wifi_net *nets; int nn = lz_wifi_results(&nets);
    Serial.print("wifi: ");
    Serial.print((s >= 0 && s <= 5) ? S_[s] : "?");
    if(conn) { Serial.print(" -> "); print_wifi_text(conn); }
    Serial.print("  saved=");
    print_wifi_text(saved);
    Serial.print("  auto-connect=");
    Serial.print(lz_wifi_autoconnect() ? "on" : "off");
    Serial.print("  cred=");
    print_wifi_text(lz_wifi_credential_store());
    Serial.print("  nets=");
    Serial.println(nn);
}

static void cmd_settings(char *args)
{
    if(args && strcmp(args, "test") == 0) {
        char err[64];
        bool ok = lz_store_settings_selftest(err, sizeof err);
        Serial.printf("Settings schema selftest: %s version=%d",
                      ok ? "PASS" : "FAIL", LZ_SETTINGS_SCHEMA_VERSION);
        if(err[0]) Serial.printf(" %s", err);
        Serial.println();
        return;
    }
    Serial.printf("settings: schema=%d mt=%s mc=%s airtime=%s bright=%d timeout=%d kb=%d tz=%s clock=%s developer=%s\n",
                  LZ_SETTINGS_SCHEMA_VERSION,
                  S.net_mt ? "on" : "off",
                  S.net_mc ? "on" : "off",
                  lz_airtime_mode_label(S.settings.airtime),
                  S.settings.bright,
                  S.settings.timeout,
                  S.settings.kb_light,
                  lz_svc_tz_abbrev(),
                  S.settings.clock24 ? "24h" : "12h",
                  S.settings.developer ? "on" : "off");
}

static void cmd_feedback(char *args)
{
    if(args && strcmp(args, "test") == 0) {
        char b[120];
        lz_svc_feedback_selftest(b, sizeof b);
        Serial.println(b);
        return;
    }
    /* default/status: notification-service diag + the DND/priority policy matrix */
    char b[760];
    lz_svc_feedback_diag(b, sizeof b);
    Serial.print(b);
    lz_feedback_policy_diag(b, sizeof b);
    Serial.print(b);
}

static void cmd_sys(void)
{
    lz_sysinfo_t si;
    lz_svc_sysinfo(&si);
    Serial.printf("battery: %d%% (%.2fV)%s  cpu %dMHz  ram %d/%dKB  up %us\n",
                  si.battery_pct, si.battery_v,
                  si.usb ? (si.charging ? " charging" : " USB") : "",
                  si.cpu_mhz, si.ram_used_kb, si.ram_total_kb, (unsigned)si.uptime_s);
}

static void cmd_power(void)
{
    lz_sysinfo_t si;
    lz_svc_sysinfo(&si);
    int mv = si.battery_v > 0.0f ? (int)(si.battery_v * 1000.0f + 0.5f) : 0;
    char b[360];
    lz_power_policy_diag(b, sizeof b, si.battery_pct, mv, si.charging, si.usb);
    Serial.print(b);
    }
static void ota_print_decision(const lz_ota_boot_signals_t *s)
{
    lz_ota_boot_decision_t d;
    lz_ota_boot_decide(s, &d);
    Serial.printf("ota boot policy: action=%s apps=%s updates=%s reason=\"%s\"\n",
                  lz_ota_boot_action_name(d.action),
                  d.allow_app_launch ? "allow" : "hold",
                  d.allow_new_update ? "allow" : "hold",
                  d.reason);
}

static void cmd_id(void)
{
    const lz_identity_t *id = lz_svc_identity();
    Serial.printf("identity: %s  \"%s\" (%s)  node !%08x\n",
                  id->id, id->long_name, id->short_name, (unsigned)id->num);
}

static void dispatch(char *line)
{
    while(*line == ' ') line++;
    if(!*line) return;
    char *args = strchr(line, ' ');
    if(args) { *args++ = 0; while(*args == ' ') args++; }
    else args = (char *)"";          /* no args: point at "" (never NULL) */

    if(!strcmp(line, "help") || !strcmp(line, "?")) cmd_help();
    else if(!strcmp(line, "time"))    cmd_time();
    else if(!strcmp(line, "settime")) cmd_settime(args);
    else if(!strcmp(line, "tz"))      cmd_tz(args);
    else if(!strcmp(line, "net"))     cmd_net(args);
    else if(!strcmp(line, "airtime")) cmd_airtime(args);
    else if(!strcmp(line, "rf"))      cmd_rf(args);
    else if(!strcmp(line, "mc"))      cmd_mc(args);
    else if(!strcmp(line, "companion")) cmd_companion(args);
    else if(!strcmp(line, "feedback")) cmd_feedback(args);
    else if(!strcmp(line, "app"))     cmd_app(args);
    else if(!strcmp(line, "touch"))   cmd_touch(args);
    else if(!strcmp(line, "emergency")) cmd_emergency(args);
    else if(!strcmp(line, "dm"))      cmd_dm(args);
    else if(!strcmp(line, "rxlog")) {
        if(!args || !args[0]) {
            if(lz_backend_rxlog_enabled)
                Serial.printf("rx logging: %s\n", lz_backend_rxlog_enabled() ? "on" : "off");
            else
                Serial.println("rx logging: unavailable");
        } else if(!strcmp(args, "on") || !strcmp(args, "off")) {
            bool on = !strcmp(args, "on");
            if(lz_backend_set_rxlog) lz_backend_set_rxlog(on);
            Serial.printf("[ok] rx logging %s\n", on ? "on" : "off");
        } else {
            Serial.println("usage: rxlog [on|off]");
        }
    }
    else if(!strcmp(line, "nodes"))   cmd_nodes(args);
    else if(!strcmp(line, "send"))    cmd_send(args);
    else if(!strcmp(line, "stats"))   cmd_stats();
    else if(!strcmp(line, "ota"))     cmd_ota(args);
    else if(!strcmp(line, "security")) cmd_security(args);
    else if(!strcmp(line, "wifi"))    cmd_wifi(args);
    else if(!strcmp(line, "settings")) cmd_settings(args);
    else if(!strcmp(line, "sys"))     cmd_sys();
    else if(!strcmp(line, "power"))   cmd_power();
    else if(!strcmp(line, "id"))      cmd_id();
    else if(!strcmp(line, "reboot"))  { Serial.println("[ok] rebooting"); delay(50); ESP.restart(); }
    else Serial.printf("[err] unknown command '%s' (try help)\n", line);
}

void lz_cli_begin(void)
{
    Serial.println("[ok] serial console ready - type 'help'");
    prompt();
}

void lz_cli_poll(void)
{
    while(Serial.available()) {
        lz_note_activity();                /* serial use keeps the device awake */
        char c = (char)Serial.read();
        if(c == '\r') continue;
        if(c == '\n') {
            Serial.println();
            g_line[g_len] = 0;
            dispatch(g_line);
            g_len = 0;
            prompt();
        } else if(c == 8 || c == 127) {        /* backspace */
            if(g_len) { g_len--; Serial.print("\b \b"); }
        } else if(g_len < sizeof g_line - 1) {
            g_line[g_len++] = c;
            Serial.print(c);                    /* echo */
        }
    }
}

#endif /* LZ_TARGET_TDECK */
