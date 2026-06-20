#include "data.h"
#include "theme.h"
#include <math.h>
#include <string.h>

const lz_app_t LZ_APPS[8] = {
    { "messages",   "Messages",   LZ_I_FORUM,      "5 chats - 2 nets",  165 },
    { "meshtastic", "Meshtastic", LZ_I_HUB,        "JESS - 7 nodes",    205 },
    { "meshcore",   "MeshCore",   LZ_I_LAN,        "Companion - 5",     72  },
    { "contacts",   "Contacts",   LZ_I_GROUP,      "9 nodes",           318 },
    { "appstore",   "App Store",  LZ_I_STOREFRONT, "Local & catalog",   280 },
    { "terminal",   "Terminal",   LZ_I_TERMINAL,   "115200 baud",       -1  },
    { "files",      "Files",      LZ_I_FOLDER,     "/sd",               242 },
    { "settings",   "Settings",   LZ_I_SETTINGS,   "Networks - Radio",  -1  },
};

const lz_thread_t LZ_THREADS[6] = {
    { "ava",     "Ava Reyes",      LZ_NET_MT, "!7c3a91d0", "see you at the ridge in 20",        "2m",  2, "2 hops" },
    { "dmitri",  "Dmitri K",       LZ_NET_MC, "MC-4f8e",   "sending coords now",                "1h",  1, "3 hops" },
    { "base",    "Base-01",        LZ_NET_MT, "!a1b2c3d4", "copy, staying on LongFast - 73",    "14m", 0, "direct" },
    { "ridge",   "Ridge Repeater", LZ_NET_MC, "MC-9a3f",   "[auto] battery 78% - uplink ok",    "1h",  0, "direct" },
    { "sam",     "Sam OK1QRP",     LZ_NET_MT, "!9f21de33", "antenna swr looks good now",        "3h",  0, "1 hop"  },
    { "weather", "Weather-Sensor", LZ_NET_MC, "MC-sens",   "[telemetry] 2.4 C - 1014 hPa",      "4h",  0, "direct" },
};

const lz_msg_t LZ_MSGS_AVA[5] = {
    { false, "heading up the summit trail now" },
    { true,  "copy, I'm at the basecamp junction" },
    { false, "nice - watch the loose rock past the saddle" },
    { true,  "will do. radio check?" },
    { false, "5 by 9, -7 SNR. see you at the ridge in 20" },
};
const lz_msg_t LZ_MSGS_DMITRI[4] = {
    { false, "trailhead is clear, nobody around" },
    { true,  "good. did the repeater come back up?" },
    { false, "yes - ridge repeater is solid now" },
    { false, "sending coords now" },
};

const lz_chan_t LZ_CHANS[4] = {
    { "longfast",  "LongFast",  LZ_NET_MT, LZ_I_TAG,      "Primary - 7 nodes",    "B01: anyone copy on the ridge?", "8m"  },
    { "emergency", "Emergency", LZ_NET_MT, LZ_I_CAMPAIGN, "Encrypted - 12 nodes", "- channel quiet -",              "2h"  },
    { "basecamp",  "Base Camp", LZ_NET_MC, LZ_I_FORUM,    "Room - 6 members",     "Ava: heading out, radio on",     "5m"  },
    { "trailnet",  "Trail Net", LZ_NET_MC, LZ_I_FORUM,    "Room - 9 members",     "Dmitri: trailhead clear",        "22m" },
};

const lz_node_t LZ_NODES[9] = {
    { "Base-01",        "B01", LZ_NET_MT, "!a1b2c3d4", "Router",   9.5f,   92,  "+9.5",  "now", "T-Beam",      "0.0 km"  },
    { "Summit Relay",   "SMT", LZ_NET_MT, "!5e6f7a8b", "Router",   2.1f,   100, "+2.1",  "1m",  "RAK4631",     "4.2 km"  },
    { "Ava Reyes",      "AVA", LZ_NET_MT, "!7c3a91d0", "Client",   -7.2f,  68,  "-7.2",  "2m",  "T-Deck",      "1.8 km"  },
    { "Sam OK1QRP",     "SAM", LZ_NET_MT, "!9f21de33", "Client",   -11.8f, 45,  "-11.8", "9m",  "Heltec V3",   "6.7 km"  },
    { "River Cabin",    "RVR", LZ_NET_MT, "!1c2d3e4f", "Client",   -15.1f, 80,  "-15.1", "28m", "T-Echo",      "11.3 km" },
    { "Ridge Repeater", "RDG", LZ_NET_MC, "MC-9a3f",   "Repeater", 5.4f,   78,  "+5.4",  "now", "RAK19007",    "3.1 km"  },
    { "Dmitri K",       "DMI", LZ_NET_MC, "MC-4f8e",   "Chat",     -9.0f,  54,  "-9.0",  "1h",  "T-Deck",      "2.6 km"  },
    { "Base Camp",      "BCR", LZ_NET_MC, "MC-room",   "Room",     NAN,    -1,  "-",     "5m",  "Room Server", "3.1 km"  },
    { "Weather-Sensor", "WX",  LZ_NET_MC, "MC-sens",   "Sensor",   1.2f,   88,  "+1.2",  "1m",  "RAK Sensor",  "3.0 km"  },
};

const char *LZ_TERM_LINES[12] = {
    "LimitlezzOS Beta 0.6  -  serial console",
    "limitlezz:~$ mesh --info",
    "owner : Jess (JESS)",
    "nets  : meshtastic[on] meshcore[on]",
    "nodes : 7 reachable, 2 routers",
    "region: US   preset: LONG_FAST",
    "limitlezz:~$ mesh --nodes --short",
    "B01  +9.5  0hop  92%  router",
    "SMT  +2.1  1hop 100%  router",
    "AVA  -7.2  2hop  68%  client",
    "limitlezz:~$ mesh --airtime",
    "tx 412  rx 1284  util 3.4%",
};
const int LZ_TERM_KIND[12] = { 0, 1, 2, 2, 2, 2, 1, 2, 2, 2, 1, 2 };

const lz_sys_stat_t LZ_SYS_STATS[5] = {
    { "CPU - ESP32-S3 @ 240MHz", "12%",          12 },
    { "RAM",                     "84 / 512 KB",  16 },
    { "Flash storage",           "6.2 / 16 MB",  39 },
    { "Temperature",             "24 C",         27 },   /* 24 of a 0-90C range */
    { "Uptime",                  "3d 04:12",     0  },   /* open-ended: no bar */
};

lv_color_t lz_tile_color(int hue)
{
    switch(hue) {
        case 165: return LZ_TILE_165;
        case 205: return LZ_TILE_205;
        case 72:  return LZ_TILE_72;
        case 318: return LZ_TILE_318;
        case 280: return LZ_TILE_280;
        case 242: return LZ_TILE_242;
        case 40:  return LZ_TILE_40;
        case 95:  return LZ_TILE_95;
        case 200: return LZ_TILE_200;
        case 48:  return LZ_TILE_48;
        case 330: return LZ_TILE_330;
        case 18:  return LZ_TILE_18;
        case 150: return LZ_TILE_165;
        default:  return LZ_GRAPHITE;
    }
}

const char *lz_app_icon_glyph(const char *token)
{
    if(!token) return LZ_I_DESCRIPTION;
    if(strcmp(token, "calculate") == 0 || strcmp(token, "calculator") == 0) return LZ_I_CALCULATE;
    if(strcmp(token, "note") == 0 || strcmp(token, "notes") == 0) return LZ_I_NOTE;
    if(strcmp(token, "satellite") == 0 || strcmp(token, "aprs") == 0) return LZ_I_SATELLITE;
    if(strcmp(token, "thermostat") == 0 || strcmp(token, "weather") == 0) return LZ_I_THERMOSTAT;
    if(strcmp(token, "dns") == 0 || strcmp(token, "bbs") == 0) return LZ_I_DNS;
    if(strcmp(token, "graphic_eq") == 0 || strcmp(token, "scope") == 0) return LZ_I_GRAPHIC_EQ;
    if(strcmp(token, "gamepad") == 0 || strcmp(token, "game") == 0) return LZ_I_GAMEPAD;
    if(strcmp(token, "map") == 0 || strcmp(token, "maps") == 0) return LZ_I_MAP;
    if(strcmp(token, "terminal") == 0) return LZ_I_TERMINAL;
    if(strcmp(token, "folder") == 0) return LZ_I_FOLDER;
    if(strcmp(token, "store") == 0 || strcmp(token, "storefront") == 0) return LZ_I_STOREFRONT;
    return LZ_I_DESCRIPTION;
}

lv_color_t lz_net_color(lz_net_t n) { return n == LZ_NET_MT ? LZ_CYAN : LZ_AMBER; }
lv_color_t lz_av_color(lz_net_t n)  { return n == LZ_NET_MT ? LZ_AV_MT : LZ_AV_MC; }
const char *lz_net_name(lz_net_t n) { return n == LZ_NET_MT ? "Meshtastic" : "MeshCore"; }

lv_color_t lz_snr_color(float snr)
{
    if(isnan(snr)) return LZ_TEXT_META;
    if(snr >= 0)   return LZ_SNR_GOOD;
    if(snr >= -10) return LZ_SNR_MID;
    return LZ_SNR_BAD;
}
