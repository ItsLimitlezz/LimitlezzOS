/* Settings (airtime scheduler, first-class network toggles, grouped rows)
 * + System (battery ring & stats, opened via Settings -> Device) */
#include "../ui.h"
#include <stdio.h>

static const char *TXPOW[]    = { "Low", "Medium", "High", "Max" };
static const int   TXPOW_DBM[] = { 2, 8, 17, 22 };
static const char *TIMEOUTS[] = { "15s", "30s", "1m", "5m", "Never" };
static const char *KBLIGHT[]  = { "Auto", "On", "Off" };

enum { ROW_VALUE, ROW_TOGGLE, ROW_SLIDER, ROW_NAV };

typedef struct {
    const char *label, *icon;
    int kind;
} srow_t;

/* focus indices 2..10 map onto this table (this is a dark-only OS — no dark
 * mode toggle; Wi-Fi opens its own setup screen) */
/* Meshtastic is fixed to US / LongFast (no Region or Modem-preset options) and
 * MeshCore to its single public profile, so neither is configurable here. */
static const srow_t SROWS[] = {   /* unsized: never drops a row when one is added */
    { "TX power",         LZ_I_CELL_TOWER, ROW_VALUE  },  /* f=2  */
    { "Wi-Fi",            LZ_I_WIFI,       ROW_NAV    },  /* f=3  */
    { "GPS",              LZ_I_LOCATION,   ROW_TOGGLE },  /* f=4  */
    { "Brightness",       LZ_I_BRIGHTNESS, ROW_SLIDER },  /* f=5  */
    { "Keyboard light",   LZ_I_BRIGHTNESS, ROW_VALUE  },  /* f=6  */
    { "Sleep after",      LZ_I_SCHEDULE,   ROW_VALUE  },  /* f=7  */
    { "Time zone",        LZ_I_PUBLIC,     ROW_VALUE  },  /* f=8  */
    { "Set time",         LZ_I_SCHEDULE,   ROW_NAV    },  /* f=9  */
    { "Clock format",     LZ_I_SCHEDULE,   ROW_VALUE  },  /* f=10 */
    { "Power saving",     LZ_I_BOLT,       ROW_TOGGLE },  /* f=11 */
    { "System & battery", LZ_I_MONITORING, ROW_NAV    },  /* f=12 */
};
#define SETTINGS_FOCUS_COUNT 13   /* 2 network rows + 11 SROWS */

/* Named regions people recognize. Each carries a STANDARD-time offset plus a
 * daylight rule, so the clock follows DST automatically — pick "Eastern" once
 * and it shows EDT in summer, EST in winter. (Arizona/Hawaii don't observe it.) */
static const struct { const char *name; const char *std; const char *dst; int off_min; int rule; } TZ[] = {
    { "Eastern",     "EST",  "EDT",  -300, LZ_DST_US   },
    { "Central",     "CST",  "CDT",  -360, LZ_DST_US   },
    { "Mountain",    "MST",  "MDT",  -420, LZ_DST_US   },
    { "Pacific",     "PST",  "PDT",  -480, LZ_DST_US   },
    { "Alaska",      "AKST", "AKDT", -540, LZ_DST_US   },
    { "Arizona",     "MST",  "MST",  -420, LZ_DST_NONE },
    { "Hawaii",      "HST",  "HST",  -600, LZ_DST_NONE },
    { "UTC",         "UTC",  "UTC",     0, LZ_DST_NONE },
    { "UK",          "GMT",  "BST",     0, LZ_DST_EU   },
    { "Central EU",  "CET",  "CEST",   60, LZ_DST_EU   },
    { "Eastern EU",  "EET",  "EEST",  120, LZ_DST_EU   },
    { "India",       "IST",  "IST",   330, LZ_DST_NONE },
    { "Japan",       "JST",  "JST",   540, LZ_DST_NONE },
    { "Sydney",      "AEST", "AEST",  600, LZ_DST_NONE },
};
#define TZ_COUNT ((int)(sizeof TZ / sizeof TZ[0]))
int lz_tz_offset(int idx) { return TZ[(idx >= 0 && idx < TZ_COUNT) ? idx : 0].off_min; }
/* push the zone at idx into the service (offset + DST rule + abbrevs) */
void lz_tz_apply(int idx)
{
    int i = (idx >= 0 && idx < TZ_COUNT) ? idx : 0;
    lz_svc_set_tz_zone(TZ[i].off_min, TZ[i].rule, TZ[i].std, TZ[i].dst);
}

/* zone lookups for the serial console (`tz <name|abbrev>`) */
static bool tz_ci_eq(const char *a, const char *b)
{
    for(; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if(ca >= 'A' && ca <= 'Z') ca += 32;
        if(cb >= 'A' && cb <= 'Z') cb += 32;
        if(ca != cb) return false;
    }
    return *a == *b;
}
int lz_tz_count(void) { return TZ_COUNT; }
const char *lz_tz_name(int idx) { return TZ[(idx >= 0 && idx < TZ_COUNT) ? idx : 0].name; }
int lz_tz_find(const char *s)
{
    for(int i = 0; i < TZ_COUNT; i++)
        if(tz_ci_eq(TZ[i].name, s) || tz_ci_eq(TZ[i].std, s) || tz_ci_eq(TZ[i].dst, s)) return i;
    return -1;
}

static void cycle(int *idx, int n) { *idx = (*idx + 1) % n; }

/* focus skips the MeshCore network row while it's locked (Stage 1) */
static bool settings_disabled(int idx) { return idx == 1 && !LZ_MESHCORE_ENABLED; }

static void settings_activate(int f)
{
    switch(f) {
        case 0: S.net_mt = !S.net_mt; lz_apply_networks(); break;
        case 1: S.net_mc = !S.net_mc; lz_apply_networks(); break;
        case 2: cycle(&S.settings.tx, 4); lz_backend_set_tx_power(TXPOW_DBM[S.settings.tx]); break;
        case 3: lz_go(LZ_V_WIFI); return;          /* Wi-Fi setup */
        case 4: S.settings.gps = !S.settings.gps; break;
        case 5: return;                            /* slider: left/right adjusts */
        case 6: cycle(&S.settings.kb_light, 3); break;
        case 7: cycle(&S.settings.timeout, 5); break;
        case 8: lz_go(LZ_V_TZPICK); S.focus = S.settings.tz_idx; lz_rebuild(); return;
        case 9: lz_settime_enter(); lz_go(LZ_V_SETTIME); return;
        case 10: S.settings.clock24 = !S.settings.clock24; lz_svc_set_clock24(S.settings.clock24); break;
        case 11: S.settings.save = !S.settings.save; break;
        case 12: lz_go(LZ_V_SYSTEM); return;
        default: return;
    }
    lz_rebuild();
}

static lv_obj_t *group_card(lv_obj_t *body, const char *title)
{
    lv_obj_t *hd = lz_text(body, title, LZ_F_SMALL, LZ_TEXT_3);
    lv_obj_set_style_pad_left(hd, 4, 0);
    lv_obj_set_style_pad_top(hd, 4, 0);
    lv_obj_t *card = lz_box(body);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(card, LZ_RADIUS_CARD, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, LZ_CARD_BORDER, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    return card;
}

static lv_obj_t *setting_row_base(lv_obj_t *card, bool focused, bool last)
{
    lv_obj_t *row = lz_box(card);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, focused ? LZ_ROW_FOCUS_BG : LZ_ROW_BG, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    if(focused) {
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_border_color(row, LZ_FOCUS, 0);
    } else if(!last) {
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x1E232A), 0);
    }
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, 10, 0);
    lv_obj_set_style_pad_ver(row, 7, 0);
    lv_obj_set_style_pad_column(row, 9, 0);
    return row;
}

static void value_chevron(lv_obj_t *row, const char *value)
{
    lz_text(row, value, LZ_F_SMALL, LZ_TEXT_VALUE);
    lz_icon(row, LZ_I_CHEV_R, &lz_icons_14, LZ_TEXT_3);
}

void lz_scr_settings(lv_obj_t *root)
{
    bool mt = S.net_mt, mc = S.net_mc, both = mt && mc;
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lz_navbar(root, "Settings", NULL);

    lv_obj_t *body = lz_vflex(root);
    lv_obj_set_style_pad_top(body, 8, 0);
    lv_obj_set_style_pad_hor(body, 9, 0);
    lv_obj_set_style_pad_bottom(body, 10, 0);
    lv_obj_set_style_pad_row(body, 5, 0);
    lz_nav_set_scroll(body);

    /* --- airtime scheduler card --- */
    lv_obj_t *air = lz_card(body);
    lv_obj_set_height(air, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(air, 9, 0);
    lv_obj_set_flex_flow(air, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(air, 6, 0);

    lv_obj_t *ahead = lz_box(air);
    lv_obj_set_width(ahead, lv_pct(100));
    lv_obj_set_height(ahead, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ahead, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ahead, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lz_text(ahead, "AIRTIME SCHEDULER", LZ_F_SMALL, lv_color_hex(0x7F868F));
    const char *alabel = both ? "Split 50 / 50" : mt ? "Meshtastic 100%"
                       : mc ? "MeshCore 100%" : "Idle";
    lz_text(ahead, alabel, LZ_F_SMALL, LZ_TEXT_VALUE);

    lv_obj_t *abar = lz_box(air);
    lv_obj_set_width(abar, lv_pct(100));
    lv_obj_set_height(abar, 7);
    lv_obj_set_style_radius(abar, 4, 0);
    lv_obj_set_style_bg_color(abar, LZ_INSET_BG, 0);
    lv_obj_set_style_bg_opa(abar, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(abar, true, 0);
    lv_obj_set_flex_flow(abar, LV_FLEX_FLOW_ROW);
    int mt_pct = mt ? (both ? 50 : 100) : 0;
    int mc_pct = mc ? (both ? 50 : 100) : 0;
    if(mt_pct) {
        lv_obj_t *seg = lz_box(abar);
        lv_obj_set_size(seg, lv_pct(mt_pct), lv_pct(100));
        lv_obj_set_style_bg_color(seg, LZ_AIRTIME_MT, 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    }
    if(mc_pct) {
        lv_obj_t *seg = lz_box(abar);
        lv_obj_set_size(seg, lv_pct(mc_pct), lv_pct(100));
        lv_obj_set_style_bg_color(seg, LZ_AIRTIME_MC, 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    }

    const char *anote = both
        ? "Both radios share airtime. Disable one to give the other full throughput and lower latency."
        : (mt || mc)
        ? "One network disabled - the active radio now has full airtime: faster delivery, lower latency."
        : "Both networks disabled. Enable one to start receiving.";
    lv_obj_t *noteL = lz_text(air, anote, LZ_F_SMALL, lv_color_hex(0x7F868F));
    lv_label_set_long_mode(noteL, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(noteL, lv_pct(100));

    /* --- NETWORKS: first-class toggles; flipping these drives the radio's
     *     time-division schedule (both = split, one = 100%) --- */
    lv_obj_t *nets = group_card(body, "NETWORKS");
    for(int i = 0; i < 2; i++) {
        bool is_mt = i == 0;
        bool on = is_mt ? mt : mc;
        bool locked = !is_mt && !LZ_MESHCORE_ENABLED;
        lv_obj_t *row = setting_row_base(nets, S.focus == i, i == 1);
        if(locked) lv_obj_set_style_opa(row, LV_OPA_50, 0);
        lv_obj_t *tile = lz_box(row);
        lv_obj_set_size(tile, 28, 28);
        lv_obj_set_style_radius(tile, 8, 0);
        lv_obj_set_style_bg_color(tile, is_mt ? LZ_IDTILE_MT : LZ_IDTILE_MC, 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_t *ic = lz_icon(tile, is_mt ? LZ_I_HUB : LZ_I_LAN, &lz_icons_16f,
                               is_mt ? LZ_ON_CYAN : LZ_ON_AMBER);
        lv_obj_center(ic);
        lv_obj_t *cl = lz_box(row);
        lv_obj_set_flex_grow(cl, 1);
        lv_obj_set_height(cl, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cl, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(cl, 1, 0);
        lz_text(cl, is_mt ? "Meshtastic" : "MeshCore", LZ_F_BODY, LZ_TEXT);
        char mtsub[40], mcsub[48];
        snprintf(mtsub, sizeof mtsub, "Node %s - US - LongFast", lz_svc_identity()->short_name);
        int mcn = lz_svc_node_count(LZ_NET_MC);   /* real heard-node count */
        if(mcn > 0) snprintf(mcsub, sizeof mcsub, "%d node%s heard", mcn, mcn == 1 ? "" : "s");
        else        snprintf(mcsub, sizeof mcsub, "Listening for nodes");
        const char *sub = is_mt
            ? (mt ? mtsub : "Disabled - history kept")
            : (mc ? mcsub : "Disabled - history kept");
        lz_text(cl, sub, LZ_F_SMALL, is_mt ? LZ_TEXT_2 : lv_color_hex(0x988E7C));
        if(locked) {
            lv_obj_t *chip = lz_box(row);
            lv_obj_set_size(chip, LV_SIZE_CONTENT, 16);
            lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(chip, lv_color_hex(0x252A31), 0);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
            lv_obj_set_style_pad_hor(chip, 7, 0);
            lv_obj_t *cl2 = lz_text(chip, "Soon", LZ_F_SMALL, lv_color_hex(0x868F99));
            lv_obj_center(cl2);
        } else {
            lz_toggle(row, on, is_mt ? LZ_TRACK_MT : LZ_TRACK_MC);
        }
    }

    /* --- grouped rows --- */
    static const struct { const char *title; int first, count; } GROUPS[6] = {
        { "RADIO",        2, 1 }, { "CONNECTIVITY", 3, 2 },
        { "DISPLAY",      5, 3 }, { "TIME",         8, 3 },
        { "POWER",       11, 1 }, { "DEVICE",      12, 1 },
    };
    char bval[8];
    for(int g = 0; g < 6; g++) {
        lv_obj_t *card = group_card(body, GROUPS[g].title);
        for(int k = 0; k < GROUPS[g].count; k++) {
            int f = GROUPS[g].first + k;
            const srow_t *r = &SROWS[f - 2];
            lv_obj_t *row = setting_row_base(card, S.focus == f, k == GROUPS[g].count - 1);
            lz_icon(row, r->icon, &lz_icons_18, lv_color_hex(0xAAB0B9));
            lv_obj_t *lbl = lz_text(row, r->label, LZ_F_BODY, LZ_TEXT_SETTING);
            lv_obj_set_flex_grow(lbl, 1);
            switch(f) {
                case 2: value_chevron(row, TXPOW[S.settings.tx]); break;
                case 3: value_chevron(row, lz_wifi_connected() ? lz_wifi_connected()
                                          : (lz_wifi_enabled() ? "On" : "Off")); break;
                case 4: lz_toggle(row, S.settings.gps, LZ_TOGGLE_ON); break;
                case 5: {
                    /* brightness slider (left/right adjusts while focused) */
                    lv_obj_t *track = lz_box(row);
                    lv_obj_set_size(track, 96, 5);
                    lv_obj_set_style_radius(track, 3, 0);
                    lv_obj_set_style_bg_color(track, lv_color_hex(0x22272F), 0);
                    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
                    lv_obj_t *fillb = lz_box(track);
                    lv_obj_set_size(fillb, lv_pct(S.settings.bright), 5);
                    lv_obj_set_style_radius(fillb, 3, 0);
                    lv_obj_set_style_bg_color(fillb, LZ_SLIDER_HI, 0);
                    lv_obj_set_style_bg_opa(fillb, LV_OPA_COVER, 0);
                    lv_obj_t *knob = lz_dot(track, 11, LZ_KNOB);
                    lv_obj_align(knob, LV_ALIGN_LEFT_MID, (96 * S.settings.bright) / 100 - 5, 0);
                    break;
                }
                case 6: value_chevron(row, KBLIGHT[S.settings.kb_light]); break;
                case 7: value_chevron(row, TIMEOUTS[S.settings.timeout]); break;
                case 8: { char zb[24]; snprintf(zb, sizeof zb, "%s (%s)",
                               TZ[S.settings.tz_idx].name, lz_svc_tz_abbrev());
                           value_chevron(row, zb); break; }
                case 9: { char tb[12]; value_chevron(row, lz_fmt_now(tb, sizeof tb)); break; }
                case 10: value_chevron(row, S.settings.clock24 ? "24-hour" : "12-hour"); break;
                case 11: lz_toggle(row, S.settings.save, LZ_TOGGLE_ON); break;
                case 12: {
                    lz_sysinfo_t si; lz_svc_sysinfo(&si);
                    char sb[16];
                    if(si.battery_pct >= 0) snprintf(sb, sizeof sb, "%d%% - %dC", si.battery_pct, si.temp_c);
                    else snprintf(sb, sizeof sb, "USB - %dC", si.temp_c);
                    value_chevron(row, sb); break;
                }
            }
            lz_nav_track(row, f);
        }
    }
    (void)bval;

    /* --- ABOUT (static) --- */
    lv_obj_t *about = group_card(body, "ABOUT");
    const char *ks[2] = { "Device", "Firmware" };
    const char *vs[2] = { "LilyGo T-Deck", "LimitlezzOS Alpha 0.2" };
    for(int i = 0; i < 2; i++) {
        lv_obj_t *r = lz_box(about);
        lv_obj_set_width(r, lv_pct(100));
        lv_obj_set_height(r, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(r, 10, 0);
        lv_obj_set_style_pad_ver(r, 7, 0);
        if(i == 0) {
            lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(r, 1, 0);
            lv_obj_set_style_border_color(r, lv_color_hex(0x1E232A), 0);
        }
        lz_text(r, ks[i], LZ_F_SMALL, lv_color_hex(0x8B939C));
        lz_text(r, vs[i], LZ_F_SMALL, LZ_TEXT_STRONG);
    }

    lz_nav_set(1, SETTINGS_FOCUS_COUNT, settings_activate);
    lz_nav_set_skip(settings_disabled);
}

/* ===== System ===== */

static lv_obj_t *g_uptime_lbl;   /* updated in place each second (no rebuild -> no scroll jump) */
static void fmt_uptime(uint32_t up, char *b, size_t n)
{
    if(up >= 86400) snprintf(b, n, "%ud %02u:%02u", up/86400, (up%86400)/3600, (up%3600)/60);
    else            snprintf(b, n, "%02u:%02u:%02u", up/3600, (up%3600)/60, up%60);
}
static void system_refresh_cb(lv_timer_t *tm)
{
    (void)tm;
    if(S.view != LZ_V_SYSTEM || !g_uptime_lbl) return;
    lz_sysinfo_t si; lz_svc_sysinfo(&si);
    char upv[20]; fmt_uptime(si.uptime_s, upv, sizeof upv);
    lv_label_set_text(g_uptime_lbl, upv);     /* live tick, scroll position kept */
}
static void system_timer_del_cb(lv_event_t *e) { lv_timer_del((lv_timer_t *)lv_event_get_user_data(e)); }

void lz_scr_system(lv_obj_t *root)
{
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lz_navbar(root, NULL, "Settings");

    lv_obj_t *body = lz_vflex(root);
    lv_obj_set_style_pad_top(body, 10, 0);
    lv_obj_set_style_pad_hor(body, 11, 0);
    lv_obj_set_style_pad_bottom(body, 10, 0);
    lv_obj_set_style_pad_row(body, 8, 0);
    lz_nav_set_scroll(body);

    /* battery ring + summary */
    lv_obj_t *hero = lz_box(body);
    lv_obj_set_width(hero, lv_pct(100));
    lv_obj_set_height(hero, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hero, 13, 0);

    lv_obj_t *ring = lv_arc_create(hero);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, 58, 58);
    lv_arc_set_rotation(ring, 270);
    lv_arc_set_bg_angles(ring, 0, 360);
    lv_arc_set_range(ring, 0, 100);
    lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(ring, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ring, lv_color_hex(0x20252D), LV_PART_MAIN);
    lv_obj_set_style_arc_width(ring, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ring, LZ_GREEN_BRIGHT, LV_PART_INDICATOR);

    /* real values from the platform (sim shows demo numbers) */
    lz_sysinfo_t si; lz_svc_sysinfo(&si);
    bool have_batt = si.battery_pct >= 0;
    lv_arc_set_value(ring, have_batt ? si.battery_pct : 0);
    char rpb[8]; snprintf(rpb, sizeof rpb, have_batt ? "%d%%" : "USB", si.battery_pct);
    lv_obj_t *rp = lz_text(ring, rpb, LZ_F_HEAD, LZ_TEXT);
    lv_obj_align(rp, LV_ALIGN_CENTER, 0, -4);
    char rvb[10]; snprintf(rvb, sizeof rvb, "%.2fV", (double)si.battery_v);
    lv_obj_t *rv = lz_text(ring, rvb, LZ_F_SMALL, lv_color_hex(0x7F868F));
    lv_obj_align(rv, LV_ALIGN_CENTER, 0, 12);

    lv_obj_t *hc = lz_box(hero);
    lv_obj_set_flex_grow(hc, 1);
    lv_obj_set_height(hc, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hc, 2, 0);
    lz_text(hc, si.usb ? "On USB power" : (have_batt ? "On battery" : "Powered"), LZ_F_HEAD, LZ_TEXT);
    char vline[24]; snprintf(vline, sizeof vline, "%.2f V", (double)si.battery_v);
    lz_text(hc, vline, LZ_F_SMALL, LZ_TEXT_2);
    lv_obj_t *chip = lz_box(hc);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, 16);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(chip, LZ_GREEN_BG, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(chip, 7, 0);
    lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(chip, 4, 0);
    /* health baseline from resting voltage: a healthy LiPo sits ~3.7-4.2V under
     * light load; sustained low voltage means it needs service/charging */
    const char *health; lv_color_t hcol, hdot;
    if(si.usb)                  { health = "Powered over USB"; hcol = LZ_GREEN_TXT; hdot = LZ_GREEN_BRIGHT; }
    else if(si.battery_v >= 3.7f){ health = "Battery healthy";  hcol = LZ_GREEN_TXT; hdot = LZ_GREEN_BRIGHT; }
    else if(si.battery_v >= 3.4f){ health = "Battery OK";       hcol = LZ_GREEN_TXT; hdot = LZ_GREEN_BRIGHT; }
    else if(si.battery_v >= 3.2f){ health = "Battery low";      hcol = LZ_SNR_MID;   hdot = LZ_SNR_MID; }
    else                         { health = "Service battery";  hcol = LZ_SNR_BAD;   hdot = LZ_SNR_BAD; }
    lv_obj_set_style_bg_color(chip, lv_color_hex(0x1A2520), 0);
    lz_dot(chip, 5, hdot);
    lz_text(chip, health, LZ_F_SMALL, hcol);

    /* live stat rows */
    char cpuv[16], ramv[24], flashv[24], tempv[12], upv[20];
    snprintf(cpuv, sizeof cpuv, "%d MHz", si.cpu_mhz);
    snprintf(ramv, sizeof ramv, "%d / %d KB", si.ram_used_kb, si.ram_total_kb);
    snprintf(flashv, sizeof flashv, "%.1f / %.1f MB", si.flash_used_kb / 1024.0, si.flash_total_kb / 1024.0);
    snprintf(tempv, sizeof tempv, "%d C", si.temp_c);
    uint32_t up = si.uptime_s;
    if(up >= 86400) snprintf(upv, sizeof upv, "%ud %02u:%02u", up/86400, (up%86400)/3600, (up%3600)/60);
    else            snprintf(upv, sizeof upv, "%02u:%02u:%02u", up/3600, (up%3600)/60, up%60);
    struct { const char *label, *value; int pct; bool bar; } rows[5] = {
        { "CPU",           cpuv,   0,  false },
        { "RAM",           ramv,   si.ram_total_kb ? si.ram_used_kb * 100 / si.ram_total_kb : 0, true },
        { "Flash storage", flashv, si.flash_total_kb ? si.flash_used_kb * 100 / si.flash_total_kb : 0, true },
        { "Temperature",   tempv,  si.temp_c > 0 ? (si.temp_c * 100 / 90) : 0, true },
        { "Uptime",        upv,    0,  false },
    };
    const lv_color_t bar_colors[5] = { LZ_BAR_CYAN, LZ_BAR_CYAN, LZ_BAR_MINT,
                                       LZ_BAR_GREEN, LZ_BAR_PURPLE };
    for(int i = 0; i < 5; i++) {
        lv_obj_t *st = lz_box(body);
        lv_obj_set_width(st, lv_pct(100));
        lv_obj_set_height(st, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(st, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(st, 3, 0);
        lv_obj_t *hd = lz_box(st);
        lv_obj_set_width(hd, lv_pct(100));
        lv_obj_set_height(hd, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(hd, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hd, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lz_text(hd, rows[i].label, LZ_F_SMALL, LZ_TEXT_VALUE);
        lv_obj_t *vlbl = lz_text(hd, rows[i].value, LZ_F_SMALL, LZ_TEXT_STRONG);
        if(i == 4) g_uptime_lbl = vlbl;   /* the per-second live tick target */
        if(!rows[i].bar) continue;
        int p = rows[i].pct; if(p < 0) p = 0; if(p > 100) p = 100;
        lv_obj_t *track = lz_box(st);
        lv_obj_set_size(track, lv_pct(100), 5);
        lv_obj_set_style_radius(track, 3, 0);
        lv_obj_set_style_bg_color(track, LZ_INSET_BG, 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
        lv_obj_t *fillb = lz_box(track);
        lv_obj_set_size(fillb, lv_pct(p), 5);
        lv_obj_set_style_radius(fillb, 3, 0);
        lv_obj_set_style_bg_color(fillb, bar_colors[i], 0);
        lv_obj_set_style_bg_opa(fillb, LV_OPA_COVER, 0);
    }

    /* radio cards — live TX/RX + airtime */
    lz_radio_stats_t rs; lz_svc_radio_stats(&rs);
    char txrx[20], util[12];
    snprintf(txrx, sizeof txrx, "%u / %u", (unsigned)rs.tx_count, (unsigned)rs.rx_count);
    snprintf(util, sizeof util, "%.1f%%", (double)rs.util_pct);
    lv_obj_t *cards = lz_box(body);
    lv_obj_set_width(cards, lv_pct(100));
    lv_obj_set_height(cards, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cards, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cards, 7, 0);
    const char *cl[2] = { "LoRa TX / RX", "Air utilization" };
    const char *cv[2] = { txrx, util };
    for(int i = 0; i < 2; i++) {
        lv_obj_t *c = lz_box(cards);
        lv_obj_set_flex_grow(c, 1);
        lv_obj_set_height(c, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(c, 9, 0);
        lv_obj_set_style_bg_color(c, LZ_CARD_BG, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_border_color(c, LZ_CARD_BORDER, 0);
        lv_obj_set_style_pad_all(c, 8, 0);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(c, 2, 0);
        lz_text(c, cl[i], LZ_F_SMALL, lv_color_hex(0x7F868F));
        lz_text(c, cv[i], LZ_F_BODY, LZ_TEXT_STRONG);
    }

    lz_nav_set(1, 0, NULL);

    /* refresh once a second so uptime + live stats keep counting on this page */
    lv_timer_t *tm = lv_timer_create(system_refresh_cb, 1000, NULL);
    lv_obj_add_event_cb(body, system_timer_del_cb, LV_EVENT_DELETE, tm);
}

/* ===== Timezone picker (scrollable list) ===== */

static void tzpick_activate(int idx)
{
    if(idx < 0 || idx >= TZ_COUNT) return;
    S.settings.tz_idx = idx;
    lz_tz_apply(idx);
    lz_back();
}

void lz_scr_tzpick(lv_obj_t *root)
{
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lz_navbar(root, "Time zone", NULL);

    lv_obj_t *body = lz_vflex(root);
    lv_obj_set_style_pad_top(body, 5, 0);
    lv_obj_set_style_pad_hor(body, 7, 0);
    lv_obj_set_style_pad_bottom(body, 8, 0);
    lv_obj_set_style_pad_row(body, 3, 0);
    lz_nav_set_scroll(body);

    for(int i = 0; i < TZ_COUNT; i++) {
        bool sel = (i == S.settings.tz_idx);
        lv_obj_t *row = lz_row(body, i == S.focus);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_t *nm = lz_text(row, TZ[i].name, LZ_F_BODY, sel ? LZ_MINT : LZ_TEXT);
        lv_obj_set_flex_grow(nm, 1);
        int off = TZ[i].off_min;
        char ob[24];
        int oh = off / 60, om = (off < 0 ? -off : off) % 60;
        if(off == 0) snprintf(ob, sizeof ob, "UTC%s", TZ[i].rule != LZ_DST_NONE ? " ·DST" : "");
        else snprintf(ob, sizeof ob, "UTC%+d:%02d%s", oh, om,
                      TZ[i].rule != LZ_DST_NONE ? " ·DST" : "");
        lz_text(row, ob, LZ_F_SMALL, LZ_TEXT_VALUE);
        if(sel) lz_dot(row, 7, LZ_MINT);   /* current selection marker */
        lz_nav_track(row, i);
    }
    lz_nav_set(1, TZ_COUNT, tzpick_activate);
}

/* ===== Manual set-time editor ===== */

static int st_y, st_mo, st_d, st_h, st_mi, st_field;   /* 0 Y,1 Mo,2 D,3 H,4 Mi */
static const int MONTH_DAYS[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

void lz_settime_enter(void)
{
    lz_svc_get_clock(&st_y, &st_mo, &st_d, &st_h, &st_mi);
    if(st_y < 2024) st_y = 2026;          /* sane default if never synced */
    st_field = 3;                          /* start on the hour */
}

static int days_in(int y, int mo)
{
    int dd = MONTH_DAYS[(mo - 1) % 12];
    if(mo == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) dd = 29;
    return dd;
}

void lz_settime_key(lz_key_t k, char c)
{
    (void)c;
    int delta = (k == LZ_K_UP) ? 1 : (k == LZ_K_DOWN) ? -1 : 0;
    if(k == LZ_K_LEFT)  { if(st_field > 0) st_field--; else { lz_back(); return; } lz_rebuild(); return; }
    if(k == LZ_K_RIGHT) { if(st_field < 4) st_field++; lz_rebuild(); return; }
    if(k == LZ_K_DEL)   { lz_back(); return; }
    if(k == LZ_K_ENTER) { lz_svc_set_clock(st_y, st_mo, st_d, st_h, st_mi); lz_back(); return; }
    if(delta) {
        switch(st_field) {
            case 0: st_y += delta; if(st_y < 2024) st_y = 2024; if(st_y > 2099) st_y = 2099; break;
            case 1: st_mo += delta; if(st_mo < 1) st_mo = 12; if(st_mo > 12) st_mo = 1; break;
            case 2: { int dd = days_in(st_y, st_mo); st_d += delta; if(st_d < 1) st_d = dd; if(st_d > dd) st_d = 1; break; }
            case 3: st_h += delta; if(st_h < 0) st_h = 23; if(st_h > 23) st_h = 0; break;
            case 4: st_mi += delta; if(st_mi < 0) st_mi = 59; if(st_mi > 59) st_mi = 0; break;
        }
        lz_rebuild();
    }
}

void lz_scr_settime(lv_obj_t *root)
{
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lz_navbar(root, "Set time", NULL);

    lv_obj_t *body = lz_vflex(root);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_top(body, 14, 0);
    lv_obj_set_style_pad_row(body, 12, 0);

    lz_text(body, "Trackball: up/down change - left/right move - click to save",
            LZ_F_SMALL, LZ_TEXT_3);

    /* HH : MM */
    lv_obj_t *time_row = lz_box(body);
    lv_obj_set_size(time_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(time_row, 6, 0);
    int tvals[2] = { st_h, st_mi };
    for(int f = 0; f < 2; f++) {
        if(f == 1) lz_text(time_row, ":", LZ_F_BIG, LZ_TEXT);
        char v[4]; snprintf(v, sizeof v, "%02d", tvals[f]);
        lv_obj_t *cell = lz_box(time_row);
        lv_obj_set_size(cell, 56, 46);
        lv_obj_set_style_radius(cell, 9, 0);
        lv_obj_set_style_bg_color(cell, st_field == (f == 0 ? 3 : 4) ? LZ_ROW_FOCUS_BG : LZ_ROW_BG, 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        if(st_field == (f == 0 ? 3 : 4)) {
            lv_obj_set_style_border_width(cell, 2, 0);
            lv_obj_set_style_border_color(cell, LZ_FOCUS, 0);
        }
        lv_obj_t *t = lz_text(cell, v, LZ_F_BIG, LZ_TEXT);
        lv_obj_center(t);
    }

    /* date Y - M - D */
    static const char *MON[12] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
    lv_obj_t *date_row = lz_box(body);
    lv_obj_set_size(date_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(date_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(date_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(date_row, 6, 0);
    char yb[6], db[4]; snprintf(yb, sizeof yb, "%d", st_y); snprintf(db, sizeof db, "%d", st_d);
    const char *dvals[3] = { MON[(st_mo - 1) % 12], db, yb };
    int dfields[3] = { 1, 2, 0 };
    for(int i = 0; i < 3; i++) {
        lv_obj_t *cell = lz_box(date_row);
        lv_obj_set_size(cell, 50, 30);
        lv_obj_set_style_radius(cell, 8, 0);
        lv_obj_set_style_bg_color(cell, st_field == dfields[i] ? LZ_ROW_FOCUS_BG : LZ_ROW_BG, 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        if(st_field == dfields[i]) {
            lv_obj_set_style_border_width(cell, 2, 0);
            lv_obj_set_style_border_color(cell, LZ_FOCUS, 0);
        }
        lv_obj_t *t = lz_text(cell, dvals[i], LZ_F_BODY, LZ_TEXT);
        lv_obj_center(t);
    }

    lz_text(body, "Or connect Wi-Fi to set the time automatically", LZ_F_SMALL, LZ_TEXT_3);
    lz_nav_set(1, 0, NULL);   /* keys handled by lz_settime_key */
}
