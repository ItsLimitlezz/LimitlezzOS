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

/* ===== Contacts =====
 * Only people the user purposely added — not every node ever heard. Add a
 * contact from its detail page (reached via the network managers). */

static lz_node_rt *contact_list[LZ_MAX_NODES];
static int contact_n;

static void contacts_activate(int idx)
{
    if(idx >= 0 && idx < contact_n) {
        S.contact_sel = contact_list[idx];
        lz_go(LZ_V_CONTACT);
    }
}

void lz_scr_contacts(lv_obj_t *root)
{
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lz_navbar(root, "Contacts", NULL);

    const lz_node_rt *nodes;
    int nn = lz_svc_nodes(&nodes);
    contact_n = 0;
    for(int i = 0; i < nn; i++)
        if(nodes[i].contact) contact_list[contact_n++] = (lz_node_rt *)&nodes[i];

    lv_obj_t *body = lz_vflex(root);
    lv_obj_set_style_pad_top(body, 5, 0);
    lv_obj_set_style_pad_hor(body, 7, 0);
    lv_obj_set_style_pad_bottom(body, 8, 0);
    lv_obj_set_style_pad_row(body, 3, 0);
    lz_nav_set_scroll(body);

    if(contact_n == 0) {
        lv_obj_t *empty = lz_text(body,
            "No contacts yet.\nOpen a node in Meshtastic or MeshCore\nand tap Add contact.",
            LZ_F_SMALL, LZ_TEXT_3);
        lv_obj_set_width(empty, lv_pct(100));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(empty, 60, 0);
        lz_nav_set(1, 0, NULL);
        return;
    }

    for(int i = 0; i < contact_n; i++) {
        lz_node_rt *n = contact_list[i];
        char ago[8], snrs[8];
        lz_fmt_ago(n->last_heard, ago, sizeof ago);
        snprintf(snrs, sizeof snrs, "%+.1f", (double)n->snr);
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
        lz_text(r, ago, LZ_F_SMALL, LZ_TEXT_META);
        lz_text(r, snrs, LZ_F_SMALL, lz_snr_color(n->snr));
        lz_nav_track(row, i);
    }
    lz_nav_set(1, contact_n, contacts_activate);
}

/* ===== Contact detail ===== */

static void contact_activate(int idx)
{
    lz_node_rt *n = S.contact_sel;
    if(!n) return;
    bool messageable = lz_node_messageable(n);
    if(idx == 0 && messageable) {
        lz_thread_rt *t = lz_svc_thread_for_node(n);
        lz_open_convo(t);
        return;
    }
    if(idx == 0 && !messageable) return;          /* infra: no Message action */
    /* Add-contact / Trace button */
    if(idx == 1) {
        if(!n->contact) lz_svc_add_contact(n);
        else lz_rebuild();                         /* Trace: no-op for now */
    }
}

void lz_scr_contact(lv_obj_t *root)
{
    lz_node_rt *n = S.contact_sel;
    if(!n) { lz_back(); return; }
    bool messageable = lz_node_messageable(n);
    char ago[8], snrs[8];
    lz_fmt_ago(n->last_heard, ago, sizeof ago);
    snprintf(snrs, sizeof snrs, "%+.1f", (double)n->snr);
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

    /* actions row. Messageable people get a Message button + Add/Saved;
     * infrastructure (Router/Repeater/Sensor/Room) gets Add/Saved + Trace,
     * never a Message button. */
    lv_obj_t *actions = lz_box(body);
    lv_obj_set_width(actions, lv_pct(100));
    lv_obj_set_height(actions, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(actions, 7, 0);
    lv_obj_set_style_pad_ver(actions, 12, 0);

    bool f0 = S.focus == 0, f1 = S.focus == 1;

    /* shared button builder */
    #define LZ_BTN(var, grow, w, bg, focused) \
        lv_obj_t *var = lz_box(actions); \
        if(grow) lv_obj_set_flex_grow(var, 1); else lv_obj_set_width(var, w); \
        lv_obj_set_height(var, 31); \
        lv_obj_set_style_radius(var, 10, 0); \
        lv_obj_set_style_bg_color(var, bg, 0); \
        lv_obj_set_style_bg_opa(var, LV_OPA_COVER, 0); \
        lv_obj_set_flex_flow(var, LV_FLEX_FLOW_ROW); \
        lv_obj_set_flex_align(var, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER); \
        lv_obj_set_style_pad_column(var, 5, 0); \
        if(focused) { lv_obj_set_style_outline_width(var, 2, 0); \
                      lv_obj_set_style_outline_color(var, LZ_FOCUS, 0); }

    if(messageable) {
        LZ_BTN(msg, true, 0, LZ_TILE_165, f0);
        lz_icon(msg, LZ_I_CHAT, &lz_icons_16f, LZ_ON_MINT);
        lz_text(msg, "Message", LZ_F_BODY, LZ_ON_MINT);
        lz_nav_track(msg, 0);

        LZ_BTN(add, false, 96, n->contact ? lv_color_hex(0x1A2520) : lv_color_hex(0x20242B), f1);
        lv_obj_set_style_border_width(add, 1, 0);
        lv_obj_set_style_border_color(add, n->contact ? LZ_GREEN_BG : lv_color_hex(0x2D323A), 0);
        lz_icon(add, LZ_I_GROUP, &lz_icons_16f, n->contact ? LZ_GREEN_TXT : lv_color_hex(0xCFD4DA));
        lz_text(add, n->contact ? "Saved" : "Add", LZ_F_SMALL,
                n->contact ? LZ_GREEN_TXT : lv_color_hex(0xCFD4DA));
        lz_nav_track(add, 1);
    } else {
        LZ_BTN(add, true, 0, n->contact ? lv_color_hex(0x1A2520) : LZ_TILE_242, f0);
        if(n->contact) {
            lv_obj_set_style_border_width(add, 1, 0);
            lv_obj_set_style_border_color(add, LZ_GREEN_BG, 0);
        }
        lz_icon(add, LZ_I_GROUP, &lz_icons_16f, n->contact ? LZ_GREEN_TXT : lv_color_white());
        lz_text(add, n->contact ? "Contact saved" : "Add contact", LZ_F_BODY,
                n->contact ? LZ_GREEN_TXT : lv_color_white());
        lz_nav_track(add, 0);

        LZ_BTN(trace, false, 41, lv_color_hex(0x20242B), f1);
        lv_obj_set_style_border_width(trace, 1, 0);
        lv_obj_set_style_border_color(trace, lv_color_hex(0x2D323A), 0);
        lz_icon(trace, LZ_I_ROUTE, &lz_icons_16f, lv_color_hex(0xCFD4DA));
        lz_nav_track(trace, 1);
    }
    #undef LZ_BTN

    /* spec table */
    lv_obj_t *card = lz_card(body);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    char batt[8];
    if(n->batt >= 0) snprintf(batt, sizeof batt, "%d%%", n->batt);
    else snprintf(batt, sizeof batt, "-");
    const char *ks[6] = { "Node ID", "Hardware", "Distance", "SNR", "Battery", "Last heard" };
    const char *vs[6] = { n->id, n->hw, n->dist, snrs, batt, ago };
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

/* ---- interactive serial console ----
 * A real shell over the mesh service: type a command, Enter runs it, output
 * appends to the scrollback. Same commands work over USB serial on hardware. */
#define TERM_MAX   80
#define TERM_COLS  44
static char    g_term[TERM_MAX][64];
static uint8_t g_term_kind[TERM_MAX];   /* 0 dim, 1 cmd, 2 out, 3 err */
static int     g_term_n;
static char    g_term_in[TERM_COLS];
static bool    g_term_seeded;

static void term_put(uint8_t kind, const char *s)
{
    if(g_term_n >= TERM_MAX) {           /* scroll the ring up by one */
        memmove(g_term[0], g_term[1], sizeof(g_term[0]) * (TERM_MAX - 1));
        memmove(g_term_kind, g_term_kind + 1, TERM_MAX - 1);
        g_term_n = TERM_MAX - 1;
    }
    snprintf(g_term[g_term_n], sizeof g_term[0], "%s", s);
    g_term_kind[g_term_n] = kind;
    g_term_n++;
}

static void term_seed(void)
{
    g_term_n = 0;
    term_put(0, "LimitlezzOS 1.0  -  serial console");
    term_put(0, "type 'help' for commands");
    g_term_seeded = true;
}

static void term_run(const char *cmd)
{
    char echo[64];
    snprintf(echo, sizeof echo, "limitlezz:~$ %s", cmd);
    term_put(1, echo);

    /* trim leading spaces */
    while(*cmd == ' ') cmd++;

    if(cmd[0] == 0) {
        /* blank line */
    } else if(strcmp(cmd, "help") == 0) {
        term_put(2, "commands:");
        term_put(2, "  info     owner, networks, region");
        term_put(2, "  nodes    heard nodes + SNR");
        term_put(2, "  airtime  tx / rx / utilization");
        term_put(2, "  whoami   this node's identity");
        term_put(2, "  clear    wipe the screen");
    } else if(strcmp(cmd, "info") == 0 || strcmp(cmd, "mesh --info") == 0) {
        const lz_identity_t *id = lz_svc_identity();
        char l[64];
        snprintf(l, sizeof l, "owner : %s (%s)", id->long_name, id->short_name); term_put(2, l);
        snprintf(l, sizeof l, "nets  : meshtastic[on] meshcore[%s]",
                 LZ_MESHCORE_ENABLED ? "on" : "soon"); term_put(2, l);
        snprintf(l, sizeof l, "nodes : %d reachable", lz_svc_node_count(LZ_NET_MT)); term_put(2, l);
        term_put(2, "region: US   preset: LONG_FAST");
    } else if(strcmp(cmd, "nodes") == 0 || strcmp(cmd, "mesh --nodes") == 0) {
        const lz_node_rt *ns; int n = lz_svc_nodes(&ns);
        const lz_identity_t *me = lz_svc_identity();
        int shown = 0;
        for(int i = 0; i < n; i++) {
            if(ns[i].net != LZ_NET_MT || ns[i].num == me->num) continue;
            char l[64];
            snprintf(l, sizeof l, "%-4s %+5.1f  %-7s %s",
                     ns[i].shortcode, (double)ns[i].snr, ns[i].role, ns[i].name);
            term_put(2, l); shown++;
        }
        if(!shown) term_put(2, "no nodes heard yet");
    } else if(strcmp(cmd, "airtime") == 0 || strcmp(cmd, "mesh --airtime") == 0) {
        lz_radio_stats_t st; lz_svc_radio_stats(&st);
        char l[64];
        snprintf(l, sizeof l, "tx %u  rx %u  util %.1f%%",
                 (unsigned)st.tx_count, (unsigned)st.rx_count, (double)st.util_pct);
        term_put(2, l);
    } else if(strcmp(cmd, "whoami") == 0) {
        const lz_identity_t *id = lz_svc_identity();
        char l[64];
        snprintf(l, sizeof l, "%s / %s / %s", id->long_name, id->short_name, id->id);
        term_put(2, l);
    } else if(strcmp(cmd, "clear") == 0) {
        g_term_n = 0;
    } else {
        char l[64];
        snprintf(l, sizeof l, "%s: command not found (try 'help')", cmd);
        term_put(3, l);
    }
}

void lz_term_key(lz_key_t k, char c)
{
    if(k == LZ_K_ENTER) {
        term_run(g_term_in);
        g_term_in[0] = 0;
        lz_rebuild();
    } else if(k == LZ_K_BACK) {
        if(g_term_in[0]) { g_term_in[strlen(g_term_in) - 1] = 0; lz_rebuild(); }
        else lz_back();
    } else if(k == LZ_K_CHAR && c >= 32 && c < 127) {
        size_t len = strlen(g_term_in);
        if(len < TERM_COLS - 1) { g_term_in[len] = c; g_term_in[len + 1] = 0; lz_rebuild(); }
    } else if(k == LZ_K_UP || k == LZ_K_DOWN) {
        lz_ui_key(k, 0);   /* let the engine scroll the scrollback */
    }
}

void lz_scr_terminal(lv_obj_t *root)
{
    if(!g_term_seeded) term_seed();

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
    lv_obj_set_style_pad_row(body, 3, 0);
    lz_nav_set_scroll(body);

    for(int i = 0; i < g_term_n; i++) {
        lv_color_t c = g_term_kind[i] == 0 ? LZ_TERM_DIM
                     : g_term_kind[i] == 1 ? LZ_TERM_CMD
                     : g_term_kind[i] == 3 ? LZ_SNR_BAD : LZ_TERM_GREEN;
        lv_obj_t *ln = lz_text(body, g_term[i], LZ_F_MONO, c);
        lv_label_set_long_mode(ln, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(ln, lv_pct(100));
    }

    /* live input line: prompt + typed text + blinking block cursor */
    lv_obj_t *prompt = lz_box(body);
    lv_obj_set_width(prompt, lv_pct(100));
    lv_obj_set_height(prompt, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(prompt, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(prompt, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(prompt, 1, 0);
    lz_text(prompt, "limitlezz:~$", LZ_F_MONO, LZ_TERM_GREEN);
    if(g_term_in[0]) lz_text(prompt, g_term_in, LZ_F_MONO, lv_color_hex(0xCFD4DA));
    lv_obj_t *cur = lz_box(prompt);
    lv_obj_set_size(cur, 6, 12);
    lv_obj_set_style_bg_color(cur, LZ_TERM_GREEN, 0);
    lv_obj_set_style_bg_opa(cur, LV_OPA_COVER, 0);
    lv_timer_t *tm = lv_timer_create(blink_cb, 530, cur);
    lv_obj_add_event_cb(cur, blink_del_cb, LV_EVENT_DELETE, tm);

    lz_nav_set(1, 0, NULL);  /* typing handled by lz_term_key; up/down scroll */
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
