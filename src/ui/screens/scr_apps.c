/* App Store (install flow), Contacts directory, Contact detail,
 * Terminal, Files */
#include "../ui.h"
#include <stdio.h>
#include <string.h>

/* ===== App Store ===== */

static void store_timer_cb(lv_timer_t *tm)
{
    int idx = (int)(intptr_t)tm->user_data;
    LZ_STORE[idx].state = LZ_ST_OPEN;
    if(S.view == LZ_V_APPSTORE) lz_rebuild();
}

static void store_activate(int idx)
{
    if(idx < 0 || idx >= 8) return;
    if(LZ_STORE[idx].state == LZ_ST_OPEN || LZ_STORE[idx].state == LZ_ST_INSTALLING) return;
    LZ_STORE[idx].state = LZ_ST_INSTALLING;
    lv_timer_t *tm = lv_timer_create(store_timer_cb, 1100, (void *)(intptr_t)idx);
    lv_timer_set_repeat_count(tm, 1);
    lz_rebuild();
}

void lz_scr_appstore(lv_obj_t *root)
{
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *bar = lz_navbar(root, "App Store", NULL);
    lv_obj_t *srch = lz_icon(bar, LZ_I_SEARCH, &lz_icons_14, LZ_TEXT_DIMLBL);
    lv_obj_align(srch, LV_ALIGN_RIGHT_MID, -9, 0);

    lv_obj_t *body = lz_vflex(root);
    lv_obj_set_style_pad_top(body, 8, 0);
    lv_obj_set_style_pad_hor(body, 8, 0);
    lv_obj_set_style_pad_bottom(body, 10, 0);
    lv_obj_set_style_pad_row(body, 3, 0);
    lz_nav_set_scroll(body);

    /* featured card (flattened to solid fill per rendering constraints) */
    lv_obj_t *feat = lz_box(body);
    lv_obj_set_width(feat, lv_pct(100));
    lv_obj_set_height(feat, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(feat, 13, 0);
    lv_obj_set_style_bg_color(feat, LZ_FEATURED, 0);
    lv_obj_set_style_bg_opa(feat, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(feat, 11, 0);
    lv_obj_set_flex_flow(feat, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(feat, 7, 0);
    lz_text(feat, "FEATURED", LZ_F_SMALL, lv_color_hex(0xC9CBE8));
    lv_obj_t *frow = lz_box(feat);
    lv_obj_set_width(frow, lv_pct(100));
    lv_obj_set_height(frow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(frow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(frow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(frow, 10, 0);
    lv_obj_t *ftile = lz_box(frow);
    lv_obj_set_size(ftile, 42, 42);
    lv_obj_set_style_radius(ftile, 11, 0);
    lv_obj_set_style_bg_color(ftile, lz_tile_color(150), 0);
    lv_obj_set_style_bg_opa(ftile, LV_OPA_COVER, 0);
    lv_obj_t *fic = lz_icon(ftile, LZ_I_MAP, &lz_icons_24, lv_color_white());
    lv_obj_center(fic);
    lv_obj_t *fcol = lz_box(frow);
    lv_obj_set_flex_grow(fcol, 1);
    lv_obj_set_height(fcol, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(fcol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(fcol, 1, 0);
    lz_text(fcol, "Node Mapper", LZ_F_HEAD, lv_color_white());
    lv_obj_t *fd = lz_text(fcol, "Live mesh topology & GPS positions on an offline map",
                           LZ_F_SMALL, lv_color_hex(0xCFD0E4));
    lv_label_set_long_mode(fd, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(fd, lv_pct(100));

    lv_obj_t *hd = lz_text(body, "Apps & utilities", LZ_F_BODY, lv_color_hex(0xCFD4DA));
    lv_obj_set_style_pad_bottom(hd, 3, 0);

    for(int i = 0; i < 8; i++) {
        lz_store_app_t *a = &LZ_STORE[i];
        lv_obj_t *row = lz_row(body, i == S.focus);
        lv_obj_set_style_radius(row, 11, 0);

        lv_obj_t *tile = lz_box(row);
        lv_obj_set_size(tile, 36, 36);
        lv_obj_set_style_radius(tile, 10, 0);
        lv_obj_set_style_bg_color(tile, lz_tile_color(a->hue), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_t *ic = lz_icon(tile, a->icon, &lz_icons_18, lv_color_white());
        lv_obj_center(ic);

        lv_obj_t *cl = lz_box(row);
        lv_obj_set_flex_grow(cl, 1);
        lv_obj_set_height(cl, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cl, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(cl, 1, 0);
        lz_text(cl, a->name, LZ_F_BODY, LZ_TEXT);
        lv_obj_t *meta = lz_box(cl);
        lv_obj_set_size(meta, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(meta, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(meta, 4, 0);
        lz_icon(meta, LZ_I_STAR, &lz_icons_14, LZ_SNR_MID);
        lz_text(meta, a->rating, LZ_F_SMALL, LZ_TEXT_VALUE);
        char cs[32]; snprintf(cs, sizeof cs, "- %s - %s", a->cat, a->size);
        lz_text(meta, cs, LZ_F_SMALL, LZ_TEXT_3);

        const char *lbl = a->state == LZ_ST_INSTALLING ? "..."
                        : a->state == LZ_ST_OPEN       ? "OPEN"
                        : a->state == LZ_ST_UPDATE     ? "UPDATE" : "GET";
        bool open = a->state == LZ_ST_OPEN;
        lv_obj_t *btn = lz_box(row);
        lv_obj_set_size(btn, LV_SIZE_CONTENT, 21);
        lv_obj_set_style_min_width(btn, 52, 0);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(btn, open ? lv_color_hex(0x222A33) : LZ_STORE_BTN, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        if(open) {
            lv_obj_set_style_border_width(btn, 1, 0);
            lv_obj_set_style_border_color(btn, lv_color_hex(0x3A414B), 0);
        }
        lv_obj_set_style_pad_hor(btn, 11, 0);
        lv_obj_t *bl = lz_text(btn, lbl, LZ_F_SMALL,
                               open ? lv_color_hex(0xCFD4DA) : LZ_ON_MINT);
        lv_obj_center(bl);
        lz_nav_track(row, i);
    }
    lz_nav_set(1, 8, store_activate);
}

/* ===== Contacts ===== */

static void contacts_activate(int idx)
{
    if(idx >= 0 && idx < 9) {
        S.contact_sel = &LZ_NODES[idx];
        lz_go(LZ_V_CONTACT);
    }
}

void lz_scr_contacts(lv_obj_t *root)
{
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lz_navbar(root, "Contacts", NULL);

    lv_obj_t *body = lz_vflex(root);
    lv_obj_set_style_pad_top(body, 5, 0);
    lv_obj_set_style_pad_hor(body, 7, 0);
    lv_obj_set_style_pad_bottom(body, 8, 0);
    lv_obj_set_style_pad_row(body, 3, 0);
    lz_nav_set_scroll(body);

    for(int i = 0; i < 9; i++) {
        const lz_node_t *n = &LZ_NODES[i];
        lv_obj_t *row = lz_row(body, i == S.focus);
        lv_obj_set_style_radius(row, 10, 0);

        lv_obj_t *avwrap = lz_box(row);
        lv_obj_set_size(avwrap, 32, 32);
        lv_obj_t *av = lz_dot(avwrap, 31, lz_av_color(n->net));
        lv_obj_t *sc = lz_text(av, n->shortcode, LZ_F_SMALL, lv_color_white());
        lv_obj_center(sc);
        lv_obj_t *ring = lz_dot(avwrap, 11, LZ_SCREEN_BG);
        lv_obj_align(ring, LV_ALIGN_BOTTOM_RIGHT, 1, 1);
        lv_obj_t *nd = lz_dot(ring, 7, lz_net_color(n->net));
        lv_obj_center(nd);

        lv_obj_t *cl = lz_box(row);
        lv_obj_set_flex_grow(cl, 1);
        lv_obj_set_height(cl, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cl, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(cl, 1, 0);
        lz_text(cl, n->name, LZ_F_BODY, LZ_TEXT);
        char meta[40]; snprintf(meta, sizeof meta, "%s - %s", n->id, n->role);
        lz_text(cl, meta, LZ_F_SMALL, lv_color_hex(0x838A93));

        lv_obj_t *r = lz_box(row);
        lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        lz_text(r, n->last, LZ_F_SMALL, LZ_TEXT_META);
        lz_text(r, n->snr_s, LZ_F_SMALL, lz_snr_color(n->snr));
        lz_nav_track(row, i);
    }
    lz_nav_set(1, 9, contacts_activate);
}

/* ===== Contact detail ===== */

static void contact_activate(int idx)
{
    const lz_node_t *n = S.contact_sel ? S.contact_sel : &LZ_NODES[0];
    if(idx == 0) {
        /* Message: open the bound thread, or a fresh one on this contact's network */
        for(int i = 0; i < 6; i++) {
            if(strcmp(LZ_THREADS[i].name, n->name) == 0) {
                lz_open_convo(&LZ_THREADS[i]);
                return;
            }
        }
        static lz_thread_t tmp;
        tmp.id = n->id; tmp.name = n->name; tmp.net = n->net;
        tmp.addr = n->id; tmp.text = "-"; tmp.t = ""; tmp.unread = 0; tmp.path = n->dist;
        lz_open_convo(&tmp);
    }
    /* idx 1 = Trace: no-op in prototype */
}

void lz_scr_contact(lv_obj_t *root)
{
    const lz_node_t *n = S.contact_sel ? S.contact_sel : &LZ_NODES[0];
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lz_navbar(root, NULL, "Back");

    lv_obj_t *body = lz_vflex(root);
    lv_obj_set_style_pad_top(body, 10, 0);
    lv_obj_set_style_pad_hor(body, 12, 0);
    lv_obj_set_style_pad_bottom(body, 10, 0);
    lv_obj_set_style_pad_row(body, 0, 0);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lz_nav_set_scroll(body);

    lv_obj_t *av = lz_dot(body, 54, lz_av_color(n->net));
    lv_obj_t *sc = lz_text(av, n->shortcode, LZ_F_TITLE, lv_color_white());
    lv_obj_center(sc);

    lv_obj_t *name = lz_text(body, n->name, LZ_F_TITLE, LZ_TEXT);
    lv_obj_set_width(name, lv_pct(100));
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(name, 7, 0);

    lv_obj_t *tag = lz_box(body);
    lv_obj_set_width(tag, lv_pct(100));
    lv_obj_set_height(tag, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tag, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tag, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tag, 5, 0);
    lv_obj_set_style_pad_top(tag, 3, 0);
    lz_dot(tag, 6, lz_net_color(n->net));
    lz_text(tag, lz_net_name(n->net), LZ_F_SMALL, lz_net_color(n->net));
    char role[24]; snprintf(role, sizeof role, "- %s", n->role);
    lz_text(tag, role, LZ_F_SMALL, LZ_TEXT_META);

    /* actions: Message (mint) + Trace */
    lv_obj_t *actions = lz_box(body);
    lv_obj_set_width(actions, lv_pct(100));
    lv_obj_set_height(actions, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(actions, 7, 0);
    lv_obj_set_style_pad_ver(actions, 12, 0);

    bool f0 = S.focus == 0, f1 = S.focus == 1;
    lv_obj_t *msg = lz_box(actions);
    lv_obj_set_flex_grow(msg, 1);
    lv_obj_set_height(msg, 31);
    lv_obj_set_style_radius(msg, 10, 0);
    lv_obj_set_style_bg_color(msg, LZ_TILE_165, 0);
    lv_obj_set_style_bg_opa(msg, LV_OPA_COVER, 0);
    if(f0) {
        lv_obj_set_style_outline_width(msg, 2, 0);
        lv_obj_set_style_outline_color(msg, LZ_FOCUS, 0);
    }
    lv_obj_set_flex_flow(msg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(msg, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(msg, 5, 0);
    lz_icon(msg, LZ_I_CHAT, &lz_icons_16f, LZ_ON_MINT);
    lz_text(msg, "Message", LZ_F_BODY, LZ_ON_MINT);

    lv_obj_t *trace = lz_box(actions);
    lv_obj_set_size(trace, 41, 31);
    lv_obj_set_style_radius(trace, 10, 0);
    lv_obj_set_style_bg_color(trace, lv_color_hex(0x20242B), 0);
    lv_obj_set_style_bg_opa(trace, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(trace, 1, 0);
    lv_obj_set_style_border_color(trace, lv_color_hex(0x2D323A), 0);
    if(f1) {
        lv_obj_set_style_outline_width(trace, 2, 0);
        lv_obj_set_style_outline_color(trace, LZ_FOCUS, 0);
    }
    lv_obj_t *tic = lz_icon(trace, LZ_I_ROUTE, &lz_icons_16f, lv_color_hex(0xCFD4DA));
    lv_obj_center(tic);

    lz_nav_track(msg, 0);     /* also makes both buttons tappable */
    lz_nav_track(trace, 1);

    /* spec table */
    lv_obj_t *card = lz_card(body);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    char batt[8];
    if(n->batt >= 0) snprintf(batt, sizeof batt, "%d%%", n->batt);
    else snprintf(batt, sizeof batt, "-");
    const char *ks[6] = { "Node ID", "Hardware", "Distance", "SNR", "Battery", "Last heard" };
    const char *vs[6] = { n->id, n->hw, n->dist, n->snr_s, batt, n->last };
    for(int i = 0; i < 6; i++) {
        lv_obj_t *r = lz_box(card);
        lv_obj_set_width(r, lv_pct(100));
        lv_obj_set_height(r, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(r, 11, 0);
        lv_obj_set_style_pad_ver(r, 7, 0);
        if(i < 5) {
            lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(r, 1, 0);
            lv_obj_set_style_border_color(r, lv_color_hex(0x21262D), 0);
        }
        lz_text(r, ks[i], LZ_F_SMALL, lv_color_hex(0x8B939C));
        lz_text(r, vs[i], LZ_F_SMALL, LZ_TEXT_STRONG);
    }

    lz_nav_set(2, 2, contact_activate);
}

/* ===== Terminal ===== */

static void blink_cb(lv_timer_t *tm)
{
    lv_obj_t *cur = tm->user_data;
    lv_obj_set_style_opa(cur, lv_obj_get_style_opa(cur, 0) == LV_OPA_COVER
                              ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
}

static void blink_del_cb(lv_event_t *e)
{
    lv_timer_del(lv_event_get_user_data(e));
}

void lz_scr_terminal(lv_obj_t *root)
{
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(root, LZ_TERM_BG, 0);

    lv_obj_t *bar = lz_box(root);
    lv_obj_set_size(bar, LZ_W, LZ_NAVBAR_H);
    lv_obj_set_style_bg_color(bar, LZ_TERM_BAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x0C0F14), 0);
    lv_obj_t *chev = lz_icon(bar, LZ_I_CHEV_L, &lz_icons_18, lv_color_hex(0xCFD4DA));
    lv_obj_align(chev, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_t *backhit = lz_box(bar);
    lv_obj_set_size(backhit, 64, LZ_NAVBAR_H);
    lv_obj_set_pos(backhit, 0, 0);
    lz_on_click(backhit, lz_back);
    lv_obj_t *t = lz_text(bar, "Terminal", LZ_F_HEAD, lv_color_hex(0xCFD4DA));
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *baud = lz_text(bar, "115200", LZ_F_MONO, lv_color_hex(0x5F6A5F));
    lv_obj_align(baud, LV_ALIGN_RIGHT_MID, -8, 0);

    lv_obj_t *body = lz_vflex(root);
    lv_obj_set_style_pad_all(body, 8, 0);
    lv_obj_set_style_pad_row(body, 4, 0);
    lz_nav_set_scroll(body);

    for(int i = 0; i < 12; i++) {
        lv_color_t c = LZ_TERM_KIND[i] == 0 ? LZ_TERM_DIM
                     : LZ_TERM_KIND[i] == 1 ? LZ_TERM_CMD : LZ_TERM_GREEN;
        lz_text(body, LZ_TERM_LINES[i], LZ_F_MONO, c);
    }

    /* prompt + blinking block cursor */
    lv_obj_t *prompt = lz_box(body);
    lv_obj_set_width(prompt, lv_pct(100));
    lv_obj_set_height(prompt, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(prompt, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(prompt, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(prompt, 2, 0);
    lz_text(prompt, "limitlezz:~$ ", LZ_F_MONO, LZ_TERM_GREEN);
    lv_obj_t *cur = lz_box(prompt);
    lv_obj_set_size(cur, 6, 12);
    lv_obj_set_style_bg_color(cur, LZ_TERM_GREEN, 0);
    lv_obj_set_style_bg_opa(cur, LV_OPA_COVER, 0);
    lv_timer_t *tm = lv_timer_create(blink_cb, 530, cur);
    lv_obj_add_event_cb(cur, blink_del_cb, LV_EVENT_DELETE, tm);

    lz_nav_set(1, 0, NULL);  /* trackball scrolls */
}

/* ===== Files ===== */

void lz_scr_files(lv_obj_t *root)
{
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lz_navbar(root, "Files", NULL);

    lv_obj_t *path = lz_box(root);
    lv_obj_set_size(path, LZ_W, 19);
    lv_obj_set_style_border_side(path, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(path, 1, 0);
    lv_obj_set_style_border_color(path, lv_color_hex(0x171B21), 0);
    lv_obj_t *pl = lz_text(path, "/sdcard", LZ_F_MONO, lv_color_hex(0x7F868F));
    lv_obj_align(pl, LV_ALIGN_LEFT_MID, 11, 0);

    lv_obj_t *body = lz_vflex(root);
    lv_obj_set_style_pad_top(body, 5, 0);
    lv_obj_set_style_pad_hor(body, 7, 0);
    lv_obj_set_style_pad_bottom(body, 8, 0);
    lv_obj_set_style_pad_row(body, 2, 0);
    lz_nav_set_scroll(body);

    for(int i = 0; i < 7; i++) {
        const lz_file_t *f = &LZ_FILES[i];
        lv_obj_t *row = lz_row(body, i == S.focus);
        lz_icon(row, f->dir ? LZ_I_FOLDER : LZ_I_DESCRIPTION, &lz_icons_18,
                f->dir ? LZ_FILE_FOLDER : lv_color_hex(0x8B939C));
        lv_obj_t *name = lz_text(row, f->name, LZ_F_BODY, LZ_TEXT_SETTING);
        lv_obj_set_flex_grow(name, 1);
        lz_text(row, f->meta, LZ_F_MONO, LZ_TEXT_META);
        lz_nav_track(row, i);
    }
    lz_nav_set(1, 7, NULL);
}
