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
#include "services/mesh.h"
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

static char    g_line[160];
static uint8_t g_len;

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
        "  companion test       loopback-verify the companion protocol\n"
        "  touch [cal|debug|S X Y]  touch: 'cal' runs on-screen calibration, 'debug' logs taps, 'S X Y' sets transform\n"
        "  dm status            show pending sent-DM delivery state\n"
        "  dm test|req <sc>|send <sc> <text>   PKI DM: self-test / request a node's key / send a DM\n"
        "  nodes                list heard nodes\n"
        "  send <text>          broadcast text on the channel\n"
        "  stats                radio TX/RX + airtime utilization\n"
        "  wifi [scan|on|off]   wifi status / control\n"
        "  settings [test]      persisted settings schema diagnostics\n"
        "  sys                  battery, uptime, memory\n"
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

static void cmd_companion(char *args)
{
    if(args && strncmp(args, "ble", 3) == 0) {
        char state[8] = {0};
        if(sscanf(args, "ble %7s", state) == 1) {
            if(strcmp(state, "on") == 0)  lz_mtc_ble_set_enabled(true);
            if(strcmp(state, "off") == 0) lz_mtc_ble_set_enabled(false);
            if(strcmp(state, "test") == 0 && lz_mtc_ble_selftest) {
                char b[120]; lz_mtc_ble_selftest(b, sizeof b); Serial.println(b);
            }
        }
        if(lz_mtc_ble_status) { char b[180]; lz_mtc_ble_status(b, sizeof b); Serial.println(b); }
        else Serial.println("[--] BLE companion not present");
        return;
    }
    if(args && strcmp(args, "test") == 0) {
        if(lz_mtc_selftest) { char b[160]; lz_mtc_selftest(b, sizeof b); Serial.println(b); }
        if(lz_mtc_ble_selftest) { char b[120]; lz_mtc_ble_selftest(b, sizeof b); Serial.println(b); }
        if(lz_mtc_ble_status) { char b[180]; lz_mtc_ble_status(b, sizeof b); Serial.println(b); }
        else Serial.println("[--] not present");
        return;
    }
    if(args && strcmp(args, "on") == 0) {
        Serial.println("[ok] companion mode ON - USB now speaks the Meshtastic app protocol");
        Serial.println("     (text console returns after 'companion off' or reboot)");
        lz_mtc_set_active(true);
        return;
    }
    if(args && strcmp(args, "off") == 0) { lz_mtc_set_active(false); Serial.println("[ok] companion mode OFF"); return; }
    Serial.printf("USB companion mode: %s  (on|off|test)\n", lz_mtc_active() ? "ON" : "off");
    if(lz_mtc_ble_status) { char b[180]; lz_mtc_ble_status(b, sizeof b); Serial.println(b); }
}

static void cmd_nodes(void)
{
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

static void cmd_sys(void)
{
    lz_sysinfo_t si;
    lz_svc_sysinfo(&si);
    Serial.printf("battery: %d%% (%.2fV)%s  cpu %dMHz  ram %d/%dKB  up %us\n",
                  si.battery_pct, si.battery_v,
                  si.usb ? (si.charging ? " charging" : " USB") : "",
                  si.cpu_mhz, si.ram_used_kb, si.ram_total_kb, (unsigned)si.uptime_s);
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
    else if(!strcmp(line, "touch"))   cmd_touch(args);
    else if(!strcmp(line, "dm"))      cmd_dm(args);
    else if(!strcmp(line, "rxlog")) {
        bool on = !(args && strcmp(args, "off") == 0);
        if(lz_backend_set_rxlog) lz_backend_set_rxlog(on);
        Serial.printf("[ok] rx logging %s\n", on ? "on" : "off");
    }
    else if(!strcmp(line, "nodes"))   cmd_nodes();
    else if(!strcmp(line, "send"))    cmd_send(args);
    else if(!strcmp(line, "stats"))   cmd_stats();
    else if(!strcmp(line, "wifi"))    cmd_wifi(args);
    else if(!strcmp(line, "settings")) cmd_settings(args);
    else if(!strcmp(line, "sys"))     cmd_sys();
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
