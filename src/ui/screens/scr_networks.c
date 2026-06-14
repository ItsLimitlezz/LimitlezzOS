/* Network stack managers: Meshtastic (cyan) and MeshCore (amber) */
#include "../ui.h"
#include "../vlist.h"
#include <stdio.h>
#include <string.h>

/* node list row metrics (fixed, for virtualization) */
#define MT_HEADER_ROWS 2
#define MT_HEADER_H (MT_HEADER_ROWS * 50)   /* USB/BLE companion toggle rows */
#define MT_ROW_H    44
#define MT_STRIDE   48

static lz_node_rt *vis_nodes[LZ_MAX_NODES];
static int vis_count;

static void open_contact(int idx)
{
    if(idx >= 0 && idx < vis_count) {
        S.contact_sel = vis_nodes[idx];
        lz_go(LZ_V_CONTACT);
    }
}

static void open_longfast(int idx) { (void)idx; lz_open_convo(lz_svc_channel_thread()); }

/* Companion rows: idx 0 = USB serial companion, idx 1 = BLE companion. Heard
 * nodes follow at MT_HEADER_ROWS+. */
static void mt_nodes_activate(int idx)
{
    if(idx == 0) { lz_mtc_set_active(!lz_mtc_active()); lz_rebuild(); return; }
    if(idx == 1) { lz_mtc_ble_set_enabled(!lz_mtc_ble_enabled()); lz_rebuild(); return; }
    open_contact(idx - MT_HEADER_ROWS);
}

/* MeshCore self-advertise (so other nodes discover us) — only when MeshCore is
 * enabled. With it on, idx 0 = zero-hop, idx 1 = flood, idx 2+ a heard node.
 * With it off there are no advert rows, so idx maps straight to nodes. */
static const char *g_mc_note;   /* transient confirmation under the buttons */
static int mc_adv_rows(void) { return S.net_mc ? 2 : 0; }
static void mc_activate(int idx)
{
    int adv = mc_adv_rows();
    if(adv && (idx == 0 || idx == 1)) {
        bool flood = (idx == 1);
        bool ok = lz_backend_mc_advert_now(flood);
        g_mc_note = !ok ? "Advert failed (radio off?)"
                  : flood ? "Flood advert sent across the mesh"
                          : "Advert sent to neighbors";
        lz_rebuild();
        return;
    }
    open_contact(idx - adv);
}

static lv_obj_t *colored_navbar(lv_obj_t *root, const char *title, lv_color_t bg,
                                lv_color_t hairline, bool status_on)
{
    lv_obj_t *bar = lz_box(root);
    lv_obj_set_size(bar, LZ_W, LZ_NAVBAR_H);
    lv_obj_set_style_bg_color(bar, bg, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, hairline, 0);
    lv_obj_t *chev = lz_icon(bar, LZ_I_CHEV_L, &lz_icons_18, lv_color_hex(0xEAF6FB));
    lv_obj_align(chev, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_t *t = lz_text(bar, title, LZ_F_HEAD, lv_color_white());
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *dot = lz_dot(bar, 6, status_on ? LZ_GREEN : lv_color_hex(0x5A616A));
    lv_obj_align(dot, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_t *hit = lz_box(bar);
    lv_obj_set_size(hit, 64, LZ_NAVBAR_H);
    lv_obj_set_pos(hit, 0, 0);
    lz_on_click(hit, lz_back);
    return bar;
}

static void tap_mt_nodes(void)    { if(S.mt_tab != 0) { S.mt_tab = 0; S.focus = 0; lz_vlist_reset_scroll(); lz_rebuild(); } }
static void tap_mt_channels(void) { if(S.mt_tab != 1) { S.mt_tab = 1; S.focus = 0; lz_vlist_reset_scroll(); lz_rebuild(); } }
static void tap_mc_contacts(void) { if(S.mc_tab != 0) { S.mc_tab = 0; S.focus = 0; lz_rebuild(); } }
static void tap_mc_rooms(void)    { if(S.mc_tab != 1) { S.mc_tab = 1; S.focus = 0; lz_rebuild(); } }

static lv_obj_t *identity_card(lv_obj_t *root, lv_color_t bg, lv_color_t hairline)
{
    lv_obj_t *card = lz_box(root);
    lv_obj_set_size(card, LZ_W, 50);
    lv_obj_set_style_bg_color(card, bg, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, hairline, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(card, 10, 0);
    lv_obj_set_style_pad_column(card, 9, 0);
    return card;
}

static void role_badge(lv_obj_t *parent, const char *role, lv_color_t fg)
{
    lv_obj_t *b = lz_box(parent);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 13);
    lv_obj_set_style_radius(b, 5, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x252A31), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(b, 5, 0);
    lv_obj_t *l = lz_text(b, role, LZ_F_SMALL, fg);
    lv_obj_center(l);
}

/* one virtualized node row (built on demand by lz_vlist as it scrolls) */
static lv_obj_t *mt_node_row_cb(lv_obj_t *content, int index, int y, bool focused, void *ctx)
{
    (void)ctx;
    lz_node_rt *n = vis_nodes[index];
    char ago[8], snrs[8];
    lz_fmt_ago(n->last_heard, ago, sizeof ago);
    snprintf(snrs, sizeof snrs, "%+.1f", (double)n->snr);
    lv_obj_t *row = lz_row(content, focused);
    lv_obj_set_height(row, MT_ROW_H);
    lv_obj_set_y(row, y);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_pad_column(row, 8, 0);

    lv_obj_t *sc = lz_box(row);
    lv_obj_set_size(sc, 30, 30);
    lv_obj_set_style_radius(sc, 8, 0);
    lv_obj_set_style_bg_color(sc, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_bg_opa(sc, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sc, 1, 0);
    lv_obj_set_style_border_color(sc, lv_color_hex(0x262B33), 0);
    lv_obj_t *scl = lz_text(sc, n->shortcode, LZ_F_SMALL, LZ_MT_BADGE_TXT);
    lv_obj_center(scl);

    lv_obj_t *cl = lz_box(row);
    lv_obj_set_flex_grow(cl, 1);
    lv_obj_set_height(cl, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cl, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cl, 1, 0);
    lv_obj_t *top = lz_box(cl);
    lv_obj_set_size(top, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 5, 0);
    lz_text(top, n->name, LZ_F_BODY, LZ_TEXT);
    role_badge(top, n->role, lv_color_hex(0x8B939C));
    char meta[56];
    snprintf(meta, sizeof meta, "%s - %s - %s",
             n->id, (n->pos_flags & LZ_NODE_POS_VALID) ? "GPS" : n->dist, ago);
    lz_text(cl, meta, LZ_F_SMALL, lv_color_hex(0x838A93));

    lv_obj_t *r = lz_box(row);
    lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lz_text(r, snrs, LZ_F_SMALL, lz_snr_color(n->snr));
    lz_text(r, "SNR", LZ_F_SMALL, LZ_TEXT_3);
    lz_nav_track(row, index + 1);   /* focus 0 is the companion toggle */
    return row;
}

/* companion-mode toggles, drawn into the vlist content panel at the top */
static void mt_companion_header(lv_obj_t *content)
{
    lv_obj_t *crow = lz_row(content, 0 == S.focus);
    lv_obj_set_height(crow, 46);
    lv_obj_set_y(crow, 0);
    lv_obj_set_style_radius(crow, 10, 0);
    lv_obj_set_style_pad_column(crow, 8, 0);
    lz_icon(crow, LZ_I_LAN, &lz_icons_16f, LZ_CYAN);
    lv_obj_t *cc = lz_box(crow);
    lv_obj_set_flex_grow(cc, 1);
    lv_obj_set_height(cc, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cc, LV_FLEX_FLOW_COLUMN);
    lz_text(cc, "Companion mode", LZ_F_BODY, LZ_TEXT);
    lz_text(cc, lz_mtc_active() ? "USB is driving the Meshtastic app"
                                : "Pair with the Meshtastic app over USB",
            LZ_F_SMALL, lv_color_hex(0x838A93));
    lz_toggle(crow, lz_mtc_active(), LZ_TOGGLE_ON);
    lz_nav_track(crow, 0);

    lv_obj_t *brow = lz_row(content, 1 == S.focus);
    lv_obj_set_height(brow, 46);
    lv_obj_set_y(brow, 50);
    lv_obj_set_style_radius(brow, 10, 0);
    lv_obj_set_style_pad_column(brow, 8, 0);
    lz_icon(brow, LZ_I_WIFI, &lz_icons_16f, LZ_CYAN);
    lv_obj_t *bc = lz_box(brow);
    lv_obj_set_flex_grow(bc, 1);
    lv_obj_set_height(bc, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bc, LV_FLEX_FLOW_COLUMN);
    lz_text(bc, "BLE companion", LZ_F_BODY, LZ_TEXT);
    lz_text(bc, lz_mtc_ble_connected() ? "Phone connected wirelessly"
                                       : lz_mtc_ble_enabled() ? "Advertising to the Meshtastic app"
                                                              : "Wireless app link is off",
            LZ_F_SMALL, lv_color_hex(0x838A93));
    lz_toggle(brow, lz_mtc_ble_enabled(), LZ_TOGGLE_ON);
    lz_nav_track(brow, 1);
}

/* ===== Meshtastic ===== */

void lz_scr_meshtastic(lv_obj_t *root)
{
    lv_color_t cyan_lt = LZ_MT_BADGE_TXT;
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x0B0E12), 0);

    colored_navbar(root, "Meshtastic", LZ_NAV_MT, lv_color_hex(0x06242B), S.net_mt);

    /* identity — the real onboarded name + MAC node id, not a placeholder */
    const lz_identity_t *me = lz_svc_identity();
    lv_obj_t *id = identity_card(root, lv_color_hex(0x0F1822), lv_color_hex(0x18222C));
    lv_obj_t *tile = lz_box(id);
    lv_obj_set_size(tile, 34, 34);
    lv_obj_set_style_radius(tile, 10, 0);
    lv_obj_set_style_bg_color(tile, LZ_IDTILE_MT, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_t *jes = lz_text(tile, me->short_name, LZ_F_BODY, LZ_ON_CYAN);
    lv_obj_center(jes);

    lv_obj_t *colm = lz_box(id);
    lv_obj_set_flex_grow(colm, 1);
    lv_obj_set_height(colm, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(colm, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(colm, 1, 0);
    lv_obj_t *nrow = lz_box(colm);
    lv_obj_set_size(nrow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(nrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(nrow, 4, 0);
    char nm[28]; snprintf(nm, sizeof nm, "%s -", me->long_name);
    lz_text(nrow, nm, LZ_F_BODY, LZ_TEXT);
    lz_text(nrow, me->short_name, LZ_F_BODY, cyan_lt);
    char idline[40]; snprintf(idline, sizeof idline, "%s - Region US - LongFast", me->id);
    lz_text(colm, idline, LZ_F_SMALL, LZ_TEXT_2);

    lv_obj_t *right = lz_box(id);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lz_text(right, "nodes", LZ_F_SMALL, LZ_TEXT_3);
    char mtn[6]; snprintf(mtn, sizeof mtn, "%d", lz_svc_node_count(LZ_NET_MT));
    lz_text(right, mtn, LZ_F_TITLE, cyan_lt);

    /* tabs */
    lv_obj_t *tabs = lz_box(root);
    lv_obj_set_size(tabs, LZ_W, 24);
    lv_obj_set_style_border_side(tabs, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(tabs, 1, 0);
    lv_obj_set_style_border_color(tabs, lv_color_hex(0x1B2026), 0);
    const char *names[2] = { "Nodes", "Channels" };
    void (*mt_taps[2])(void) = { tap_mt_nodes, tap_mt_channels };
    for(int i = 0; i < 2; i++) {
        bool act = (S.mt_tab == i);
        lv_obj_t *tb = lz_box(tabs);
        lv_obj_set_size(tb, LZ_W / 2 - 8, 24);
        lv_obj_set_pos(tb, 8 + i * (LZ_W / 2 - 8), 0);
        lv_obj_set_style_border_side(tb, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(tb, 2, 0);
        lv_obj_set_style_border_color(tb, act ? LZ_CYAN : lv_color_hex(0x0B0E12), 0);
        lv_obj_t *lbl = lz_text(tb, names[i], LZ_F_BODY, act ? LZ_TEXT_BRIGHT : LZ_TEXT_META);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);
        lz_on_click(tb, mt_taps[i]);
    }

    lv_obj_t *body = lz_vflex(root);
    lv_obj_set_style_pad_top(body, 5, 0);
    lv_obj_set_style_pad_hor(body, 7, 0);
    lv_obj_set_style_pad_bottom(body, 8, 0);
    lv_obj_set_style_pad_row(body, 3, 0);

    if(S.mt_tab == 0) {
        const lz_node_rt *nodes;
        int nn = lz_svc_nodes(&nodes);
        const lz_identity_t *me = lz_svc_identity();
        vis_count = 0;
        for(int i = 0; i < nn; i++)
            if(nodes[i].net == LZ_NET_MT && nodes[i].num != me->num)
                vis_nodes[vis_count++] = (lz_node_rt *)&nodes[i];

        /* Virtualized: only the on-screen rows (+2 buffer above/below) are ever
         * built as LVGL objects, recycled in place as the list scrolls — so a
         * large mesh can't exhaust the object pool. focus 0/1 = companion rows. */
        lv_obj_t *content = lz_vlist(body, MT_HEADER_H, vis_count, MT_STRIDE,
                                     MT_HEADER_ROWS, mt_node_row_cb, NULL);
        mt_companion_header(content);
        lz_nav_set(1, vis_count + MT_HEADER_ROWS, mt_nodes_activate);
    } else {
        lz_nav_set_scroll(body);
        /* LongFast (Primary) is the live broadcast channel — tap to open it
         * and message everyone nearby. Emergency is shown but not yet wired. */
        const char *names[2] = { "LongFast", "Emergency" };
        const char *subs[2]  = { "Primary - broadcast to everyone", "Encrypted - 12 nodes" };
        for(int i = 0; i < 2; i++) {
            bool primary = (i == 0);
            lv_obj_t *row = lz_row(body, primary && S.focus == 0);
            lv_obj_set_style_radius(row, 10, 0);
            lv_obj_set_style_pad_column(row, 8, 0);
            if(!primary) lv_obj_set_style_opa(row, LV_OPA_50, 0);
            lz_icon(row, primary ? LZ_I_TAG : LZ_I_CAMPAIGN, &lz_icons_18, cyan_lt);
            lv_obj_t *cl = lz_box(row);
            lv_obj_set_flex_grow(cl, 1);
            lv_obj_set_height(cl, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(cl, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(cl, 1, 0);
            lz_text(cl, names[i], LZ_F_BODY, LZ_TEXT);
            lz_text(cl, subs[i], LZ_F_SMALL, lv_color_hex(0x838A93));
            if(primary) {
                lv_obj_t *b = lz_box(row);
                lv_obj_set_size(b, LV_SIZE_CONTENT, 15);
                lv_obj_set_style_radius(b, 5, 0);
                lv_obj_set_style_bg_color(b, LZ_MT_BADGE_BG, 0);
                lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
                lv_obj_set_style_pad_hor(b, 6, 0);
                lv_obj_t *bl = lz_text(b, "PRIMARY", LZ_F_SMALL, cyan_lt);
                lv_obj_center(bl);
                lz_nav_track(row, 0);
            }
        }
        lz_nav_set(1, 1, open_longfast);   /* only the primary channel is openable */
    }
}

/* ===== MeshCore ===== */

void lz_scr_meshcore(lv_obj_t *root)
{
    lv_color_t amber_lt = lv_color_hex(0xE8B25C);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x0E0D0A), 0);

    colored_navbar(root, "MeshCore", LZ_NAV_MC, lv_color_hex(0x2B1D07), S.net_mc);

    if(!LZ_MESHCORE_ENABLED) {                 /* not receive-ready yet — Alpha */
        lv_obj_t *cs = lz_text(root, "Coming soon", LZ_F_HEAD, amber_lt);
        lv_obj_align(cs, LV_ALIGN_CENTER, 0, -10);
        lv_obj_t *sub = lz_text(root, "MeshCore support is in development.", LZ_F_SMALL, LZ_TEXT_3);
        lv_obj_align(sub, LV_ALIGN_CENTER, 0, 14);
        lz_nav_set(1, 0, NULL);
        return;
    }

    lv_obj_t *id = identity_card(root, lv_color_hex(0x1A1710), lv_color_hex(0x241F15));
    lv_obj_t *tile = lz_box(id);
    lv_obj_set_size(tile, 34, 34);
    lv_obj_set_style_radius(tile, 10, 0);
    lv_obj_set_style_bg_color(tile, LZ_IDTILE_MC, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_t *kg = lz_icon(tile, LZ_I_KEY, &lz_icons_16f, LZ_ON_AMBER);
    lv_obj_center(kg);

    lv_obj_t *colm = lz_box(id);
    lv_obj_set_flex_grow(colm, 1);
    lv_obj_set_height(colm, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(colm, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(colm, 1, 0);
    lv_obj_t *nrow = lz_box(colm);
    lv_obj_set_size(nrow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(nrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(nrow, 5, 0);
    lz_text(nrow, lz_svc_identity()->long_name, LZ_F_BODY, LZ_TEXT);
    role_badge(nrow, "Companion", lv_color_hex(0x9A8F7A));
    char mcaddr[24]; lz_backend_mc_addr(mcaddr, sizeof mcaddr);
    lz_text(colm, mcaddr, LZ_F_SMALL, lv_color_hex(0x988E7C));

    lv_obj_t *right = lz_box(id);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lz_text(right, "contacts", LZ_F_SMALL, lv_color_hex(0x82796A));
    char mcn[6]; snprintf(mcn, sizeof mcn, "%d", lz_svc_node_count(LZ_NET_MC));
    lz_text(right, mcn, LZ_F_TITLE, amber_lt);

    /* tabs */
    lv_obj_t *tabs = lz_box(root);
    lv_obj_set_size(tabs, LZ_W, 24);
    lv_obj_set_style_border_side(tabs, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(tabs, 1, 0);
    lv_obj_set_style_border_color(tabs, lv_color_hex(0x1F1B12), 0);
    const char *names[2] = { "Contacts", "Rooms" };
    void (*mc_taps[2])(void) = { tap_mc_contacts, tap_mc_rooms };
    for(int i = 0; i < 2; i++) {
        bool act = (S.mc_tab == i);
        lv_obj_t *tb = lz_box(tabs);
        lv_obj_set_size(tb, LZ_W / 2 - 8, 24);
        lv_obj_set_pos(tb, 8 + i * (LZ_W / 2 - 8), 0);
        lv_obj_set_style_border_side(tb, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(tb, 2, 0);
        lv_obj_set_style_border_color(tb, act ? LZ_AMBER : lv_color_hex(0x0E0D0A), 0);
        lv_obj_t *lbl = lz_text(tb, names[i], LZ_F_BODY, act ? LZ_TEXT_BRIGHT : LZ_TEXT_META);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);
        lz_on_click(tb, mc_taps[i]);
    }

    lv_obj_t *body = lz_vflex(root);
    lv_obj_set_style_pad_top(body, 5, 0);
    lv_obj_set_style_pad_hor(body, 7, 0);
    lv_obj_set_style_pad_bottom(body, 8, 0);
    lv_obj_set_style_pad_row(body, 3, 0);
    lz_nav_set_scroll(body);

    /* --- self-advertise (only when MeshCore is enabled) --- */
    int adv = mc_adv_rows();
    if(adv) {
        const char *labels[2] = { "Advertise to neighbors", "Flood advert (whole mesh)" };
        const char *ics[2]    = { LZ_I_HUB, LZ_I_LAN };
        for(int b = 0; b < 2; b++) {
            lv_obj_t *btn = lz_row(body, b == S.focus);
            lv_obj_set_style_radius(btn, 10, 0);
            lv_obj_set_style_pad_column(btn, 8, 0);
            lz_icon(btn, ics[b], &lz_icons_16f, LZ_AMBER);
            lv_obj_t *t = lz_text(btn, labels[b], LZ_F_BODY, LZ_TEXT);
            lv_obj_set_flex_grow(t, 1);
            lz_nav_track(btn, b);
        }
        if(g_mc_note) {
            lv_obj_t *note = lz_text(body, g_mc_note, LZ_F_SMALL, LZ_AMBER);
            lv_obj_set_style_pad_left(note, 4, 0);
            lv_obj_set_style_pad_bottom(note, 2, 0);
        }
    }

    const lz_node_rt *nodes;
    int nn = lz_svc_nodes(&nodes);
    vis_count = 0;
    for(int i = 0; i < nn; i++) {
        const lz_node_rt *n = &nodes[i];
        if(n->net != LZ_NET_MC) continue;
        bool is_room = strcmp(n->role, "Room") == 0;
        if((S.mc_tab == 1) != is_room) continue;
        vis_nodes[vis_count++] = (lz_node_rt *)n;
    }

    for(int i = 0; i < vis_count; i++) {
        lz_node_rt *n = vis_nodes[i];
        char ago[8]; lz_fmt_ago(n->last_heard, ago, sizeof ago);
        lv_obj_t *row = lz_row(body, i + adv == S.focus);   /* advert rows (if any) come first */
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_column(row, 8, 0);

        lv_obj_t *tl = lz_box(row);
        lv_obj_set_size(tl, 30, 30);
        lv_obj_set_style_radius(tl, 8, 0);
        lv_obj_set_style_bg_color(tl, LZ_MC_ROWTILE, 0);
        lv_obj_set_style_bg_opa(tl, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(tl, 1, 0);
        lv_obj_set_style_border_color(tl, lv_color_hex(0x2E2719), 0);
        const char *ic = strcmp(n->role, "Repeater") == 0 ? LZ_I_ROUTER
                       : strcmp(n->role, "Room") == 0     ? LZ_I_FORUM
                       : strcmp(n->role, "Sensor") == 0   ? LZ_I_SENSORS
                                                          : LZ_I_PERSON;
        lv_obj_t *icl = lz_icon(tl, ic, &lz_icons_16f, LZ_AMBER);
        lv_obj_center(icl);

        lv_obj_t *cl = lz_box(row);
        lv_obj_set_flex_grow(cl, 1);
        lv_obj_set_height(cl, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cl, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(cl, 1, 0);
        lv_obj_t *top = lz_box(cl);
        lv_obj_set_size(top, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(top, 5, 0);
        lz_text(top, n->name, LZ_F_BODY, LZ_TEXT);
        role_badge(top, n->role, lv_color_hex(0xB0A48D));
        char meta[56];
        snprintf(meta, sizeof meta, "%s - %s - %s", n->id, n->dist, ago);
        lz_text(cl, meta, LZ_F_SMALL, lv_color_hex(0x8A8475));

        bool online = strcmp(ago, "now") == 0 || strchr(ago, 'm') != NULL;
        lz_dot(row, 7, online ? LZ_GREEN : lv_color_hex(0x4A515B));
        lz_nav_track(row, i + adv);
    }
    if(vis_count == 0) {
        const char *msg = !S.net_mc ? "MeshCore is off — enable it in Settings."
                        : S.mc_tab == 1 ? "No MeshCore rooms heard yet."
                        : "No MeshCore nodes heard yet — advertise to announce yourself.";
        lv_obj_t *empty = lz_text(body, msg, LZ_F_SMALL, LZ_TEXT_3);
        lv_obj_set_style_pad_top(empty, 8, 0);
        lv_obj_set_style_pad_left(empty, 4, 0);
    }
    lz_nav_set(1, vis_count + adv, mc_activate);
}
