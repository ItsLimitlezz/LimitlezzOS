/* Messages (unified inbox: Direct/Channels tabs, network filter chips)
 * + Conversation (network-bound thread with tagged compose bar) */
#include "../ui.h"
#include <stdio.h>
#include <string.h>

static lz_thread_rt *vis_threads[LZ_MAX_THREADS];
static int vis_thread_count;
static lv_obj_t *g_draft_label;
static lz_thread_rt *g_draft_thread;

static bool net_on(lz_net_t n) { return n == LZ_NET_MT ? S.net_mt : S.net_mc; }

static void draft_label_text(const lz_thread_rt *t, char *out, size_t cap, bool *has_draft)
{
    bool has = S.draft[0] != 0;
    if(has_draft) *has_draft = has;
    if(!has) {
        snprintf(out, cap, "Message %s...", t ? t->name : "thread");
        return;
    }

    snprintf(out, cap, "%s", S.draft);
    int maxw = LZ_W - 80;                    /* usable px inside the pill */
    lv_point_t sz;
    lv_txt_get_size(&sz, S.draft, LZ_F_SMALL, 0, 0, LV_COORD_MAX, 0);
    if(sz.x <= maxw) return;

    int start = 0;
    while(S.draft[start]) {
        lv_txt_get_size(&sz, S.draft + start, LZ_F_SMALL, 0, 0, LV_COORD_MAX, 0);
        if(sz.x <= maxw - 12) break;          /* leave room for the ellipsis */
        start++;
    }
    snprintf(out, cap, "...%s", S.draft + start);
}

bool lz_convo_draft_refresh(void)
{
    if(S.view != LZ_V_CONVO || !g_draft_label || !g_draft_thread) return false;
    char shown[LZ_DRAFT_MAX + 16];
    bool has_draft = false;
    draft_label_text(g_draft_thread, shown, sizeof shown, &has_draft);
    lv_label_set_text(g_draft_label, shown);
    lv_obj_set_style_text_color(g_draft_label,
                                has_draft ? LZ_TEXT : lv_color_hex(0x6B727B), 0);
    return true;
}

static bool filter_match(lz_net_t n)
{
    return S.msg_filter == LZ_FILT_ALL ||
           (S.msg_filter == LZ_FILT_MT && n == LZ_NET_MT) ||
           (S.msg_filter == LZ_FILT_MC && n == LZ_NET_MC);
}

void lz_open_convo(lz_thread_rt *t)
{
    if(!t) return;                       /* thread table full: stay put */
    S.convo = t;
    S.draft[0] = 0;
    lz_svc_open_thread(t);
    lz_go(LZ_V_CONVO);
}

static void messages_activate(int idx)
{
    if(idx >= 0 && idx < vis_thread_count) lz_open_convo(vis_threads[idx]);
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

/* open a node's profile (contact detail) — from there you can Message or
 * Add to contacts. Deferred so we don't navigate inside an LVGL event. */
static void open_profile_async(void *p)
{
    lz_node_rt *n = (lz_node_rt *)p;
    if(n) { S.contact_sel = n; lz_go(LZ_V_CONTACT); lz_rebuild(); }
}
/* long-press an incoming channel message -> open the sender's profile. The
 * bubble text is "SHORT: message"; we recover the sender by short code. */
static void channel_longpress_cb(lv_event_t *e)
{
    lv_obj_t *bub = lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(bub, 0);
    if(!lbl) return;
    const char *txt = lv_label_get_text(lbl);
    char sc[8]; int i = 0;
    while(txt[i] && txt[i] != ':' && txt[i] != ' ' && i < (int)sizeof sc - 1) { sc[i] = txt[i]; i++; }
    sc[i] = 0;
    lz_node_rt *n = lz_svc_node_by_shortcode(sc);
    if(n) lv_async_call(open_profile_async, n);
}
/* tap the name in a DM's header -> open that contact's profile */
static void convo_header_cb(lv_event_t *e)
{
    lz_node_rt *n = (lz_node_rt *)lv_event_get_user_data(e);
    if(n) lv_async_call(open_profile_async, n);
}
/* long-press a failed (red) sent bubble -> resend it */
static void resend_async(void *p) { if(lz_svc_resend((int)(intptr_t)p)) lz_rebuild(); }
static void resend_cb(lv_event_t *e)
{
    lv_async_call(resend_async, lv_event_get_user_data(e));
}

/* long-press a chat row -> silence/unsilence it (no notification, no badge) */
static void mute_async(void *p) { lz_svc_toggle_mute((lz_thread_rt *)p); lz_rebuild(); }
static void mute_longpress_cb(lv_event_t *e)
{
    /* swallow the rest of this press so lifting the finger doesn't ALSO fire a
     * click and open the chat — you mute on hold, then tap again to enter */
    lv_indev_t *ind = lv_indev_get_act();
    if(ind) lv_indev_wait_release(ind);
    lv_async_call(mute_async, lv_event_get_user_data(e));
}

/* a small crescent "moon" = silenced (iOS cue). carve must match the row bg so
 * the cut-out reads as a crescent. */
static void mute_moon(lv_obj_t *parent, lv_color_t carve)
{
    lv_obj_t *wrap = lz_box(parent);
    lv_obj_set_size(wrap, 14, 14);
    lv_obj_t *full = lz_dot(wrap, 13, lv_color_hex(0x8A929C));
    lv_obj_center(full);
    lv_obj_t *cut = lz_dot(wrap, 11, carve);
    lv_obj_align(cut, LV_ALIGN_TOP_RIGHT, 2, -2);
}

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
    lv_obj_t *mc_chip = filter_chip(filters, "MeshCore", S.msg_filter == LZ_FILT_MC,
                LZ_AMBER, true, LZ_AMBER, LZ_ON_AMBER);
    if(LZ_MESHCORE_ENABLED) lz_on_click(mc_chip, tap_filter_mc);
    else lv_obj_set_style_opa(mc_chip, LV_OPA_70, 0);   /* locked, but still legible */

    /* list */
    lv_obj_t *body = lz_vflex(root);
    lv_obj_set_style_pad_top(body, 5, 0);
    lv_obj_set_style_pad_hor(body, 7, 0);
    lv_obj_set_style_pad_bottom(body, 8, 0);
    lv_obj_set_style_pad_row(body, 3, 0);
    lz_nav_set_scroll(body);

    if(S.msg_tab == LZ_TAB_DMS) {
        int tn = lz_svc_thread_count_all();
        vis_thread_count = 0;
        for(int i = 0; i < tn; i++) {
            lz_thread_rt *th = lz_svc_thread_at(i);   /* newest-first */
            if(th->is_channel) continue;              /* channels live on the other tab */
            if(filter_match(th->net)) vis_threads[vis_thread_count++] = th;
        }

        for(int i = 0; i < vis_thread_count; i++) {
            lz_thread_rt *t = vis_threads[i];
            char ago[8]; lz_fmt_ago(t->last_ts, ago, sizeof ago);
            bool unread = t->unread && !t->muted;
            lv_obj_t *row = lz_row(body, i == S.focus);
            lv_obj_set_style_radius(row, 11, 0);
            if(unread && i != S.focus)               /* highlight an unread chat (dark mint) */
                lv_obj_set_style_bg_color(row, lv_color_hex(0x123026), 0);
            if(!net_on(t->net)) lv_obj_set_style_opa(row, LV_OPA_40, 0);
            lv_obj_add_event_cb(row, mute_longpress_cb, LV_EVENT_LONG_PRESSED, t); /* hold = silence */

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
            lv_obj_t *name = lz_text(top, t->name, LZ_F_BODY, unread ? lv_color_hex(0xF2F4F6) : LZ_TEXT);
            lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
            lz_text(top, ago, LZ_F_SMALL, LZ_TEXT_META);

            lv_obj_t *bot = lz_box(colm);
            lv_obj_set_width(bot, lv_pct(100));
            lv_obj_set_height(bot, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(bot, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(bot, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_t *snip = lz_text(bot, t->last_text, LZ_F_SMALL,
                                     unread ? lv_color_hex(0xCFD4DA) : lv_color_hex(0x838A93));
            lv_label_set_long_mode(snip, LV_LABEL_LONG_DOT);
            lv_obj_set_flex_grow(snip, 1);
            if(t->muted) {
                /* silenced: a crescent moon instead of the unread badge */
                mute_moon(bot, i == S.focus ? LZ_ROW_FOCUS_BG : LZ_ROW_BG);
            } else if(t->unread) {
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
            if(!LZ_MESHCORE_ENABLED && S.net_mt) {
                snprintf(note, sizeof note,
                         "MeshCore is coming soon - Meshtastic only for now.");
            } else {
                const char *who = (!S.net_mt && !S.net_mc) ? "Meshtastic & MeshCore"
                                  : !S.net_mt ? "Meshtastic" : "MeshCore";
                snprintf(note, sizeof note,
                         "%s disabled in Settings - conversations hidden, history kept.", who);
            }
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
        /* Channels = broadcast channels (LongFast). Real, openable: tap to
         * read + send broadcast to everyone nearby. */
        int tn = lz_svc_thread_count_all();
        vis_thread_count = 0;
        for(int i = 0; i < tn; i++) {
            lz_thread_rt *th = lz_svc_thread_at(i);
            if(th->is_channel) vis_threads[vis_thread_count++] = th;
        }
        for(int i = 0; i < vis_thread_count; i++) {
            lz_thread_rt *t = vis_threads[i];
            char ago[8]; lz_fmt_ago(t->last_ts, ago, sizeof ago);
            lv_obj_t *row = lz_row(body, i == S.focus);
            lv_obj_set_style_radius(row, 11, 0);
            lv_obj_add_event_cb(row, mute_longpress_cb, LV_EVENT_LONG_PRESSED, t); /* hold = silence */

            lv_obj_t *tile = lz_box(row);
            lv_obj_set_size(tile, 33, 33);
            lv_obj_set_style_radius(tile, 9, 0);
            lv_obj_set_style_bg_color(tile, lz_tile_color(205), 0);
            lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
            lv_obj_t *ic = lz_icon(tile, LZ_I_TAG, &lz_icons_18, lv_color_white());
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
            lz_text(top, t->name, LZ_F_BODY, LZ_TEXT);
            lz_text(top, t->last_ts ? ago : "", LZ_F_SMALL, LZ_TEXT_META);
            lz_text(colm, "Primary - broadcast to everyone", LZ_F_SMALL, lv_color_hex(0x838A93));
            lv_obj_t *last = lz_text(colm, t->last_text[0] ? t->last_text : "Tap to open the channel",
                                     LZ_F_SMALL, LZ_TEXT_VALUE);
            lv_label_set_long_mode(last, LV_LABEL_LONG_DOT);
            lv_obj_set_width(last, lv_pct(100));
            if(t->muted)        /* silenced channel: crescent moon at the right */
                mute_moon(row, i == S.focus ? LZ_ROW_FOCUS_BG : LZ_ROW_BG);
            lz_nav_track(row, i);
        }
        lz_nav_set(1, vis_thread_count, messages_activate);
    }
}

/* ===== Conversation ===== */

void lz_scr_convo(lv_obj_t *root)
{
    g_draft_label = NULL;
    g_draft_thread = NULL;
    lz_thread_rt *t = S.convo;
    if(!t) t = lz_svc_thread_at(0);
    if(!t) return;
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
    char pathb[24]; snprintf(pathb, sizeof pathb, "- %s", t->path);
    lz_text(sub, pathb, LZ_F_SMALL, LZ_TEXT_META);
    lv_obj_align(sub, LV_ALIGN_BOTTOM_MID, 0, -1);

    /* DM header is tappable -> the contact's profile (Message / Add contact).
     * Channels have no single peer, so only wire it for direct threads. */
    lz_node_rt *peer = (!t->is_channel && t->node_num != LZ_BROADCAST)
                       ? lz_svc_node_by_num(t->node_num) : NULL;
    if(peer) {
        lv_obj_t *hit = lz_box(bar);          /* transparent tap target over the name */
        lv_obj_set_size(hit, 184, LZ_NAVBAR_H);
        lv_obj_align(hit, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(hit, convo_header_cb, LV_EVENT_CLICKED, peer);
    }

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

    const lz_msg_rt *msgs;
    int total = lz_svc_tail(&msgs);
    for(int i = 0; i < total; i++) {
        bool self = msgs[i].self;
        const char *txt = msgs[i].text;

        /* row spanning the width; justify the bubble cluster right for self
         * (outgoing) and left for incoming — the standard messenger layout */
        lv_obj_t *line = lz_box(body);
        lv_obj_set_width(line, lv_pct(100));
        lv_obj_set_height(line, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(line, self ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

        lv_obj_t *col = lz_box(line);
        lv_obj_set_width(col, LV_SIZE_CONTENT);
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(col, (LZ_W * 74) / 100, 0);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START,
                              self ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(col, 2, 0);

        lv_obj_t *bub = lz_box(col);
        lv_obj_set_height(bub, LV_SIZE_CONTENT);
        lv_obj_set_width(bub, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(bub, (LZ_W * 74) / 100, 0);
        lv_obj_set_style_radius(bub, 13, 0);
        /* delivery status colors a sent DM bubble: green=sending, blue=delivered,
         * red=failed; channel/historical sent stay the default teal */
        lv_color_t bg = self ? LZ_SELF_BUBBLE : LZ_BUBBLE_IN;
        bool status_color = false;
        if(self) switch(msgs[i].status) {
            case LZ_MSG_SENDING:   bg = lv_color_hex(0x2E8B43); status_color = true; break;  /* green */
            case LZ_MSG_DELIVERED: bg = lv_color_hex(0x1E6FD0); status_color = true; break;  /* blue */
            case LZ_MSG_FAILED:    bg = lv_color_hex(0xC9402F); status_color = true; break;  /* red */
            default: break;
        }
        lv_obj_set_style_bg_color(bub, bg, 0);
        lv_obj_set_style_bg_opa(bub, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(bub, 9, 0);
        lv_obj_set_style_pad_ver(bub, 6, 0);
        lv_obj_t *bl = lz_text(bub, txt, LZ_F_BODY,
                               (self && status_color) ? lv_color_white()
                               : self ? LZ_ON_MINT : lv_color_hex(0xE3E7EC));
        /* LVGL 8 labels only wrap at a fixed width: measure, clamp, wrap */
        lv_point_t tsz;
        lv_txt_get_size(&tsz, txt, LZ_F_BODY, 0, 0, LV_COORD_MAX, 0);
        int maxw = (LZ_W * 74) / 100 - 18;
        if(tsz.x > maxw) {
            lv_label_set_long_mode(bl, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(bl, maxw);
        }
        /* in a channel, long-press an incoming message to DM that person */
        if(t->is_channel && !self) {
            lv_obj_add_flag(bub, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(bub, channel_longpress_cb, LV_EVENT_LONG_PRESSED, NULL);
        }
        /* long-press a failed (red) sent bubble to resend it */
        if(self && msgs[i].status == LZ_MSG_FAILED &&
           msgs[i].retries < LZ_MSG_RETRY_MAX) {
            lv_obj_add_flag(bub, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(bub, resend_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        }

        /* status line under a sent DM bubble */
        char ts[16]; lz_fmt_hm(msgs[i].ts, ts, sizeof ts);
        char stbuf[24];
        const char *st = "";
        if(self) switch(msgs[i].status) {
            case LZ_MSG_SENDING:
                if(msgs[i].retries) {
                    snprintf(stbuf, sizeof stbuf, "  retrying %u/%u",
                             (unsigned)msgs[i].retries,
                             (unsigned)LZ_MSG_RETRY_MAX);
                    st = stbuf;
                } else {
                    st = "  sending";
                }
                break;
            case LZ_MSG_DELIVERED: st = "  delivered"; break;
            case LZ_MSG_FAILED:
                st = msgs[i].retries >= LZ_MSG_RETRY_MAX
                   ? "  failed - retry limit"
                   : msgs[i].fail_reason == LZ_FAIL_RADIO_SEND
                   ? "  failed - radio send"
                   : msgs[i].fail_reason == LZ_FAIL_ACK_TIMEOUT
                   ? "  failed - ack timeout"
                   : "  failed - hold to resend";
                break;
            default: break;
        }
        char tl[40]; snprintf(tl, sizeof tl, "%s%s", ts, st);
        lz_text(col, tl, LZ_F_SMALL, lv_color_hex(0x6B727B));
    }

    /* read-only thread (MeshCore in Stage 1, infrastructure): no composer */
    if(!t->messageable) {
        lv_obj_t *ro = lz_box(root);
        lv_obj_set_size(ro, LZ_W, 35);
        lv_obj_set_style_bg_color(ro, lv_color_hex(0x0F141B), 0);
        lv_obj_set_style_bg_opa(ro, LV_OPA_COVER, 0);
        lv_obj_set_style_border_side(ro, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(ro, 1, 0);
        lv_obj_set_style_border_color(ro, lv_color_hex(0x1A212A), 0);
        const char *msg = t->net == LZ_NET_MC
            ? "MeshCore replies arrive in a later update"
            : "This node can be observed, not messaged";
        lv_obj_t *rl = lz_text(ro, msg, LZ_F_SMALL, lv_color_hex(0x6B727B));
        lv_obj_center(rl);
        lz_nav_set(1, 0, NULL);
        return;
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
    g_draft_thread = t;
    g_draft_label = lz_text(input, "", LZ_F_SMALL, LZ_TEXT);
    lv_label_set_long_mode(g_draft_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(g_draft_label, lv_pct(100));
    lv_obj_align(g_draft_label, LV_ALIGN_LEFT_MID, 11, 0);
    lz_convo_draft_refresh();

    /* send: paper plane only — the nav bar already names the network */
    lv_obj_t *send = lz_box(compose);
    lv_obj_set_size(send, 34, 24);
    lv_obj_set_style_radius(send, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(send, t->net == LZ_NET_MT ? LZ_SEND_MT : LZ_SEND_MC, 0);
    lv_obj_set_style_bg_opa(send, LV_OPA_COVER, 0);
    lv_obj_t *si = lz_icon(send, LZ_I_SEND, &lz_icons_14, lv_color_white());
    lv_obj_center(si);
    lz_on_click(send, tap_send);

    lz_nav_set(1, 0, NULL);  /* no focusables: up/down scroll, Enter sends */
}
