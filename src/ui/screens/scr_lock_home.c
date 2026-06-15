/* Lock screen + Home launcher (paged iOS-style 4x2 grid per design) */
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

    /* Full-screen "tap to unlock" layer as the BOTTOM child (a fresh object
     * each rebuild — avoids piling duplicate handlers on the persistent screen
     * object). Everything below is drawn on top of it, so the notification card
     * captures its own taps while a tap anywhere else falls through to unlock. */
    lv_obj_t *unlock_layer = lz_box(root);
    lv_obj_set_size(unlock_layer, LZ_W, LZ_H);
    lv_obj_set_pos(unlock_layer, 0, 0);
    lz_on_click(unlock_layer, lock_tap);

    /* top inset row: just the enabled-network icons (left), battery (right) —
     * Meshtastic (cyan hub) and/or MeshCore (amber lan), or nothing if idle */
    int ix = 13;
    if(S.net_mt) {
        lv_obj_t *mt = lz_icon(root, LZ_I_HUB, &lz_icons_14, LZ_CYAN);
        lv_obj_align(mt, LV_ALIGN_TOP_LEFT, ix, 12);
        ix += 20;
    }
    if(S.net_mc) {
        lv_obj_t *mc = lz_icon(root, LZ_I_LAN, &lz_icons_14, LZ_AMBER);
        lv_obj_align(mc, LV_ALIGN_TOP_LEFT, ix, 12);
    }

    lz_sysinfo_t si; lz_svc_sysinfo(&si);
    if(si.battery_pct >= 0) {
        char pb[8]; snprintf(pb, sizeof pb, "%d%%", si.battery_pct);
        lv_obj_t *pct = lz_text(root, pb, LZ_F_SMALL, lv_color_hex(0xAEB6BF));
        lv_obj_align(pct, LV_ALIGN_TOP_RIGHT, -42, 13);
    }
    lv_obj_t *bw = lz_battery_glyph(root);   /* shared iPhone-style battery */
    lv_obj_align(bw, LV_ALIGN_TOP_RIGHT, -11, 13);

    /* clock + date (real time once synced, otherwise --:-- + a hint) */
    char clk[12]; lz_fmt_now(clk, sizeof clk);
    lv_obj_t *clock = lz_text(root, clk, LZ_F_CLOCK, LZ_TEXT);
    lv_obj_align(clock, LV_ALIGN_CENTER, 0, -38);
    char dbuf[28]; lz_fmt_date(dbuf, sizeof dbuf);
    lv_obj_t *date = lz_text(root, dbuf, LZ_F_BODY, LZ_TEXT_VALUE);
    lv_obj_align(date, LV_ALIGN_CENTER, 0, -4);

    /* When there's a new message, the notification card becomes the focal
     * element (centered) and the trackball hint hides — so it reads like an
     * iPhone lock screen. With nothing waiting, the unlock hint is shown. */
    g_notif_t = lz_svc_top_unread();
    if(!g_notif_t) {
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
        lv_obj_align(pill, LV_ALIGN_CENTER, 0, 40);
    } else {
        /* centered new-message notification; tap to open it (clears unread on view) */
        lv_obj_t *card = lz_box(root);
        lv_obj_set_size(card, 272, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x171C24), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x2A313B), 0);
        lv_obj_set_style_pad_all(card, 10, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 3, 0);
        lv_obj_align(card, LV_ALIGN_CENTER, 0, 46);

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

        /* iPhone-style stack count: "+N more" beneath the newest notification */
        int more = lz_svc_unread_count() - 1;
        if(more > 0) {
            char mb[16]; snprintf(mb, sizeof mb, "+%d more", more);
            lv_obj_t *ml = lz_text(root, mb, LZ_F_SMALL, lv_color_hex(0x8A929C));
            lv_obj_align_to(ml, card, LV_ALIGN_OUT_BOTTOM_MID, 0, 7);
        }
    }

    lv_obj_t *foot = lz_text(root, "LimitlezzOS  Beta 0.6", LZ_F_SMALL, lv_color_hex(0x5A616A));
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -10);

    lz_nav_set(1, 1, NULL);  /* Enter unlocks (handled in engine) */
}

/* ===== Home ===== */

#define HOME_MAX_CELLS 8
#define HOME_COLS 4
#define HOME_LOCAL_MAX LZ_MAX_LOCAL_APPS

static int home_builtin_n;
static int home_local_n;
static int home_total_n;
static int home_page_count;

static bool app_visible(const char *id)
{
    return strcmp(id, "terminal") != 0 || S.settings.developer;
}

/* apps not usable in this beta are grayed out and inert */
static bool app_enabled(const char *id)
{
    if(strcmp(id, "meshcore") == 0) return LZ_MESHCORE_ENABLED;
    return true;
}

static void home_prepare(lz_local_app_t *local, int local_cap)
{
    home_builtin_n = 0;
    for(int i = 0; i < 8; i++)
        if(app_visible(LZ_APPS[i].id)) home_builtin_n++;

    home_local_n = local_cap > 0 ? lz_svc_scan_apps(local, local_cap) : 0;
    home_total_n = home_builtin_n + home_local_n;
    home_page_count = (home_total_n + HOME_MAX_CELLS - 1) / HOME_MAX_CELLS;
    if(home_page_count < 1) home_page_count = 1;
    if(S.home_page < 0) S.home_page = 0;
    if(S.home_page >= home_page_count) S.home_page = home_page_count - 1;
}

static int home_app_index(int visible_idx)
{
    int seen = 0;
    for(int i = 0; i < 8; i++) {
        if(!app_visible(LZ_APPS[i].id)) continue;
        if(seen == visible_idx) return i;
        seen++;
    }
    return -1;
}

static int home_local_index(int visible_idx)
{
    int idx = visible_idx - home_builtin_n;
    return (idx >= 0 && idx < home_local_n) ? idx : -1;
}

static int home_page_item_count(int page)
{
    int start = page * HOME_MAX_CELLS;
    int n = home_total_n - start;
    if(n < 0) n = 0;
    if(n > HOME_MAX_CELLS) n = HOME_MAX_CELLS;
    return n;
}

static int home_abs_index(int page_idx)
{
    return S.home_page * HOME_MAX_CELLS + page_idx;
}

static lz_view_t app_view(const char *id)
{
    if(strcmp(id, "messages") == 0) return LZ_V_MESSAGES;
    if(strcmp(id, "meshtastic") == 0) return LZ_V_MESHTASTIC;
    if(strcmp(id, "meshcore") == 0) return LZ_V_MESHCORE;
    if(strcmp(id, "contacts") == 0) return LZ_V_CONTACTS;
    if(strcmp(id, "appstore") == 0) return LZ_V_APPSTORE;
    if(strcmp(id, "terminal") == 0) return LZ_V_TERMINAL;
    if(strcmp(id, "files") == 0) return LZ_V_FILES;
    return LZ_V_SETTINGS;
}

static void home_activate(int idx)
{
    int abs_idx = home_abs_index(idx);
    int local_idx = home_local_index(abs_idx);
    if(local_idx >= 0) {
        lz_local_app_t local[HOME_LOCAL_MAX];
        int n = lz_svc_scan_apps(local, HOME_LOCAL_MAX);
        if(local_idx < n) {
            S.local_app_sel = local[local_idx];
            lz_go(LZ_V_LOCALAPP);
        }
        return;
    }

    int app_idx = home_app_index(abs_idx);
    if(app_idx >= 0 && app_enabled(LZ_APPS[app_idx].id))
        lz_go(app_view(LZ_APPS[app_idx].id));
}

static bool home_disabled(int idx)
{
    int abs_idx = home_abs_index(idx);
    if(home_local_index(abs_idx) >= 0) return false;
    int app_idx = home_app_index(abs_idx);
    return app_idx >= 0 && !app_enabled(LZ_APPS[app_idx].id);
}

bool lz_home_page_key(lz_key_t dir)
{
    int count = home_page_item_count(S.home_page);
    if(count <= 0 || home_page_count <= 1) return false;

    int row = S.focus / HOME_COLS;
    int col = S.focus % HOME_COLS;
    if(dir == LZ_K_RIGHT && S.home_page + 1 < home_page_count &&
       (col == HOME_COLS - 1 || S.focus == count - 1)) {
        S.home_page++;
        int next_count = home_page_item_count(S.home_page);
        int nf = row * HOME_COLS;
        if(nf >= next_count) nf = next_count > 0 ? next_count - 1 : 0;
        S.focus = nf;
        lz_rebuild();
        return true;
    }
    if(dir == LZ_K_LEFT && S.home_page > 0 && col == 0) {
        S.home_page--;
        int prev_count = home_page_item_count(S.home_page);
        int nf = row * HOME_COLS + (HOME_COLS - 1);
        if(nf >= prev_count) nf = prev_count > 0 ? prev_count - 1 : 0;
        S.focus = nf;
        lz_rebuild();
        return true;
    }
    return false;
}

void lz_scr_home(lv_obj_t *root)
{
    lz_status_bar(root);
    lz_local_app_t local[HOME_LOCAL_MAX];
    home_prepare(local, HOME_LOCAL_MAX);

    int page_start = S.home_page * HOME_MAX_CELLS;
    int count = home_page_item_count(S.home_page);
    if(S.focus >= count) S.focus = count > 0 ? count - 1 : 0;

    /* 4-column grid: padding 11px 12px, cell 71px + 4px gap, row gap 9 */
    for(int i = 0; i < count; i++) {
        int abs_idx = page_start + i;
        int app_idx = home_app_index(abs_idx);
        int local_idx = home_local_index(abs_idx);
        const lz_app_t *a = app_idx >= 0 ? &LZ_APPS[app_idx] : NULL;
        const lz_local_app_t *la = local_idx >= 0 ? &local[local_idx] : NULL;
        const char *id = a ? a->id : la->id;
        const char *name = a ? a->name : la->name;
        const char *icon = a ? a->icon : lz_app_icon_glyph(la->icon);
        int hue = a ? a->hue : la->hue;
        bool foc = (i == S.focus);
        bool en = a ? app_enabled(a->id) : true;
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
        lv_obj_set_style_bg_color(tile, en ? lz_tile_color(hue) : lv_color_hex(0x23272E), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        if(foc) {
            lv_obj_set_style_outline_width(tile, LZ_FOCUS_RING_W, 0);
            lv_obj_set_style_outline_color(tile, LZ_FOCUS, 0);
            lv_obj_set_style_outline_pad(tile, 1, 0);
        }
        lv_obj_t *ic = lz_icon(tile, icon, a ? &lz_icons_24 : &lz_icons_18,
                               en ? lv_color_white() : lv_color_hex(0x5A616A));
        lv_obj_center(ic);

        lv_obj_t *lbl = lz_text(root, name, LZ_F_SMALL,
                                !en ? lv_color_hex(0x5A616A)
                                    : foc ? LZ_TEXT_BRIGHT : LZ_TEXT_DIMLBL);
        lv_obj_set_width(lbl, 71);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(lbl, cell_x, cell_y + 46 + 4);

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

        /* iPhone-style unread counter badge on the Messages icon (muted chats
         * excluded): 1-9 as the number, "9+" once there are ten or more */
        if(strcmp(id, "messages") == 0) {
            int u = lz_svc_unread_total();
            if(u > 0) {
                lv_obj_t *bdg = lz_box(tile);
                lv_obj_set_size(bdg, LV_SIZE_CONTENT, 16);
                lv_obj_set_style_min_width(bdg, 16, 0);
                lv_obj_set_style_radius(bdg, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_bg_color(bdg, lv_color_hex(0xFF3B30), 0);
                lv_obj_set_style_bg_opa(bdg, LV_OPA_COVER, 0);
                lv_obj_set_style_pad_hor(bdg, 4, 0);
                lv_obj_set_style_border_width(bdg, 2, 0);
                lv_obj_set_style_border_color(bdg, LZ_SCREEN_BG, 0);  /* ring it off the tile */
                char ub[6];
                if(u <= 9) snprintf(ub, sizeof ub, "%d", u);
                else       snprintf(ub, sizeof ub, "9+");
                lv_obj_t *ul = lz_text(bdg, ub, LZ_F_SMALL, lv_color_white());
                lv_obj_center(ul);
                lv_obj_align(bdg, LV_ALIGN_TOP_RIGHT, 7, -6);
            }
        }

        lz_nav_track(tile, i);
    }

    if(home_page_count > 1) {
        const int dot = 5, gap = 5;
        int total_w = home_page_count * dot + (home_page_count - 1) * gap;
        int x = (LZ_W - total_w) / 2;
        for(int p = 0; p < home_page_count; p++) {
            lv_obj_t *d = lz_dot(root, dot, p == S.home_page ? LZ_TEXT_BRIGHT : lv_color_hex(0x4B535C));
            lv_obj_set_pos(d, x + p * (dot + gap), LZ_H - 15);
        }
    }

    lz_nav_set(HOME_COLS, count, home_activate);
    lz_nav_set_skip(home_disabled);
}
