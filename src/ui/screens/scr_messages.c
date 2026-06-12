/* Messages (unified inbox: Direct/Channels tabs, network filter chips)
 * + Conversation (network-bound thread with tagged compose bar) */
#include "../ui.h"
#include <stdio.h>
#include <string.h>

static const lz_thread_t *vis_threads[6];
static int vis_thread_count;

static bool net_on(lz_net_t n) { return n == LZ_NET_MT ? S.net_mt : S.net_mc; }

static bool filter_match(lz_net_t n)
{
    return S.msg_filter == LZ_FILT_ALL ||
           (S.msg_filter == LZ_FILT_MT && n == LZ_NET_MT) ||
           (S.msg_filter == LZ_FILT_MC && n == LZ_NET_MC);
}

void lz_open_convo(const lz_thread_t *t)
{
    S.convo = t;
    S.sent_count = 0;
    S.draft[0] = 0;
    lz_go(LZ_V_CONVO);
}

static void messages_activate(int idx)
{
    if(S.msg_tab == LZ_TAB_DMS && idx < vis_thread_count)
        lz_open_convo(vis_threads[idx]);
    /* channel rows: no-op (matches prototype) */
}

static lv_obj_t *filter_chip(lv_obj_t *parent, const char *label,
                             bool active, lv_color_t dot, bool has_dot,
                             lv_color_t act_bg, lv_color_t act_fg)
{
    lv_obj_t *chip = lz_box(parent);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, 17);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(chip, active ? act_bg : LZ_CHIP_BG, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(chip, 8, 0);
    lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(chip, 4, 0);
    if(has_dot) lz_dot(chip, 6, dot);
    lz_text(chip, label, LZ_F_SMALL, active ? act_fg : LZ_TEXT_DIMLBL);
    return chip;
}

static void tab_bar(lv_obj_t *parent, const char *l, const char *r, bool left_active,
                    lv_color_t underline, lv_color_t bg,
                    void (*l_tap)(void), void (*r_tap)(void))
{
    lv_obj_t *tabs = lz_box(parent);
    lv_obj_set_size(tabs, LZ_W, 24);
    lv_obj_set_style_bg_color(tabs, bg, 0);
    lv_obj_set_style_bg_opa(tabs, LV_OPA_COVER, 0);
    const char *names[2] = { l, r };
    void (*taps[2])(void) = { l_tap, r_tap };
    for(int i = 0; i < 2; i++) {
        bool act = (i == 0) == left_active;
        lv_obj_t *t = lz_box(tabs);
        lv_obj_set_size(t, LZ_W / 2 - 8, 24);
        lv_obj_set_pos(t, 8 + i * (LZ_W / 2 - 8), 0);
        lv_obj_set_style_border_side(t, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(t, 2, 0);
        lv_obj_set_style_border_color(t, act ? underline : bg, 0);
        lv_obj_t *lbl = lz_text(t, names[i], LZ_F_BODY,
                                act ? LZ_TEXT_BRIGHT : LZ_TEXT_META);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);
        if(taps[i]) lz_on_click(t, taps[i]);
    }
}

static void tap_tab_dms(void)      { if(S.msg_tab != LZ_TAB_DMS)      { S.msg_tab = LZ_TAB_DMS;      S.focus = 0; lz_rebuild(); } }
static void tap_tab_channels(void) { if(S.msg_tab != LZ_TAB_CHANNELS) { S.msg_tab = LZ_TAB_CHANNELS; S.focus = 0; lz_rebuild(); } }
static void tap_filter_all(void)   { S.msg_filter = LZ_FILT_ALL; S.focus = 0; lz_rebuild(); }
static void tap_filter_mt(void)    { S.msg_filter = LZ_FILT_MT;  S.focus = 0; lz_rebuild(); }
static void tap_filter_mc(void)    { S.msg_filter = LZ_FILT_MC;  S.focus = 0; lz_rebuild(); }
static void tap_compose(void)      { lz_go(LZ_V_CONTACTS); }
static void tap_send(void)         { lz_ui_key(LZ_K_ENTER, 0); }

void lz_scr_messages(lv_obj_t *root)
{
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *bar = lz_navbar(root, "Messages", NULL);
    lv_obj_t *pen = lz_icon(bar, LZ_I_EDIT, &lz_icons_18, LZ_MINT);
    lv_obj_align(pen, LV_ALIGN_RIGHT_MID, -7, 0);
    lv_obj_t *penhit = lz_box(bar);
    lv_obj_set_size(penhit, 44, LZ_NAVBAR_H);
    lv_obj_align(penhit, LV_ALIGN_RIGHT_MID, 0, 0);
    lz_on_click(penhit, tap_compose);

    tab_bar(root, "Direct", "Channels", S.msg_tab == LZ_TAB_DMS,
            LZ_MINT, lv_color_hex(0x0E141B), tap_tab_dms, tap_tab_channels);

    /* filter chips */
    lv_obj_t *filters = lz_box(root);
    lv_obj_set_size(filters, LZ_W, 29);
    lv_obj_set_style_border_side(filters, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(filters, 1, 0);
    lv_obj_set_style_border_color(filters, lv_color_hex(0x171B21), 0);
    lv_obj_set_flex_flow(filters, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(filters, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(filters, 8, 0);
    lv_obj_set_style_pad_column(filters, 5, 0);
    lz_on_click(filter_chip(filters, "All", S.msg_filter == LZ_FILT_ALL,
                lv_color_black(), false, lv_color_hex(0xCFD4DA), LZ_ON_MINT),
                tap_filter_all);
    lz_on_click(filter_chip(filters, "Meshtastic", S.msg_filter == LZ_FILT_MT,
                LZ_CYAN, true, LZ_CYAN, LZ_ON_CYAN),
                tap_filter_mt);
    lz_on_click(filter_chip(filters, "MeshCore", S.msg_filter == LZ_FILT_MC,
                LZ_AMBER, true, LZ_AMBER, LZ_ON_AMBER),
                tap_filter_mc);

    /* list */
    lv_obj_t *body = lz_vflex(root);
    lv_obj_set_style_pad_top(body, 5, 0);
    lv_obj_set_style_pad_hor(body, 7, 0);
    lv_obj_set_style_pad_bottom(body, 8, 0);
    lv_obj_set_style_pad_row(body, 3, 0);
    lz_nav_set_scroll(body);

    if(S.msg_tab == LZ_TAB_DMS) {
        vis_thread_count = 0;
        for(int i = 0; i < 6; i++)
            if(filter_match(LZ_THREADS[i].net)) vis_threads[vis_thread_count++] = &LZ_THREADS[i];

        for(int i = 0; i < vis_thread_count; i++) {
            const lz_thread_t *t = vis_threads[i];
            lv_obj_t *row = lz_row(body, i == S.focus);
            lv_obj_set_style_radius(row, 11, 0);
            if(!net_on(t->net)) lv_obj_set_style_opa(row, LV_OPA_40, 0);

            /* avatar + network dot */
            lv_obj_t *avwrap = lz_box(row);
            lv_obj_set_size(avwrap, 34, 34);
            lv_obj_t *av = lz_dot(avwrap, 33, lz_av_color(t->net));
            char initial[2] = { t->name[0], 0 };
            lv_obj_t *ini = lz_text(av, initial, LZ_F_BODY, lv_color_white());
            lv_obj_center(ini);
            lv_obj_t *ring = lz_dot(avwrap, 13, LZ_SCREEN_BG);
            lv_obj_align(ring, LV_ALIGN_BOTTOM_RIGHT, 1, 1);
            lv_obj_t *nd = lz_dot(ring, 9, lz_net_color(t->net));
            lv_obj_center(nd);

            /* text column */
            lv_obj_t *colm = lz_box(row);
            lv_obj_set_flex_grow(colm, 1);
            lv_obj_set_height(colm, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(colm, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(colm, 2, 0);

            lv_obj_t *top = lz_box(colm);
            lv_obj_set_width(top, lv_pct(100));
            lv_obj_set_height(top, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_t *name = lz_text(top, t->name, LZ_F_BODY, LZ_TEXT);
            lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
            lz_text(top, t->t, LZ_F_SMALL, LZ_TEXT_META);

            lv_obj_t *bot = lz_box(colm);
            lv_obj_set_width(bot, lv_pct(100));
            lv_obj_set_height(bot, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(bot, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(bot, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_t *snip = lz_text(bot, t->text, LZ_F_SMALL,
                                     t->unread ? lv_color_hex(0xCFD4DA) : lv_color_hex(0x838A93));
            lv_label_set_long_mode(snip, LV_LABEL_LONG_DOT);
            lv_obj_set_flex_grow(snip, 1);
            if(t->unread) {
                lv_obj_t *badge = lz_box(bot);
                lv_obj_set_size(badge, LV_SIZE_CONTENT, 15);
                lv_obj_set_style_min_width(badge, 15, 0);
                lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_bg_color(badge, LZ_MINT, 0);
                lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
                lv_obj_set_style_pad_hor(badge, 4, 0);
                char ub[4]; snprintf(ub, sizeof ub, "%d", t->unread);
                lv_obj_t *ul = lz_text(badge, ub, LZ_F_SMALL, LZ_ON_MINT);
                lv_obj_center(ul);
            }
            lz_nav_track(row, i);
        }

        /* disabled-network note (history kept) */
        if(S.msg_filter == LZ_FILT_ALL && (!S.net_mt || !S.net_mc)) {
            char note[96];
            const char *who = (!S.net_mt && !S.net_mc) ? "Meshtastic & MeshCore"
                              : !S.net_mt ? "Meshtastic" : "MeshCore";
            snprintf(note, sizeof note,
                     "%s disabled in Settings - conversations hidden, history kept.", who);
            lv_obj_t *nb = lz_box(body);
            lv_obj_set_width(nb, lv_pct(100));
            lv_obj_set_height(nb, LV_SIZE_CONTENT);
            lv_obj_set_style_radius(nb, 9, 0);
            lv_obj_set_style_border_width(nb, 1, 0);
            lv_obj_set_style_border_color(nb, lv_color_hex(0x2A2F37), 0);
            lv_obj_set_style_pad_all(nb, 8, 0);
            lv_obj_t *nl = lz_text(nb, note, LZ_F_SMALL, lv_color_hex(0x7F868F));
            lv_label_set_long_mode(nl, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(nl, lv_pct(100));
        }
        lz_nav_set(1, vis_thread_count, messages_activate);
    } else {
        int n = 0;
        for(int i = 0; i < 4; i++) {
            const lz_chan_t *c = &LZ_CHANS[i];
            if(!filter_match(c->net)) continue;
            int idx = n++;
            lv_obj_t *row = lz_row(body, idx == S.focus);
            lv_obj_set_style_radius(row, 11, 0);
            if(!net_on(c->net)) lv_obj_set_style_opa(row, LV_OPA_40, 0);

            lv_obj_t *tile = lz_box(row);
            lv_obj_set_size(tile, 33, 33);
            lv_obj_set_style_radius(tile, 9, 0);
            lv_obj_set_style_bg_color(tile, lz_tile_color(c->net == LZ_NET_MT ? 205 : 72), 0);
            lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
            lv_obj_t *ic = lz_icon(tile, c->icon, &lz_icons_18, lv_color_white());
            lv_obj_center(ic);

            lv_obj_t *colm = lz_box(row);
            lv_obj_set_flex_grow(colm, 1);
            lv_obj_set_height(colm, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(colm, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(colm, 1, 0);

            lv_obj_t *top = lz_box(colm);
            lv_obj_set_width(top, lv_pct(100));
            lv_obj_set_height(top, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lz_text(top, c->name, LZ_F_BODY, LZ_TEXT);
            lz_text(top, c->t, LZ_F_SMALL, LZ_TEXT_META);
            lz_text(colm, c->sub, LZ_F_SMALL, lv_color_hex(0x838A93));
            lv_obj_t *last = lz_text(colm, c->text, LZ_F_SMALL, LZ_TEXT_VALUE);
            lv_label_set_long_mode(last, LV_LABEL_LONG_DOT);
            lv_obj_set_width(last, lv_pct(100));
            lz_nav_track(row, idx);
        }
        lz_nav_set(1, n, messages_activate);
    }
}

/* ===== Conversation ===== */

void lz_scr_convo(lv_obj_t *root)
{
    const lz_thread_t *t = S.convo ? S.convo : &LZ_THREADS[0];
    lv_color_t net = lz_net_color(t->net);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    /* nav bar: name + network tag */
    lv_obj_t *bar = lz_box(root);
    lv_obj_set_size(bar, LZ_W, LZ_NAVBAR_H);
    lv_obj_set_style_bg_color(bar, LZ_NAVBAR_BG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, LZ_HAIRLINE, 0);
    lv_obj_t *chev = lz_icon(bar, LZ_I_CHEV_L, &lz_icons_18, LZ_TEXT_NAV);
    lv_obj_align(chev, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_t *backhit = lz_box(bar);
    lv_obj_set_size(backhit, 64, LZ_NAVBAR_H);
    lv_obj_set_pos(backhit, 0, 0);
    lz_on_click(backhit, lz_back);
    lv_obj_t *name = lz_text(bar, t->name, LZ_F_BODY, lv_color_hex(0xF2F4F6));
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 3);
    lv_obj_t *sub = lz_box(bar);
    lv_obj_set_size(sub, LV_SIZE_CONTENT, 10);
    lv_obj_set_flex_flow(sub, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sub, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(sub, 4, 0);
    lz_dot(sub, 5, net);
    lz_text(sub, lz_net_name(t->net), LZ_F_SMALL, net);
    char path[24]; snprintf(path, sizeof path, "- %s", t->path);
    lz_text(sub, path, LZ_F_SMALL, LZ_TEXT_META);
    lv_obj_align(sub, LV_ALIGN_BOTTOM_MID, 0, -1);

    /* thread */
    lv_obj_t *body = lz_vflex(root);
    lv_obj_set_style_pad_all(body, 9, 0);
    lv_obj_set_style_pad_row(body, 6, 0);
    lz_nav_set_scroll(body);

    char cap[64];
    snprintf(cap, sizeof cap, "Encrypted - %s - %s", lz_net_name(t->net), t->addr);
    lv_obj_t *caption = lz_text(body, cap, LZ_F_SMALL, lv_color_hex(0x5E656E));
    lv_obj_set_width(caption, lv_pct(100));
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);

    const lz_msg_t *msgs = NULL; int mc = 0;
    static lz_msg_t fallback;
    if(strcmp(t->id, "ava") == 0)         { msgs = LZ_MSGS_AVA;    mc = 5; }
    else if(strcmp(t->id, "dmitri") == 0) { msgs = LZ_MSGS_DMITRI; mc = 4; }
    else { fallback.self = false; fallback.text = t->text; msgs = &fallback; mc = 1; }

    int total = mc + S.sent_count;
    for(int i = 0; i < total; i++) {
        bool self = i < mc ? msgs[i].self : true;
        const char *txt = i < mc ? msgs[i].text : S.sent[i - mc];

        lv_obj_t *line = lz_box(body);
        lv_obj_set_width(line, lv_pct(100));
        lv_obj_set_height(line, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(line, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START,
                              self ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(line, 2, 0);

        lv_obj_t *bub = lz_box(line);
        lv_obj_set_height(bub, LV_SIZE_CONTENT);
        lv_obj_set_width(bub, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(bub, (LZ_W * 74) / 100, 0);
        lv_obj_set_style_radius(bub, 13, 0);
        lv_obj_set_style_bg_color(bub, self ? LZ_SELF_BUBBLE : LZ_BUBBLE_IN, 0);
        lv_obj_set_style_bg_opa(bub, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(bub, 9, 0);
        lv_obj_set_style_pad_ver(bub, 6, 0);
        lv_obj_t *bl = lz_text(bub, txt, LZ_F_BODY,
                               self ? LZ_ON_MINT : lv_color_hex(0xE3E7EC));
        /* LVGL 8 labels only wrap at a fixed width: measure, clamp, wrap */
        lv_point_t tsz;
        lv_txt_get_size(&tsz, txt, LZ_F_BODY, 0, 0, LV_COORD_MAX, 0);
        int maxw = (LZ_W * 74) / 100 - 18;
        if(tsz.x > maxw) {
            lv_label_set_long_mode(bl, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(bl, maxw);
        }

        char ts[8]; snprintf(ts, sizeof ts, "14:2%d", i % 10);
        lz_text(line, ts, LZ_F_SMALL, lv_color_hex(0x6B727B));
    }

    /* compose bar: input pill + network-tagged send */
    lv_obj_t *compose = lz_box(root);
    lv_obj_set_size(compose, LZ_W, 35);
    lv_obj_set_style_bg_color(compose, lv_color_hex(0x0F141B), 0);
    lv_obj_set_style_bg_opa(compose, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(compose, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(compose, 1, 0);
    lv_obj_set_style_border_color(compose, lv_color_hex(0x1A212A), 0);
    lv_obj_set_flex_flow(compose, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(compose, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(compose, 7, 0);
    lv_obj_set_style_pad_column(compose, 6, 0);

    lv_obj_t *input = lz_box(compose);
    lv_obj_set_flex_grow(input, 1);
    lv_obj_set_height(input, 24);
    lv_obj_set_style_radius(input, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(input, lv_color_hex(0x191D24), 0);
    lv_obj_set_style_bg_opa(input, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(input, 1, 0);
    lv_obj_set_style_border_color(input, lv_color_hex(0x2A2F37), 0);
    char ph[48];
    bool has_draft = S.draft[0] != 0;
    if(!has_draft) snprintf(ph, sizeof ph, "Message %s...", t->name);
    lv_obj_t *itxt = lz_text(input, has_draft ? S.draft : ph, LZ_F_SMALL,
                             has_draft ? LZ_TEXT : lv_color_hex(0x6B727B));
    lv_label_set_long_mode(itxt, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(itxt, lv_pct(100));
    lv_obj_align(itxt, LV_ALIGN_LEFT_MID, 11, 0);

    lv_obj_t *send = lz_box(compose);
    lv_obj_set_size(send, LV_SIZE_CONTENT, 24);
    lv_obj_set_style_radius(send, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(send, t->net == LZ_NET_MT ? LZ_SEND_MT : LZ_SEND_MC, 0);
    lv_obj_set_style_bg_opa(send, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(send, 10, 0);
    lv_obj_set_flex_flow(send, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(send, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(send, 3, 0);
    lz_text(send, lz_net_name(t->net), LZ_F_SMALL, lv_color_white());
    lz_icon(send, LZ_I_SEND, &lz_icons_14, lv_color_white());
    lz_on_click(send, tap_send);

    lz_nav_set(1, 0, NULL);  /* no focusables: up/down scroll, Enter sends */
}
