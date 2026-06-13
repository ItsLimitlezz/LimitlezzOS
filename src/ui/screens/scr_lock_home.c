/* Lock screen + Home launcher (single iOS-style 4x2 grid per design) */
#include "../ui.h"
#include <stdio.h>
#include <string.h>

/* ===== Lock ===== */

static void lock_tap(void) { lz_ui_key(LZ_K_ENTER, 0); }  /* tap anywhere unlocks */

/* lock-screen notification: tapping it opens the conversation directly (which
 * clears its unread, so the notification goes away after you view it) */
static lz_thread_rt *g_notif_t;
static void notif_tap(void) { if(g_notif_t) lz_open_convo(g_notif_t); }

void lz_scr_lock(lv_obj_t *root)
{
    lv_obj_set_style_bg_color(root, lv_color_hex(0x0B0E13), 0);
    lz_on_click(root, lock_tap);

    /* top inset row: active networks left, battery right */
    lv_obj_t *hub = lz_icon(root, LZ_I_HUB, &lz_icons_14, LZ_CYAN);
    lv_obj_align(hub, LV_ALIGN_TOP_LEFT, 13, 11);
    if(LZ_MESHCORE_ENABLED) {
        lv_obj_t *lan = lz_icon(root, LZ_I_LAN, &lz_icons_14, LZ_AMBER);
        lv_obj_align(lan, LV_ALIGN_TOP_LEFT, 31, 11);
        lv_obj_t *nets = lz_text(root, "2 networks", LZ_F_SMALL, lv_color_hex(0x7F868F));
        lv_obj_align(nets, LV_ALIGN_TOP_LEFT, 50, 14);
    } else {
        lv_obj_t *nets = lz_text(root, "Meshtastic", LZ_F_SMALL, lv_color_hex(0x7F868F));
        lv_obj_align(nets, LV_ALIGN_TOP_LEFT, 31, 14);
    }

    lz_sysinfo_t si; lz_svc_sysinfo(&si);
    if(si.battery_pct >= 0) {
        char pb[8]; snprintf(pb, sizeof pb, "%d%%", si.battery_pct);
        lv_obj_t *pct = lz_text(root, pb, LZ_F_SMALL, lv_color_hex(0xAEB6BF));
        lv_obj_align(pct, LV_ALIGN_TOP_RIGHT, -36, 14);
    } else {
        lv_obj_t *pct = lz_text(root, "USB", LZ_F_SMALL, lv_color_hex(0xAEB6BF));
        lv_obj_align(pct, LV_ALIGN_TOP_RIGHT, -36, 14);
    }
    lv_obj_t *batt = lz_box(root);
    lv_obj_set_size(batt, 17, 9);
    lv_obj_set_style_radius(batt, 2, 0);
    lv_obj_set_style_border_width(batt, 1, 0);
    lv_obj_set_style_border_color(batt, lv_color_hex(0x767D86), 0);
    lv_obj_align(batt, LV_ALIGN_TOP_RIGHT, -13, 14);
    int lpct = si.battery_pct < 0 ? 100 : si.battery_pct;
    lv_obj_t *fill = lz_box(batt);
    lv_obj_set_size(fill, 1 + (13 * lpct) / 100, 5);
    lv_obj_set_style_bg_color(fill, (lpct <= 15 && !si.usb) ? lv_color_hex(0xE0564E) : LZ_GREEN_BRIGHT, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_align(fill, LV_ALIGN_LEFT_MID, 1, 0);

    /* clock + date (real time once synced, otherwise --:-- + a hint) */
    char clk[12]; lz_fmt_now(clk, sizeof clk);
    lv_obj_t *clock = lz_text(root, clk, LZ_F_CLOCK, LZ_TEXT);
    lv_obj_align(clock, LV_ALIGN_CENTER, 0, -38);
    char dbuf[28]; lz_fmt_date(dbuf, sizeof dbuf);
    lv_obj_t *date = lz_text(root, dbuf, LZ_F_BODY, LZ_TEXT_VALUE);
    lv_obj_align(date, LV_ALIGN_CENTER, 0, -4);

    /* unlock pill */
    lv_obj_t *pill = lz_box(root);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, 28);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pill, LZ_CHIP_BG, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pill, 1, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(0x262B33), 0);
    lv_obj_set_style_pad_hor(pill, 14, 0);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(pill, 7, 0);
    /* trackball: a proper round ball primitive */
    lv_obj_t *ball = lz_box(pill);
    lv_obj_set_size(ball, 12, 12);
    lv_obj_set_style_radius(ball, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ball, 1, 0);
    lv_obj_set_style_border_color(ball, lv_color_hex(0x3A414B), 0);
    lv_obj_set_style_bg_color(ball, lv_color_hex(0x20242B), 0);
    lv_obj_set_style_bg_opa(ball, LV_OPA_COVER, 0);
    lv_obj_t *core = lz_dot(ball, 6, LZ_MINT);
    lv_obj_center(core);
    lz_text(pill, "Click trackball or press Enter", LZ_F_SMALL, lv_color_hex(0xCFD4DA));
    lv_obj_align(pill, LV_ALIGN_CENTER, 0, 36);

    /* new-message notification: tap to open it (clears unread on view) */
    g_notif_t = lz_svc_top_unread();
    if(g_notif_t) {
        lv_obj_t *card = lz_box(root);
        lv_obj_set_size(card, 268, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x171C24), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x2A313B), 0);
        lv_obj_set_style_pad_all(card, 9, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 2, 0);
        lv_obj_align(card, LV_ALIGN_CENTER, 0, 74);

        lv_obj_t *hrow = lz_box(card);
        lv_obj_set_width(hrow, lv_pct(100));
        lv_obj_set_height(hrow, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(hrow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(hrow, 6, 0);
        lz_icon(hrow, LZ_I_FORUM, &lz_icons_14, LZ_MINT);
        lv_obj_t *nm = lz_text(hrow, g_notif_t->name, LZ_F_BODY, LZ_TEXT);
        lv_obj_set_flex_grow(nm, 1);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        if(g_notif_t->unread > 1) {
            char cnt[8]; snprintf(cnt, sizeof cnt, "%d", g_notif_t->unread);
            lz_text(hrow, cnt, LZ_F_SMALL, LZ_MINT);
        }
        lv_obj_t *snip = lz_text(card, g_notif_t->last_text, LZ_F_SMALL, lv_color_hex(0xCFD4DA));
        lv_obj_set_width(snip, lv_pct(100));
        lv_label_set_long_mode(snip, LV_LABEL_LONG_DOT);
        lz_on_click(card, notif_tap);
    }

    lv_obj_t *foot = lz_text(root, "LimitlezzOS  Alpha 0.41", LZ_F_SMALL, lv_color_hex(0x5A616A));
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -10);

    lz_nav_set(1, 1, NULL);  /* Enter unlocks (handled in engine) */
}

/* ===== Home ===== */

/* apps not usable in this beta are grayed out and inert */
static bool app_enabled(const char *id)
{
    if(strcmp(id, "meshcore") == 0) return LZ_MESHCORE_ENABLED;
    if(strcmp(id, "appstore") == 0) return false;   /* not wired up yet */
    return true;
}

static void home_activate(int idx)
{
    static const lz_view_t views[8] = {
        LZ_V_MESSAGES, LZ_V_MESHTASTIC, LZ_V_MESHCORE, LZ_V_CONTACTS,
        LZ_V_APPSTORE, LZ_V_TERMINAL, LZ_V_FILES, LZ_V_SETTINGS,
    };
    if(idx >= 0 && idx < 8 && app_enabled(LZ_APPS[idx].id)) lz_go(views[idx]);
}

static bool home_disabled(int idx)
{
    return idx >= 0 && idx < 8 && !app_enabled(LZ_APPS[idx].id);
}

void lz_scr_home(lv_obj_t *root)
{
    lz_status_bar(root);

    /* 4-column grid: padding 11px 12px, cell 71px + 4px gap, row gap 9 */
    for(int i = 0; i < 8; i++) {
        const lz_app_t *a = &LZ_APPS[i];
        bool foc = (i == S.focus);
        bool en = app_enabled(a->id);
        int col = i % 4, row = i / 4;
        /* center the 4x2 grid in the area below the status bar */
        int cell_x = 12 + col * 75;
        int avail = LZ_H - LZ_STATUSBAR_H;
        int grid_h = 2 * 46 + 2 * 14 + 1 * 18;   /* tiles + labels + row gap */
        int top = LZ_STATUSBAR_H + (avail - grid_h) / 2;
        int cell_y = top + row * (46 + 14 + 18);

        lv_obj_t *tile = lz_box(root);
        lv_obj_set_size(tile, 46, 46);
        lv_obj_set_pos(tile, cell_x + (71 - 46) / 2, cell_y);
        lv_obj_set_style_radius(tile, LZ_RADIUS_TILE, 0);
        lv_obj_set_style_bg_color(tile, en ? lz_tile_color(a->hue) : lv_color_hex(0x23272E), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        if(foc) {
            lv_obj_set_style_outline_width(tile, LZ_FOCUS_RING_W, 0);
            lv_obj_set_style_outline_color(tile, LZ_FOCUS, 0);
            lv_obj_set_style_outline_pad(tile, 1, 0);
        }
        lv_obj_t *ic = lz_icon(tile, a->icon, &lz_icons_24,
                               en ? lv_color_white() : lv_color_hex(0x5A616A));
        lv_obj_center(ic);

        lv_obj_t *lbl = lz_text(root, a->name, LZ_F_SMALL,
                                !en ? lv_color_hex(0x5A616A)
                                    : foc ? LZ_TEXT_BRIGHT : LZ_TEXT_DIMLBL);
        lv_obj_update_layout(lbl);
        lv_obj_set_pos(lbl, cell_x + (71 - lv_obj_get_width(lbl)) / 2, cell_y + 46 + 4);

        /* "SOON" badge: a small pill in the tile's top-right corner, clearly
         * separated from the glyph and the app-name label below */
        if(!en) {
            lv_obj_t *soon = lz_box(tile);
            lv_obj_set_size(soon, LV_SIZE_CONTENT, 11);
            lv_obj_set_style_radius(soon, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(soon, lv_color_hex(0x0C0E12), 0);
            lv_obj_set_style_bg_opa(soon, LV_OPA_COVER, 0);
            lv_obj_set_style_pad_hor(soon, 3, 0);
            lv_obj_t *st = lz_text(soon, "SOON", LZ_F_SMALL, lv_color_hex(0x969EA8));
            lv_obj_center(st);
            lv_obj_align(soon, LV_ALIGN_TOP_RIGHT, 3, -4);
        }

        lz_nav_track(tile, i);
    }

    lz_nav_set(4, 8, home_activate);
    lz_nav_set_skip(home_disabled);
}
