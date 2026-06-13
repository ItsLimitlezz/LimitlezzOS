/* Wi-Fi setup: enable toggle, scanned network list, and password login.
 * Settings > Connectivity > Wi-Fi opens this. Drives the App Store / OTA
 * connectivity later. */
#include "../ui.h"
#include <stdio.h>
#include <string.h>

static void wifi_toggle(void) { lz_wifi_set_enabled(!lz_wifi_enabled()); S.focus = 0; lz_rebuild(); }
static void wifi_connect_tap(void)
{
    lz_wifi_connect(S.wifi_pw_ssid, S.draft);
    S.wifi_pw_mode = false; S.draft[0] = 0; S.focus = 0; lz_rebuild();
}
static void wifi_cancel_tap(void) { S.wifi_pw_mode = false; S.draft[0] = 0; lz_rebuild(); }

/* focus 0 = enable toggle; focus 1.. = network rows */
static void wifi_activate(int idx)
{
    if(idx == 0) { wifi_toggle(); return; }
    const lz_wifi_net *nets;
    int n = lz_wifi_results(&nets);
    int ni = idx - 1;
    if(ni < 0 || ni >= n) return;
    const char *ssid = nets[ni].ssid;
    const char *cur = lz_wifi_connected();
    if(cur && strcmp(cur, ssid) == 0) { lz_wifi_forget(); lz_rebuild(); return; }  /* disconnect */
    if(nets[ni].secure) {
        S.wifi_pw_mode = true;
        snprintf(S.wifi_pw_ssid, sizeof S.wifi_pw_ssid, "%s", ssid);
        S.draft[0] = 0;
        lz_rebuild();
    } else {
        lz_wifi_connect(ssid, "");
        lz_rebuild();
    }
}

static void signal_bars(lv_obj_t *parent, int rssi)
{
    int strength = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : 1;
    lv_obj_t *box = lz_box(parent);
    lv_obj_set_size(box, 4 * 3 + 3 * 2, 11);
    static const int hs[4] = { 4, 6, 8, 11 };
    for(int i = 0; i < 4; i++) {
        lv_obj_t *b = lz_box(box);
        lv_obj_set_size(b, 3, hs[i]);
        lv_obj_set_style_radius(b, 1, 0);
        lv_obj_set_style_bg_color(b, i < strength ? lv_color_hex(0x5FE3B3) : lv_color_hex(0x3D434B), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, i * 5, 0);
    }
}

void lz_scr_wifi(lv_obj_t *root)
{
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lz_navbar(root, "Wi-Fi", NULL);

    /* password entry mode replaces the list */
    if(S.wifi_pw_mode) {
        lv_obj_t *body = lz_vflex(root);
        lv_obj_set_style_pad_all(body, 14, 0);
        lv_obj_set_style_pad_row(body, 9, 0);
        lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

        char title[48]; snprintf(title, sizeof title, "Join \"%s\"", S.wifi_pw_ssid);
        lv_obj_t *t = lz_text(body, title, LZ_F_HEAD, LZ_TEXT);
        lv_obj_set_width(t, lv_pct(100));
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);

        /* password field (masked) */
        lv_obj_t *field = lz_box(body);
        lv_obj_set_size(field, 260, 34);
        lv_obj_set_style_radius(field, 9, 0);
        lv_obj_set_style_bg_color(field, lv_color_hex(0x191D24), 0);
        lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(field, 2, 0);
        lv_obj_set_style_border_color(field, LZ_MINT, 0);
        lv_obj_set_flex_flow(field, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(field, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(field, 2, 0);
        char mask[40]; size_t pl = strlen(S.draft); if(pl > 32) pl = 32;
        for(size_t i = 0; i < pl; i++) mask[i] = '*';
        mask[pl] = 0;
        bool empty = !S.draft[0];
        lz_text(field, empty ? "Password" : mask, LZ_F_TITLE,
                empty ? lv_color_hex(0x5E656E) : LZ_TEXT);
        if(!empty) {
            lv_obj_t *cur = lz_box(field);
            lv_obj_set_size(cur, 2, 18);
            lv_obj_set_style_bg_color(cur, LZ_MINT, 0);
            lv_obj_set_style_bg_opa(cur, LV_OPA_COVER, 0);
        }

        lv_obj_t *hintl = lz_text(body, "Type the password, then press Enter",
                                  LZ_F_SMALL, lv_color_hex(0x6F7882));
        lv_obj_set_width(hintl, lv_pct(100));
        lv_obj_set_style_text_align(hintl, LV_TEXT_ALIGN_CENTER, 0);

        /* Connect / Cancel buttons (tappable) */
        lv_obj_t *btns = lz_box(body);
        lv_obj_set_width(btns, lv_pct(100));
        lv_obj_set_height(btns, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(btns, 8, 0);
        lv_obj_t *cancel = lz_box(btns);
        lv_obj_set_size(cancel, 90, 30);
        lv_obj_set_style_radius(cancel, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(cancel, lv_color_hex(0x20242B), 0);
        lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cancel, 1, 0);
        lv_obj_set_style_border_color(cancel, lv_color_hex(0x2D323A), 0);
        lv_obj_t *cl = lz_text(cancel, "Cancel", LZ_F_BODY, lv_color_hex(0xCFD4DA));
        lv_obj_center(cl);
        lz_on_click(cancel, wifi_cancel_tap);
        lv_obj_t *conn = lz_box(btns);
        lv_obj_set_size(conn, 110, 30);
        lv_obj_set_style_radius(conn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(conn, LZ_TILE_165, 0);
        lv_obj_set_style_bg_opa(conn, LV_OPA_COVER, 0);
        lv_obj_t *cn = lz_text(conn, "Connect", LZ_F_BODY, LZ_ON_MINT);
        lv_obj_center(cn);
        lz_on_click(conn, wifi_connect_tap);

        lz_nav_set(1, 0, NULL);     /* keys handled by wifi_pw_key */
        return;
    }

    lv_obj_t *body = lz_vflex(root);
    lv_obj_set_style_pad_top(body, 6, 0);
    lv_obj_set_style_pad_hor(body, 8, 0);
    lv_obj_set_style_pad_bottom(body, 8, 0);
    lv_obj_set_style_pad_row(body, 4, 0);
    lz_nav_set_scroll(body);

    bool on = lz_wifi_enabled();

    /* enable toggle row (focus 0) */
    lv_obj_t *trow = lz_row(body, S.focus == 0);
    lv_obj_t *ic = lz_icon(trow, LZ_I_WIFI, &lz_icons_18, on ? LZ_MINT : lv_color_hex(0x868F99));
    (void)ic;
    lv_obj_t *tl = lz_text(trow, "Wi-Fi", LZ_F_BODY, LZ_TEXT);
    lv_obj_set_flex_grow(tl, 1);
    lz_toggle(trow, on, LZ_TOGGLE_ON);
    lz_nav_track(trow, 0);

    if(!on) {
        lv_obj_t *h = lz_text(body, "Turn on Wi-Fi to find networks.",
                              LZ_F_SMALL, LZ_TEXT_3);
        lv_obj_set_style_pad_top(h, 10, 0);
        lz_nav_set(1, 1, wifi_activate);
        return;
    }

    /* status line */
    int st = lz_wifi_status();
    const char *status = st == LZ_WIFI_SCANNING   ? "Scanning..."
                       : st == LZ_WIFI_CONNECTING  ? "Connecting..."
                       : st == LZ_WIFI_FAILED       ? "Couldn't connect - check the password"
                       : lz_wifi_connected()        ? NULL
                                                     : "Select a network";
    if(status) {
        lv_obj_t *s = lz_text(body, status, LZ_F_SMALL,
                              st == LZ_WIFI_FAILED ? LZ_SNR_BAD : LZ_TEXT_3);
        lv_obj_set_style_pad_left(s, 4, 0);
    }

    const lz_wifi_net *nets;
    int n = lz_wifi_results(&nets);
    const char *cur = lz_wifi_connected();
    for(int i = 0; i < n; i++) {
        lv_obj_t *row = lz_row(body, S.focus == i + 1);
        bool isconn = cur && strcmp(cur, nets[i].ssid) == 0;

        lv_obj_t *nm = lz_text(row, nets[i].ssid, LZ_F_BODY, LZ_TEXT);
        lv_obj_set_flex_grow(nm, 1);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);

        if(isconn) lz_text(row, "Connected", LZ_F_SMALL, LZ_GREEN_TXT);
        if(nets[i].secure) lz_icon(row, LZ_I_LOCK, &lz_icons_14, lv_color_hex(0x868F99));
        signal_bars(row, nets[i].rssi);
        lz_nav_track(row, i + 1);
    }

    lz_nav_set(1, n + 1, wifi_activate);
}
