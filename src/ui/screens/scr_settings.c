/* Settings (airtime scheduler, first-class network toggles, grouped rows)
 * + System (battery ring & stats, opened via Settings -> Device) */
#include "../ui.h"
#include <stdio.h>

static const char *REGIONS[]  = { "US", "EU868", "ANZ", "CN", "JP" };
static const char *PRESETS[]  = { "Long/Fast", "Long/Slow", "Med/Fast", "Short/Fast" };
static const char *TXPOW[]    = { "Low", "Medium", "High", "Max" };
static const char *TIMEOUTS[] = { "15s", "30s", "1m", "5m", "Never" };

enum { ROW_VALUE, ROW_TOGGLE, ROW_SLIDER, ROW_NAV };

typedef struct {
    const char *label, *icon;
    int kind;
} srow_t;

/* focus indices 2..10 map onto this table (this is a dark-only OS — no dark
 * mode toggle; Wi-Fi opens its own setup screen) */
static const srow_t SROWS[9] = {
    { "Region",           LZ_I_PUBLIC,     ROW_VALUE  },  /* f=2  */
    { "Modem preset",     LZ_I_GRAPHIC_EQ, ROW_VALUE  },  /* f=3  */
    { "TX power",         LZ_I_CELL_TOWER, ROW_VALUE  },  /* f=4  */
    { "Wi-Fi",            LZ_I_WIFI,       ROW_NAV    },  /* f=5  */
    { "GPS",              LZ_I_LOCATION,   ROW_TOGGLE },  /* f=6  */
    { "Brightness",       LZ_I_BRIGHTNESS, ROW_SLIDER },  /* f=7  */
    { "Sleep after",      LZ_I_SCHEDULE,   ROW_VALUE  },  /* f=8  */
    { "Power saving",     LZ_I_BOLT,       ROW_TOGGLE },  /* f=9  */
    { "System & battery", LZ_I_MONITORING, ROW_NAV    },  /* f=10 */
};
#define SETTINGS_FOCUS_COUNT 11   /* 2 network rows + 9 SROWS */

static void cycle(int *idx, int n) { *idx = (*idx + 1) % n; }

static void settings_activate(int f)
{
    switch(f) {
        case 0: S.net_mt = !S.net_mt; break;
        case 1: if(!LZ_MESHCORE_ENABLED) return; S.net_mc = !S.net_mc; break;
        case 2: cycle(&S.settings.region, 5); break;
        case 3: cycle(&S.settings.preset, 4); break;
        case 4: cycle(&S.settings.tx, 4); break;
        case 5: lz_go(LZ_V_WIFI); return;          /* Wi-Fi setup */
        case 6: S.settings.gps = !S.settings.gps; break;
        case 7: return;                            /* slider: left/right adjusts */
        case 8: cycle(&S.settings.timeout, 5); break;
        case 9: S.settings.save = !S.settings.save; break;
        case 10: lz_go(LZ_V_SYSTEM); return;
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

    /* --- NETWORKS: first-class toggles (MeshCore locked until Stage 2) --- */
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
        const char *sub = is_mt
            ? (mt ? "Node JESS - US - LongFast" : "Disabled - history kept")
            : (locked ? "Coming soon" : mc ? "Companion - 5 contacts" : "Disabled - history kept");
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
    static const struct { const char *title; int first, count; } GROUPS[5] = {
        { "RADIO",        2, 3 }, { "CONNECTIVITY", 5, 2 },
        { "DISPLAY",      7, 2 }, { "POWER",        9, 1 }, { "DEVICE", 10, 1 },
    };
    char bval[8];
    for(int g = 0; g < 5; g++) {
        lv_obj_t *card = group_card(body, GROUPS[g].title);
        for(int k = 0; k < GROUPS[g].count; k++) {
            int f = GROUPS[g].first + k;
            const srow_t *r = &SROWS[f - 2];
            lv_obj_t *row = setting_row_base(card, S.focus == f, k == GROUPS[g].count - 1);
            lz_icon(row, r->icon, &lz_icons_18, lv_color_hex(0xAAB0B9));
            lv_obj_t *lbl = lz_text(row, r->label, LZ_F_BODY, LZ_TEXT_SETTING);
            lv_obj_set_flex_grow(lbl, 1);
            switch(f) {
                case 2: value_chevron(row, REGIONS[S.settings.region]); break;
                case 3: value_chevron(row, PRESETS[S.settings.preset]); break;
                case 4: value_chevron(row, TXPOW[S.settings.tx]); break;
                case 5: value_chevron(row, lz_wifi_connected() ? lz_wifi_connected()
                                          : (lz_wifi_enabled() ? "On" : "Off")); break;
                case 6: lz_toggle(row, S.settings.gps, LZ_TOGGLE_ON); break;
                case 7: {
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
                case 8: value_chevron(row, TIMEOUTS[S.settings.timeout]); break;
                case 9: lz_toggle(row, S.settings.save, LZ_TOGGLE_ON); break;
                case 10: value_chevron(row, "87% - 24C"); break;
            }
            lz_nav_track(row, f);
        }
    }
    (void)bval;

    /* --- ABOUT (static) --- */
    lv_obj_t *about = group_card(body, "ABOUT");
    const char *ks[2] = { "Device", "Firmware" };
    const char *vs[2] = { "LilyGo T-Deck", "LimitlezzOS 1.0" };
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
}

/* ===== System ===== */

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
    lv_arc_set_value(ring, 87);
    lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(ring, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ring, lv_color_hex(0x20252D), LV_PART_MAIN);
    lv_obj_set_style_arc_width(ring, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ring, LZ_GREEN_BRIGHT, LV_PART_INDICATOR);
    lv_obj_t *rp = lz_text(ring, "87%", LZ_F_HEAD, LZ_TEXT);
    lv_obj_align(rp, LV_ALIGN_CENTER, 0, -4);
    lv_obj_t *rv = lz_text(ring, "3.94V", LZ_F_SMALL, lv_color_hex(0x7F868F));
    lv_obj_align(rv, LV_ALIGN_CENTER, 0, 12);

    lv_obj_t *hc = lz_box(hero);
    lv_obj_set_flex_grow(hc, 1);
    lv_obj_set_height(hc, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hc, 2, 0);
    lz_text(hc, "14h 20m left", LZ_F_HEAD, LZ_TEXT);
    lz_text(hc, "Discharging - -142 mA", LZ_F_SMALL, LZ_TEXT_2);
    lv_obj_t *chip = lz_box(hc);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, 16);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(chip, LZ_GREEN_BG, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(chip, 7, 0);
    lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(chip, 4, 0);
    lz_dot(chip, 5, LZ_GREEN_BRIGHT);
    lz_text(chip, "Battery healthy", LZ_F_SMALL, LZ_GREEN_TXT);

    /* stat bars */
    const lv_color_t bar_colors[5] = { LZ_BAR_CYAN, LZ_BAR_CYAN, LZ_BAR_MINT,
                                       LZ_BAR_GREEN, LZ_BAR_PURPLE };
    for(int i = 0; i < 5; i++) {
        const lz_sys_stat_t *s = &LZ_SYS_STATS[i];
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
        lz_text(hd, s->label, LZ_F_SMALL, LZ_TEXT_VALUE);
        lz_text(hd, s->value, LZ_F_SMALL, LZ_TEXT_STRONG);
        lv_obj_t *track = lz_box(st);
        lv_obj_set_size(track, lv_pct(100), 5);
        lv_obj_set_style_radius(track, 3, 0);
        lv_obj_set_style_bg_color(track, LZ_INSET_BG, 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
        lv_obj_t *fillb = lz_box(track);
        lv_obj_set_size(fillb, lv_pct(s->pct), 5);
        lv_obj_set_style_radius(fillb, 3, 0);
        lv_obj_set_style_bg_color(fillb, bar_colors[i], 0);
        lv_obj_set_style_bg_opa(fillb, LV_OPA_COVER, 0);
    }

    /* radio cards */
    lv_obj_t *cards = lz_box(body);
    lv_obj_set_width(cards, lv_pct(100));
    lv_obj_set_height(cards, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cards, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cards, 7, 0);
    const char *cl[2] = { "LoRa TX / RX", "Air utilization" };
    const char *cv[2] = { "412 / 1284", "3.4%" };
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
}
