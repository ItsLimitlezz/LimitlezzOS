#include "ui.h"
#include <string.h>

lz_state_t S;

static lv_obj_t *g_root;
static int g_cols = 1;
static int g_count = 0;
static void (*g_activate)(int idx);
static lv_obj_t *g_scroll;
static lv_obj_t *g_focus_obj;   /* object to scroll into view */

/* ================= engine ================= */

void lz_ui_init(lv_obj_t *root)
{
    memset(&S, 0, sizeof(S));
    S.view = LZ_V_LOCK;
    S.net_mt = true;
    S.net_mc = true;
    S.settings.gps = true;
    S.settings.dark = true;
    S.settings.save = true;
    S.settings.bright = 74;
    S.settings.timeout = 1;   /* 30s */
    g_root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LZ_W, LZ_H);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lz_rebuild();
}

void lz_nav_set(int cols, int count, void (*activate)(int idx))
{
    g_cols = cols > 0 ? cols : 1;
    g_count = count;
    g_activate = activate;
}

void lz_nav_set_scroll(lv_obj_t *scroll) { g_scroll = scroll; }

/* Tap dispatch is deferred with lv_async_call: activating usually rebuilds
 * the screen, and deleting the pressed object from inside its own event is
 * not safe in LVGL 8. */
static void tap_item_async(void *p)
{
    int idx = (int)(intptr_t)p;
    if(idx < 0 || idx >= g_count) return;
    S.focus = idx;
    if(g_activate) g_activate(idx);
    else lz_rebuild();          /* no action: focus just follows the tap */
}

static void tap_item_cb(lv_event_t *e)
{
    lv_async_call(tap_item_async, lv_event_get_user_data(e));
}

static void tap_fn_async(void *p) { ((void (*)(void))p)(); }

static void tap_fn_cb(lv_event_t *e)
{
    lv_async_call(tap_fn_async, lv_event_get_user_data(e));
}

void lz_on_click(lv_obj_t *obj, void (*fn)(void))
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, tap_fn_cb, LV_EVENT_CLICKED, (void *)fn);
}

void lz_nav_track(lv_obj_t *obj, int idx)
{
    if(idx == S.focus) g_focus_obj = obj;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, tap_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
}

void lz_rebuild(void)
{
    g_cols = 1; g_count = 0; g_activate = NULL; g_scroll = NULL; g_focus_obj = NULL;
    lv_obj_clean(g_root);
    /* screens style the root itself (flex flow, bg) — reset it fully so
     * layout from the previous screen can't leak into the next build */
    lv_obj_remove_style_all(g_root);
    lv_obj_set_size(g_root, LZ_W, LZ_H);
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(g_root, LZ_SCREEN_BG, 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    switch(S.view) {
        case LZ_V_LOCK:       lz_scr_lock(g_root); break;
        case LZ_V_HOME:       lz_scr_home(g_root); break;
        case LZ_V_MESSAGES:   lz_scr_messages(g_root); break;
        case LZ_V_CONVO:      lz_scr_convo(g_root); break;
        case LZ_V_MESHTASTIC: lz_scr_meshtastic(g_root); break;
        case LZ_V_MESHCORE:   lz_scr_meshcore(g_root); break;
        case LZ_V_APPSTORE:   lz_scr_appstore(g_root); break;
        case LZ_V_CONTACTS:   lz_scr_contacts(g_root); break;
        case LZ_V_CONTACT:    lz_scr_contact(g_root); break;
        case LZ_V_SETTINGS:   lz_scr_settings(g_root); break;
        case LZ_V_SYSTEM:     lz_scr_system(g_root); break;
        case LZ_V_TERMINAL:   lz_scr_terminal(g_root); break;
        case LZ_V_FILES:      lz_scr_files(g_root); break;
        default: break;
    }
    if(g_focus_obj && g_scroll) {
        lv_obj_update_layout(g_scroll);
        lv_obj_scroll_to_view(g_focus_obj, LV_ANIM_OFF);
    } else if(S.view == LZ_V_CONVO && g_scroll) {
        /* thread opens (and stays) pinned to the latest message */
        lv_obj_update_layout(g_scroll);
        lv_obj_scroll_to_y(g_scroll, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

void lz_go(lz_view_t v)
{
    if(S.nav_depth < (int)(sizeof(S.nav_stack)/sizeof(S.nav_stack[0])))
        S.nav_stack[S.nav_depth++] = S.view;
    S.view = v;
    S.focus = 0;
    lz_rebuild();
}

void lz_back(void)
{
    lz_view_t v = LZ_V_HOME;
    if(S.nav_depth > 0) v = S.nav_stack[--S.nav_depth];
    if(v == LZ_V_LOCK) v = LZ_V_HOME;
    S.view = v;
    S.focus = 0;
    lz_rebuild();
}

static void unlock(void)
{
    S.nav_depth = 0;
    S.view = LZ_V_HOME;
    S.focus = 0;
    lz_rebuild();
}

static void move(lz_key_t dir)
{
    /* brightness slider: left/right adjust while focused */
    if(S.view == LZ_V_SETTINGS && lz_settings_slider_focused() &&
       (dir == LZ_K_LEFT || dir == LZ_K_RIGHT)) {
        lz_settings_bright_adjust(dir == LZ_K_RIGHT ? 6 : -6);
        lz_rebuild();
        return;
    }
    /* tab switching with left/right on single-column tabbed screens */
    if(dir == LZ_K_LEFT || dir == LZ_K_RIGHT) {
        int d = (dir == LZ_K_RIGHT) ? 1 : -1;
        if(S.view == LZ_V_MESSAGES) {
            int t = (int)S.msg_tab + d;
            if(t >= 0 && t <= 1 && t != (int)S.msg_tab) { S.msg_tab = (lz_msg_tab_t)t; S.focus = 0; lz_rebuild(); }
            return;
        }
        if(S.view == LZ_V_MESHTASTIC) {
            int t = S.mt_tab + d;
            if(t >= 0 && t <= 1 && t != S.mt_tab) { S.mt_tab = t; S.focus = 0; lz_rebuild(); }
            return;
        }
        if(S.view == LZ_V_MESHCORE) {
            int t = S.mc_tab + d;
            if(t >= 0 && t <= 1 && t != S.mc_tab) { S.mc_tab = t; S.focus = 0; lz_rebuild(); }
            return;
        }
    }
    if(g_count == 0) {
        /* no focusables: up/down scroll the list (terminal, conversation) */
        if(g_scroll) {
            int dy = dir == LZ_K_DOWN ? 60 : dir == LZ_K_UP ? -60 : 0;
            if(dy) lv_obj_scroll_by(g_scroll, 0, -dy, LV_ANIM_OFF);
        }
        return;
    }
    int f = S.focus, nf = f;
    if(dir == LZ_K_UP)         nf = f - g_cols;
    else if(dir == LZ_K_DOWN)  nf = f + g_cols;
    else if(dir == LZ_K_LEFT)  { if(f % g_cols > 0) nf = f - 1; }
    else if(dir == LZ_K_RIGHT) { if(f % g_cols < g_cols - 1 && f + 1 < g_count) nf = f + 1; }
    if(nf < 0 || nf >= g_count) nf = f;
    if(nf != f) { S.focus = nf; lz_rebuild(); }
}

static void activate(void)
{
    if(S.view == LZ_V_LOCK) { unlock(); return; }
    if(g_activate && S.focus >= 0 && S.focus < g_count) g_activate(S.focus);
}

static void convo_send(void)
{
    size_t len = strlen(S.draft);
    while(len && S.draft[len-1] == ' ') S.draft[--len] = 0;
    if(!len || S.sent_count >= LZ_SENT_MAX) return;
    strncpy(S.sent[S.sent_count], S.draft, LZ_DRAFT_MAX - 1);
    S.sent[S.sent_count][LZ_DRAFT_MAX - 1] = 0;
    S.sent_count++;
    S.draft[0] = 0;
    lz_rebuild();
    if(g_scroll) {
        lv_obj_update_layout(g_scroll);
        lv_obj_scroll_to_y(g_scroll, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

void lz_ui_key(lz_key_t k, char c)
{
    switch(k) {
        case LZ_K_UP: case LZ_K_DOWN: case LZ_K_LEFT: case LZ_K_RIGHT:
            move(k);
            return;
        case LZ_K_ENTER:
            if(S.view == LZ_V_CONVO) { convo_send(); return; }
            activate();
            return;
        case LZ_K_BACK:
            if(S.view == LZ_V_LOCK) return;
            if(S.view == LZ_V_CONVO && S.draft[0]) {       /* backspace edits draft first */
                S.draft[strlen(S.draft) - 1] = 0;
                lz_rebuild();
                return;
            }
            lz_back();
            return;
        case LZ_K_CHAR:
            if(S.view == LZ_V_MESSAGES) {                  /* 1/2/3 network filter */
                lz_filter_t f = S.msg_filter;
                if(c == '1') f = LZ_FILT_ALL;
                else if(c == '2') f = LZ_FILT_MT;
                else if(c == '3') f = LZ_FILT_MC;
                if(f != S.msg_filter) { S.msg_filter = f; S.focus = 0; lz_rebuild(); }
                return;
            }
            if(S.view == LZ_V_CONVO && c >= 32 && c < 127) {
                size_t len = strlen(S.draft);
                if(len < LZ_DRAFT_MAX - 1) { S.draft[len] = c; S.draft[len+1] = 0; lz_rebuild(); }
            }
            return;
    }
}

/* ================= shared widgets ================= */

lv_obj_t *lz_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    /* visual primitive: must not steal taps from the row/handler above it */
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *lz_text(lv_obj_t *parent, const char *txt, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

lv_obj_t *lz_icon(lv_obj_t *parent, const char *glyph, const lv_font_t *font, lv_color_t color)
{
    return lz_text(parent, glyph, font, color);
}

lv_obj_t *lz_navbar(lv_obj_t *parent, const char *title, const char *back_label)
{
    lv_obj_t *bar = lz_box(parent);
    lv_obj_set_size(bar, LZ_W, LZ_NAVBAR_H);
    lv_obj_set_style_bg_color(bar, LZ_NAVBAR_BG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, LZ_HAIRLINE, 0);

    lv_obj_t *chev = lz_icon(bar, LZ_I_CHEV_L, &lz_icons_18, LZ_TEXT_NAV);
    lv_obj_align(chev, LV_ALIGN_LEFT_MID, 5, 0);
    if(back_label) {
        lv_obj_t *bl = lz_text(bar, back_label, LZ_F_SMALL, lv_color_hex(0xCFD4DA));
        lv_obj_align(bl, LV_ALIGN_LEFT_MID, 24, 0);
    }
    if(title) {
        lv_obj_t *t = lz_text(bar, title, LZ_F_HEAD, lv_color_hex(0xF2F4F6));
        lv_obj_align(t, LV_ALIGN_CENTER, 0, 0);
    }
    /* invisible back hit area over the chevron */
    lv_obj_t *hit = lz_box(bar);
    lv_obj_set_size(hit, 64, LZ_NAVBAR_H);
    lv_obj_set_pos(hit, 0, 0);
    lz_on_click(hit, lz_back);
    return bar;
}

lv_obj_t *lz_row(lv_obj_t *parent, bool focused)
{
    lv_obj_t *row = lz_box(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(row, LZ_RADIUS_ROW, 0);
    lv_obj_set_style_bg_color(row, focused ? LZ_ROW_FOCUS_BG : LZ_ROW_BG, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, focused ? LZ_FOCUS_RING_W : 1, 0);
    lv_obj_set_style_border_color(row, focused ? LZ_FOCUS : LZ_ROW_BORDER, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, 9, 0);
    lv_obj_set_style_pad_ver(row, 7, 0);
    lv_obj_set_style_pad_column(row, 9, 0);
    return row;
}

lv_obj_t *lz_card(lv_obj_t *parent)
{
    lv_obj_t *c = lz_box(parent);
    lv_obj_set_width(c, lv_pct(100));
    lv_obj_set_style_radius(c, LZ_RADIUS_CARD, 0);
    lv_obj_set_style_bg_color(c, LZ_CARD_BG, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, LZ_CARD_BORDER, 0);
    return c;
}

lv_obj_t *lz_dot(lv_obj_t *parent, int size, lv_color_t color)
{
    lv_obj_t *d = lz_box(parent);
    lv_obj_set_size(d, size, size);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, color, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    return d;
}

lv_obj_t *lz_toggle(lv_obj_t *parent, bool on, lv_color_t on_color)
{
    lv_obj_t *t = lz_box(parent);
    lv_obj_set_size(t, 38, 22);
    lv_obj_set_style_radius(t, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(t, on ? on_color : LZ_TRACK_OFF, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_t *k = lz_dot(t, 18, LZ_KNOB);
    lv_obj_align(k, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -2 : 2, 0);
    return t;
}

lv_obj_t *lz_vflex(lv_obj_t *parent)
{
    lv_obj_t *body = lv_obj_create(parent);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LZ_W);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    return body;
}

/* Status bar: drawn entirely as primitives (spec §6.6) */
void lz_status_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lz_box(parent);
    lv_obj_set_size(bar, LZ_W, LZ_STATUSBAR_H);
    lv_obj_set_style_bg_color(bar, LZ_STATUSBAR_BG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x0A0D11), 0);

    lv_color_t mint = lv_color_hex(0x5FE3B3);

    lv_obj_t *hub = lz_icon(bar, LZ_I_HUB, &lz_icons_14, mint);
    lv_obj_align(hub, LV_ALIGN_LEFT_MID, 9, 0);
    lv_obj_t *nodes = lz_text(bar, "7 nodes", LZ_F_MONO, lv_color_hex(0xAEB6BF));
    lv_obj_align(nodes, LV_ALIGN_LEFT_MID, 27, 0);

    /* battery: outline + fill */
    lv_obj_t *batt = lz_box(bar);
    lv_obj_set_size(batt, 18, 9);
    lv_obj_set_style_radius(batt, 2, 0);
    lv_obj_set_style_border_width(batt, 1, 0);
    lv_obj_set_style_border_color(batt, lv_color_hex(0x767D86), 0);
    lv_obj_align(batt, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_t *fill = lz_box(batt);
    lv_obj_set_size(fill, 12, 5);
    lv_obj_set_style_bg_color(fill, mint, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_align(fill, LV_ALIGN_LEFT_MID, 1, 0);
    lv_obj_t *nub = lz_box(bar);
    lv_obj_set_size(nub, 2, 4);
    lv_obj_set_style_bg_color(nub, lv_color_hex(0x767D86), 0);
    lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
    lv_obj_align(nub, LV_ALIGN_RIGHT_MID, -9, 0);

    lv_obj_t *clock = lz_text(bar, "14:23", LZ_F_MONO, lv_color_hex(0xCDD3DA));
    lv_obj_align(clock, LV_ALIGN_RIGHT_MID, -38, 0);

    /* signal bars */
    static const int hs[4] = { 4, 6, 8, 11 };
    for(int i = 0; i < 4; i++) {
        lv_obj_t *b = lz_box(bar);
        lv_obj_set_size(b, 3, hs[i]);
        lv_obj_set_style_radius(b, 1, 0);
        lv_obj_set_style_bg_color(b, i < 3 ? mint : lv_color_hex(0x3D434B), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, -(85 - i * 5), (11 - hs[i]) / 2 + 0);
        lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -(85 - i * 5), -5);
    }
}

bool lz_settings_slider_focused(void)
{
    /* settings focus order: 0 Meshtastic, 1 MeshCore, then rows; brightness is row idx 7 */
    return S.focus == 7;
}

void lz_settings_bright_adjust(int delta)
{
    int b = S.settings.bright + delta;
    if(b < 5) b = 5;
    if(b > 100) b = 100;
    S.settings.bright = b;
}
