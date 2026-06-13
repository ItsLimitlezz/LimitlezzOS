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
extern "C" int  lz_backend_tdm_info(char *buf, int n)     __attribute__((weak));

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
        "  rf                   show radio profile + TDM schedule\n"
        "  nodes                list heard nodes\n"
        "  send <text>          broadcast text on the channel\n"
        "  stats                radio TX/RX + airtime utilization\n"
        "  wifi [scan|on|off]   wifi status / control\n"
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
    lz_rebuild();
    Serial.printf("[ok] timezone = %s\n", lz_tz_name(idx));
    cmd_time();
}

static void apply_networks(void)
{
    if(lz_backend_set_networks) lz_backend_set_networks(S.net_mt, S.net_mc);
    lz_rebuild();
}

static void cmd_net(char *args)
{
    char which[8] = {0}, state[8] = {0};
    if(args && sscanf(args, "%7s %7s", which, state) == 2) {
        bool on = strcmp(state, "on") == 0;
        if(strcmp(which, "mt") == 0)      S.net_mt = on;
        else if(strcmp(which, "mc") == 0) S.net_mc = on;
        else { Serial.println("usage: net mt|mc on|off"); return; }
        apply_networks();
    }
    Serial.printf("networks: Meshtastic %s, MeshCore %s\n",
                  S.net_mt ? "on" : "off", S.net_mc ? "on" : "off");
}

static void cmd_rf(void)
{
    if(lz_backend_tdm_info) {
        char buf[256];
        lz_backend_tdm_info(buf, sizeof buf);
        Serial.println(buf);
    } else {
        Serial.println("[--] TDM scheduler not present in this build");
    }
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
        Serial.printf("  %-10s %-16s %s  SNR %.1f  %s%s\n",
                      nd->id, nd->name, nd->net == LZ_NET_MC ? "MC" : "MT",
                      nd->snr, ago, nd->contact ? "  *contact" : "");
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

static void cmd_wifi(char *args)
{
    if(args && strcmp(args, "scan") == 0) { lz_wifi_set_enabled(true); lz_wifi_scan(); Serial.println("[ok] scanning"); return; }
    if(args && strcmp(args, "on") == 0)   { lz_wifi_set_enabled(true);  Serial.println("[ok] wifi on");  return; }
    if(args && strcmp(args, "off") == 0)  { lz_wifi_set_enabled(false); Serial.println("[ok] wifi off"); return; }
    static const char *S_[] = { "off", "idle", "scanning", "connecting", "connected", "failed" };
    int s = lz_wifi_status();
    const char *conn = lz_wifi_connected();
    const char *saved = lz_wifi_saved_ssid();
    Serial.printf("wifi: %s%s%s  saved=%s  auto-connect=%s\n",
                  (s >= 0 && s <= 5) ? S_[s] : "?",
                  conn ? " -> " : "", conn ? conn : "",
                  saved ? saved : "(none)", lz_wifi_autoconnect() ? "on" : "off");
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
    else if(!strcmp(line, "rf"))      cmd_rf();
    else if(!strcmp(line, "nodes"))   cmd_nodes();
    else if(!strcmp(line, "send"))    cmd_send(args);
    else if(!strcmp(line, "stats"))   cmd_stats();
    else if(!strcmp(line, "wifi"))    cmd_wifi(args);
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
