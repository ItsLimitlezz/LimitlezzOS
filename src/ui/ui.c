#include "ui.h"
#include "vlist.h"
#include <string.h>
#include <stdio.h>

extern uint32_t lz_tick_ms(void);

/* ---- screen timeout / backlight ---- */
static uint32_t g_last_activity;
static bool     g_dimmed;
static void   (*g_backlight_cb)(int pct);

/* Sleep-after options mirror Settings: 15s / 30s / 1m / 5m / Never */
static const uint32_t TIMEOUT_MS[5] = { 15000, 30000, 60000, 300000, 0 };

void lz_set_backlight_cb(void (*fn)(int pct)) { g_backlight_cb = fn; }

/* the left-roll back gesture requires a firm/repeated left (two within ~550ms)
 * so a gentle accidental nudge at an edge doesn't exit the screen */
static bool confirm_left_back(void)
{
    static uint32_t last;
    static int cnt;
    uint32_t now = lz_tick_ms();
    cnt = (now - last < 550) ? cnt + 1 : 1;
    last = now;
    if(cnt >= 2) { cnt = 0; return true; }
    return false;
}

void lz_apply_brightness(void)
{
    if(g_backlight_cb && !g_dimmed) g_backlight_cb(S.settings.bright);
}

bool lz_is_dimmed(void) { return g_dimmed; }

void lz_note_activity(void)
{
    g_last_activity = lz_tick_ms();
    if(g_dimmed) {                       /* woke from sleep: restore brightness */
        g_dimmed = false;
        if(g_backlight_cb) g_backlight_cb(S.settings.bright);
    }
}

void lz_idle_tick(void)
{
    if(S.view == LZ_V_ONBOARD) return;            /* don't sleep mid-setup */
    uint32_t to = TIMEOUT_MS[S.settings.timeout < 5 ? S.settings.timeout : 1];
    if(to == 0 || g_dimmed) return;               /* "Never" or already asleep */
    if(lz_tick_ms() - g_last_activity < to) return;
    g_dimmed = true;
    if(g_backlight_cb) g_backlight_cb(0);          /* screen off */
    if(S.view != LZ_V_LOCK) {                      /* lock so a key wakes to lock */
        S.nav_depth = 0;
        S.view = LZ_V_LOCK;
        S.focus = 0;
        lz_rebuild();
    }
}

lz_state_t S;

/* push the network enable toggles to the radio backend's TDM scheduler:
 * both on -> configured split, one on -> 100%, neither -> idle */
void lz_apply_networks(void) { lz_backend_set_networks(S.net_mt, S.net_mc); }

static lv_obj_t *g_root;
static int g_cols = 1;
static int g_count = 0;
static void (*g_activate)(int idx);
static bool (*g_skip)(int idx);  /* indices the focus ring must skip over */
static lv_obj_t *g_scroll;
static lv_obj_t *g_focus_obj;   /* object to scroll into view */
static int  g_convo_scroll_y;
static bool g_convo_stick_bottom = true;
static bool g_convo_force_bottom = true;
static bool g_convo_skip_capture;
static bool g_bright_dirty;      /* brightness adjusted but not yet written to SD */

static int clamp_i(int v, int lo, int hi)
{
    if(v < lo) return lo;
    if(v > hi) return hi;
    return v;
}

static void convo_scroll_capture(void)
{
    if(g_convo_skip_capture) {
        g_convo_skip_capture = false;
        return;
    }
    if(S.view != LZ_V_CONVO || !g_scroll) return;
    lv_obj_update_layout(g_scroll);
    g_convo_scroll_y = lv_obj_get_scroll_y(g_scroll);
    g_convo_stick_bottom = lv_obj_get_scroll_bottom(g_scroll) <= 4;
}

static void convo_scroll_reset(void)
{
    g_convo_scroll_y = 0;
    g_convo_stick_bottom = true;
    g_convo_force_bottom = true;
    g_convo_skip_capture = true;
}

static int tx_dbm_for_idx(int idx)
{
    static const int TX_DBM[4] = { 2, 8, 17, 22 };
    return TX_DBM[clamp_i(idx, 0, 3)];
}

static void settings_defaults(void)
{
    S.net_mt = true;
    S.net_mc = false;         /* MeshCore off by default; enabling it starts TDM */
    S.settings.gps = false;   /* off by default to save battery (GPS unused in Alpha) */
    S.settings.dark = true;
    S.settings.save = false;
    S.settings.bright = 74;
    S.settings.timeout = 1;   /* 30s */
    S.settings.airtime = LZ_AIRTIME_DEFAULT;
    S.settings.tx = 3;        /* Max (22 dBm) - matches the radio's init power */
    S.settings.kb_light = 0;  /* Auto */
    S.settings.tz_idx = 0;    /* Eastern (EST/EDT, DST-aware) by default */
    S.settings.clock24 = false;
    S.settings.developer = false;
}

static void settings_sanitize(void)
{
    if(!LZ_MESHCORE_ENABLED) S.net_mc = false;
    S.settings.airtime = lz_airtime_mode_clamp(S.settings.airtime);
    S.settings.tx = clamp_i(S.settings.tx, 0, 3);
    S.settings.bright = clamp_i(S.settings.bright, 5, 100);
    S.settings.timeout = clamp_i(S.settings.timeout, 0, 4);
    S.settings.kb_light = clamp_i(S.settings.kb_light, 0, 2);
    S.settings.tz_idx = clamp_i(S.settings.tz_idx, 0, lz_tz_count() - 1);
}

static void settings_load(void)
{
    lz_user_settings_t p;
    if(!lz_store_load_settings(&p)) return;
    S.net_mt = p.net_mt;
    S.net_mc = p.net_mc;
    S.settings.airtime = p.airtime;
    S.settings.tx = p.tx;
    S.settings.gps = p.gps;
    S.settings.bright = p.bright;
    S.settings.timeout = p.timeout;
    S.settings.kb_light = p.kb_light;
    S.settings.tz_idx = p.tz_idx;
    S.settings.clock24 = p.clock24;
    S.settings.save = p.save;
    S.settings.developer = p.developer;
    settings_sanitize();
}

void lz_settings_save(void)
{
    settings_sanitize();
    lz_user_settings_t p = {
        S.net_mt, S.net_mc, S.settings.airtime, S.settings.tx, S.settings.gps,
        S.settings.bright, S.settings.timeout, S.settings.kb_light,
        S.settings.tz_idx, S.settings.clock24, S.settings.save,
        S.settings.developer,
    };
    lz_store_save_settings(&p);
    g_bright_dirty = false;      /* any full save also commits brightness */
}

/* Commit a pending brightness change (deferred during slider drags so a
 * key-repeat burst doesn't rewrite settings.cfg on every 6% step). */
static void lz_settings_flush(void)
{
    if(g_bright_dirty) lz_settings_save();
}

static void settings_apply_runtime(void)
{
    lz_tz_apply(S.settings.tz_idx);
    lz_svc_set_clock24(S.settings.clock24);
    lz_backend_set_airtime(S.settings.airtime);
    lz_apply_networks();
    lz_backend_set_tx_power(tx_dbm_for_idx(S.settings.tx));
}

/* ================= engine ================= */

void lz_ui_init(lv_obj_t *root)
{
    memset(&S, 0, sizeof(S));
    S.view = lz_svc_needs_onboarding() ? LZ_V_ONBOARD : LZ_V_LOCK;
    settings_defaults();
    settings_load();
    settings_apply_runtime();
    g_root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LZ_W, LZ_H);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lz_note_activity();
    lz_apply_brightness();
    lz_rebuild();
}

void lz_nav_set(int cols, int count, void (*activate)(int idx))
{
    g_cols = cols > 0 ? cols : 1;
    g_count = count;
    g_activate = activate;
}

void lz_nav_set_skip(bool (*fn)(int)) { g_skip = fn; }

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
    /* SHORT_CLICKED (not CLICKED): a tap selects/opens, but a long press is
     * suppressed here so rows can use LONG_PRESSED for their own action
     * (e.g. hold a chat to silence it) without also triggering the tap. */
    lv_obj_add_event_cb(obj, tap_item_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)idx);
}

void lz_rebuild(void)
{
    convo_scroll_capture();
    g_cols = 1; g_count = 0; g_activate = NULL; g_scroll = NULL; g_focus_obj = NULL; g_skip = NULL;
    lz_vlist_invalidate();   /* body about to be deleted; drop vlist's pointers */
    lv_obj_clean(g_root);
    /* screens style the root itself (flex flow, bg) — reset it fully so
     * layout from the previous screen can't leak into the next build */
    lv_obj_remove_style_all(g_root);
    lv_obj_set_size(g_root, LZ_W, LZ_H);
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(g_root, LZ_SCREEN_BG, 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    switch(S.view) {
        case LZ_V_ONBOARD:    lz_scr_onboard(g_root); break;
        case LZ_V_LOCK:       lz_scr_lock(g_root); break;
        case LZ_V_HOME:       lz_scr_home(g_root); break;
        case LZ_V_MESSAGES:   lz_scr_messages(g_root); break;
        case LZ_V_CONVO:      lz_scr_convo(g_root); break;
        case LZ_V_MESHTASTIC: lz_scr_meshtastic(g_root); break;
        case LZ_V_MESHCORE:   lz_scr_meshcore(g_root); break;
        case LZ_V_APPSTORE:   lz_scr_appstore(g_root); break;
        case LZ_V_LOCALAPP:   lz_scr_local_app(g_root); break;
        case LZ_V_LOCALAPP_RUN: lz_scr_local_app_run(g_root); break;
        case LZ_V_CONTACTS:   lz_scr_contacts(g_root); break;
        case LZ_V_CONTACT:    lz_scr_contact(g_root); break;
        case LZ_V_SETTINGS:   lz_scr_settings(g_root); break;
        case LZ_V_SYSTEM:     lz_scr_system(g_root); break;
        case LZ_V_TERMINAL:   lz_scr_terminal(g_root); break;
        case LZ_V_FILES:      lz_scr_files(g_root); break;
        case LZ_V_WIFI:       lz_scr_wifi(g_root); break;
        case LZ_V_SETTIME:    lz_scr_settime(g_root); break;
        case LZ_V_TZPICK:     lz_scr_tzpick(g_root); break;
        case LZ_V_TOUCHCAL:   lz_scr_touchcal(g_root); break;
        default: break;
    }
    if(g_focus_obj && g_scroll) {
        lv_obj_update_layout(g_scroll);
        /* recursive: settings rows sit inside group cards, so the scrollable
         * body is the grandparent, not the direct parent */
        lv_obj_scroll_to_view_recursive(g_focus_obj, LV_ANIM_OFF);
    } else if(S.view == LZ_V_CONVO && g_scroll) {
        lv_obj_update_layout(g_scroll);
        if(g_convo_force_bottom || g_convo_stick_bottom)
            lv_obj_scroll_to_y(g_scroll, LV_COORD_MAX, LV_ANIM_OFF);
        else
            lv_obj_scroll_to_y(g_scroll, g_convo_scroll_y, LV_ANIM_OFF);
        g_convo_force_bottom = false;
    } else if(S.view == LZ_V_TERMINAL && g_scroll) {
        /* console stays pinned to the newest line */
        lv_obj_update_layout(g_scroll);
        lv_obj_scroll_to_y(g_scroll, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

void lz_go(lz_view_t v)
{
    lz_settings_flush();
    if(S.nav_depth < (int)(sizeof(S.nav_stack)/sizeof(S.nav_stack[0])))
        S.nav_stack[S.nav_depth++] = S.view;
    if(v == LZ_V_CONVO) convo_scroll_reset();
    S.view = v;
    S.focus = 0;
    lz_vlist_reset_scroll();    /* fresh screen starts at the top */
    lz_rebuild();
}

void lz_back(void)
{
    lz_settings_flush();
    if(S.view == LZ_V_LOCALAPP_RUN) lz_stop_local_app();
    lz_view_t v = LZ_V_HOME;
    if(S.nav_depth > 0) v = S.nav_stack[--S.nav_depth];
    if(v == LZ_V_LOCK) v = LZ_V_HOME;
    if(v == LZ_V_CONVO) convo_scroll_reset();
    S.view = v;
    S.focus = 0;
    lz_vlist_reset_scroll();
    lz_rebuild();
}

static void unlock(void)
{
    lz_settings_flush();
    S.nav_depth = 0;
    S.view = LZ_V_HOME;
    S.focus = 0;
    S.home_page = 0;
    lz_rebuild();
}

/* manual lock (sym+L shortcut): jump straight to the lock screen from anywhere */
void lz_lock(void)
{
    if(S.view == LZ_V_ONBOARD || S.view == LZ_V_LOCK) return;
    lz_settings_flush();
    S.nav_depth = 0;
    S.view = LZ_V_LOCK;
    S.focus = 0;
    S.draft[0] = 0;
    lz_vlist_reset_scroll();
    lz_rebuild();
}

static void move(lz_key_t dir)
{
    /* brightness slider: left/right adjust while focused */
    if(S.view == LZ_V_SETTINGS && lz_settings_slider_focused() &&
       (dir == LZ_K_LEFT || dir == LZ_K_RIGHT)) {
        lz_settings_bright_adjust(dir == LZ_K_RIGHT ? 6 : -6);
        if(!lz_settings_brightness_refresh()) lz_rebuild();
        return;
    }
    lz_settings_flush();   /* any other navigation commits a pending brightness change */
    if(S.view == LZ_V_HOME && (dir == LZ_K_LEFT || dir == LZ_K_RIGHT)) {
        if(lz_home_page_key(dir)) return;
    }
    /* tab switching with left/right on single-column tabbed screens; rolling
     * left past the first tab goes back (the T-Deck has no dedicated back key) */
    if(dir == LZ_K_LEFT || dir == LZ_K_RIGHT) {
        int d = (dir == LZ_K_RIGHT) ? 1 : -1;
        if(S.view == LZ_V_MESSAGES) {
            int t = (int)S.msg_tab + d;
            if(d < 0 && t < 0) { if(confirm_left_back()) lz_back(); return; }
            if(t >= 0 && t <= 1 && t != (int)S.msg_tab) { S.msg_tab = (lz_msg_tab_t)t; S.focus = 0; lz_rebuild(); }
            return;
        }
        if(S.view == LZ_V_MESHTASTIC) {
            int t = S.mt_tab + d;
            if(d < 0 && t < 0) { if(confirm_left_back()) lz_back(); return; }
            if(t >= 0 && t <= 1 && t != S.mt_tab) { S.mt_tab = t; S.focus = 0; lz_vlist_reset_scroll(); lz_rebuild(); }
            return;
        }
        if(S.view == LZ_V_MESHCORE) {
            int t = S.mc_tab + d;
            if(d < 0 && t < 0) { if(confirm_left_back()) lz_back(); return; }
            if(t >= 0 && t <= 1 && t != S.mc_tab) { S.mc_tab = t; S.focus = 0; lz_rebuild(); }
            return;
        }
    }
    /* roll left with nowhere to move = go back */
    if(dir == LZ_K_LEFT && S.view != LZ_V_LOCK && S.view != LZ_V_ONBOARD) {
        bool can_left = (g_count > 0 && g_cols > 1 && (S.focus % g_cols) > 0);
        if(!can_left) { if(confirm_left_back()) lz_back(); return; }
    }
    if(g_count == 0) {
        /* no focusables: up/down scroll the list, clamped to the content so the
         * trackball can't scroll past the top/bottom (touch is clamped by the
         * elastic flag; this clamps the programmatic trackball scroll) */
        if(g_scroll) {
            if(dir == LZ_K_DOWN) {
                int avail = lv_obj_get_scroll_bottom(g_scroll);
                int d = avail < 60 ? avail : 60;
                if(d > 0) lv_obj_scroll_by(g_scroll, 0, -d, LV_ANIM_OFF);
            } else if(dir == LZ_K_UP) {
                int avail = lv_obj_get_scroll_top(g_scroll);
                int d = avail < 60 ? avail : 60;
                if(d > 0) lv_obj_scroll_by(g_scroll, 0, d, LV_ANIM_OFF);
            }
        }
        return;
    }
    int f = S.focus, nf = f;
    /* step in the requested direction, skipping disabled cells so the ring
     * never lands on something inert (no dead controls) */
    int step = (dir == LZ_K_UP) ? -g_cols : (dir == LZ_K_DOWN) ? g_cols
             : (dir == LZ_K_LEFT) ? -1 : 1;
    int cand = f;
    for(;;) {
        if(dir == LZ_K_LEFT  && cand % g_cols == 0) break;             /* row edge */
        if(dir == LZ_K_RIGHT && cand % g_cols == g_cols - 1) break;
        cand += step;
        if(cand < 0 || cand >= g_count) break;
        if(!g_skip || !g_skip(cand)) { nf = cand; break; }             /* found focusable */
        if(dir == LZ_K_LEFT || dir == LZ_K_RIGHT) {                     /* keep scanning the row */
            if(cand % g_cols == 0 || cand % g_cols == g_cols - 1) break;
        }
    }
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
    if(!len || !S.convo) return;
    if(!lz_svc_send_text(S.convo, S.draft)) return;   /* read-only thread: ignore */
    S.draft[0] = 0;
    g_convo_force_bottom = true;
    lz_rebuild();
    if(g_scroll) {
        lv_obj_update_layout(g_scroll);
        lv_obj_scroll_to_y(g_scroll, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

static void suggest_short(const char *longn, char *out, size_t cap);

/* onboarding: steps 0/1 are text entry (capture into S.draft), step 2 is the
 * networks chooser driven by the normal focus/activate engine, step 3 is done */
static void onboard_key(lz_key_t k, char c)
{
    if(S.ob_step == 2) {                       /* networks: focus rows + Continue */
        if(k == LZ_K_UP || k == LZ_K_DOWN) { move(k); return; }
        if(k == LZ_K_ENTER) { activate(); return; }
        if(k == LZ_K_BACK || k == LZ_K_DEL) { S.ob_step = 1; S.focus = 0;
                             suggest_short(S.ob_long, S.draft, sizeof S.draft); lz_rebuild(); return; }
        return;
    }
    if(k == LZ_K_ENTER) { lz_onboard_advance(); return; }
    if(k == LZ_K_DEL) {                        /* backspace: delete a char first */
        if(S.draft[0]) { S.draft[strlen(S.draft) - 1] = 0; lz_rebuild(); }
        else if(S.ob_step > 0) { S.ob_step--; lz_rebuild(); }
        return;
    }
    if(k == LZ_K_BACK) {                        /* back: previous step */
        if(S.ob_step > 0) { S.ob_step--; lz_rebuild(); }
        return;
    }
    if(k == LZ_K_CHAR && c >= 32 && c < 127 && S.ob_step <= 1) {
        size_t len = strlen(S.draft);
        /* Meshtastic short_name is 4 chars max; long_name up to ~39 */
        size_t cap = (S.ob_step == 1) ? 4 : sizeof(S.ob_long) - 1;
        if(S.ob_step == 1 && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');  /* tag uppercases */
        if(len < cap) { S.draft[len] = c; S.draft[len + 1] = 0; lz_rebuild(); }
        return;
    }
}

/* wifi password entry: type into S.draft, Enter connects, Back cancels/edits */
static void wifi_pw_key(lz_key_t k, char c)
{
    if(k == LZ_K_ENTER) {
        lz_wifi_connect(S.wifi_pw_ssid, S.draft);
        S.wifi_pw_mode = false; S.draft[0] = 0; S.focus = 0;
        lz_rebuild();
        return;
    }
    if(k == LZ_K_DEL) {                        /* backspace: delete a char */
        if(S.draft[0]) S.draft[strlen(S.draft) - 1] = 0;
        else { S.wifi_pw_mode = false; S.focus = 0; }
        lz_rebuild();
        return;
    }
    if(k == LZ_K_BACK) { S.wifi_pw_mode = false; S.draft[0] = 0; S.focus = 0; lz_rebuild(); return; }
    if(k == LZ_K_CHAR && c >= 32 && c < 127) {
        size_t len = strlen(S.draft);
        if(len < LZ_DRAFT_MAX - 1) { S.draft[len] = c; S.draft[len + 1] = 0; lz_rebuild(); }
    }
}

void lz_ui_key(lz_key_t k, char c)
{
    bool was_asleep = g_dimmed;
    lz_note_activity();                  /* any input wakes the screen + resets idle */
    /* tap/key to wake, then tap/key to act: the first input after sleep only
     * lights the screen (so a pocket bump can't unlock or trigger anything) */
    if(was_asleep) return;
    /* sym+L on the T-Deck keyboard emits '"'; use it as a Lock shortcut — but
     * not where you're actually typing, so a quote still works in a message,
     * Wi-Fi password, onboarding name, or terminal command. */
    if(k == LZ_K_CHAR && c == '"' &&
       S.view != LZ_V_CONVO && S.view != LZ_V_ONBOARD && S.view != LZ_V_TERMINAL &&
       !(S.view == LZ_V_WIFI && S.wifi_pw_mode)) {
        lz_lock();
        return;
    }
    if(S.view == LZ_V_ONBOARD) { onboard_key(k, c); return; }
    if(S.view == LZ_V_WIFI && S.wifi_pw_mode) { wifi_pw_key(k, c); return; }
    if(S.view == LZ_V_SETTIME) { lz_settime_key(k, c); return; }
    if(S.view == LZ_V_TERMINAL) { lz_term_key(k, c); return; }
    switch(k) {
        case LZ_K_UP: case LZ_K_DOWN: case LZ_K_LEFT: case LZ_K_RIGHT:
            move(k);
            return;
        case LZ_K_ENTER:
            if(S.view == LZ_V_CONVO) { convo_send(); return; }
            activate();
            return;
        case LZ_K_DEL:
            /* keyboard backspace: in the composer, delete a char; only leave
             * the thread when the draft is already empty. Everywhere else it
             * acts as back (the T-Deck has no dedicated back key). */
            if(S.view == LZ_V_CONVO && S.draft[0]) {
                S.draft[strlen(S.draft) - 1] = 0;
                if(!lz_convo_draft_refresh()) lz_rebuild();
                return;
            }
            if(S.view == LZ_V_LOCK) return;
            lz_back();
            return;
        case LZ_K_BACK:
            if(S.view == LZ_V_LOCK) return;
            /* explicit back (Esc / nav chevron) always leaves the screen */
            lz_back();
            return;
        case LZ_K_CHAR:
            if(S.view == LZ_V_MESSAGES) {                  /* 1/2/3 network filter */
                lz_filter_t f = S.msg_filter;
                if(c == '1') f = LZ_FILT_ALL;
                else if(c == '2') f = LZ_FILT_MT;
                else if(c == '3' && LZ_MESHCORE_ENABLED) f = LZ_FILT_MC;
                if(f != S.msg_filter) { S.msg_filter = f; S.focus = 0; lz_rebuild(); }
                return;
            }
            if(S.view == LZ_V_CONVO && c >= 32 && c < 127) {
                size_t len = strlen(S.draft);
                if(len < LZ_DRAFT_MAX - 1) {
                    S.draft[len] = c;
                    S.draft[len+1] = 0;
                    if(!lz_convo_draft_refresh()) lz_rebuild();
                }
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
    /* keep flick/momentum (swipe-and-release glides) but no rubber-band
     * overscroll past the content edge */
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLL_CHAIN);
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

    lv_color_t white = lv_color_hex(0xF2F4F6);
    lv_color_t dim   = lv_color_hex(0x3D434B);

    /* LEFT: time, iPhone-style (no AM/PM in the status bar) */
    char clk[12]; lz_fmt_now(clk, sizeof clk);  /* real time, or "--:--" if unsynced */
    char *sp = strchr(clk, ' '); if(sp) *sp = '\0';
    lv_obj_t *time_l = lz_text(bar, clk, LZ_F_HEAD, white);
    lv_obj_align(time_l, LV_ALIGN_LEFT_MID, 12, 0);

    /* RIGHT cluster: [signal bars] [wifi] [battery], flex so nothing overlaps */
    lv_obj_t *right = lz_box(bar);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LZ_STATUSBAR_H - 1);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 6, 0);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, -10, 0);

    /* signal bars — lit count from the strongest heard link (RSSI/SNR) */
    const lz_node_rt *ns; int nc = lz_svc_nodes(&ns);
    const lz_identity_t *me = lz_svc_identity();
    float best = -1000.0f; bool any = false;
    for(int i = 0; i < nc; i++) {
        if(ns[i].num == me->num || ns[i].net != LZ_NET_MT) continue;
        float s = ns[i].snr;
        if(s == s) { any = true; if(s > best) best = s; }   /* s==s skips NAN */
    }
    int lit = !any ? 0 : best >= 5.0f ? 4 : best >= -2.0f ? 3 : best >= -8.0f ? 2 : 1;
    lv_obj_t *bars = lz_box(right);
    lv_obj_set_size(bars, 4 * 3 + 3 * 2, 11);
    static const int hs[4] = { 5, 7, 9, 11 };
    for(int i = 0; i < 4; i++) {
        lv_obj_t *b = lz_box(bars);
        lv_obj_set_size(b, 3, hs[i]);
        lv_obj_set_style_radius(b, 1, 0);
        lv_obj_set_style_bg_color(b, i < lit ? white : dim, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, i * 5, 0);
    }

    /* wifi: bright when joined, dim when on-but-unjoined, hidden when off */
    if(lz_wifi_enabled())
        lz_icon(right, LZ_I_WIFI, &lz_icons_18,
                lz_wifi_connected() ? white : dim);

    /* battery (shared iPhone-style glyph, also used on the lock screen) */
    lz_battery_glyph(right);
}

/* iPhone-style battery: white rounded outline + white inner fill + nub, red
 * when critically low off-charger, a charging bolt on USB. Caller positions it
 * (it's a 25x12 box; inside a flex row it just flows). */
lv_obj_t *lz_battery_glyph(lv_obj_t *parent)
{
    lv_color_t white = lv_color_hex(0xF2F4F6);
    lz_sysinfo_t si; lz_svc_sysinfo(&si);
    int pct = si.battery_pct < 0 ? 100 : si.battery_pct;
    lv_color_t bc = (!si.usb && pct <= 15) ? lv_color_hex(0xE0564E) : white;
    lv_obj_t *bwrap = lz_box(parent);
    lv_obj_set_size(bwrap, 25, 12);
    lv_obj_t *body = lz_box(bwrap);
    lv_obj_set_size(body, 22, 11);
    lv_obj_align(body, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(body, 3, 0);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_color(body, white, 0);
    int fw = (18 * pct) / 100; if(fw < 1 && pct > 0) fw = 1;
    lv_obj_t *fill = lz_box(body);
    lv_obj_set_size(fill, fw, 7);
    lv_obj_set_style_radius(fill, 1, 0);
    lv_obj_set_style_bg_color(fill, bc, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_align(fill, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_t *nub = lz_box(bwrap);
    lv_obj_set_size(nub, 2, 4);
    lv_obj_set_style_radius(nub, 1, 0);
    lv_obj_set_style_bg_color(nub, white, 0);
    lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
    lv_obj_align(nub, LV_ALIGN_RIGHT_MID, 0, 0);
    if(si.usb) {   /* charging/USB bolt over the battery */
        lv_obj_t *bolt = lz_icon(bwrap, LZ_I_BOLT, &lz_icons_18, lv_color_hex(0x0B0E12));
        lv_obj_align(bolt, LV_ALIGN_LEFT_MID, 3, 0);
    }
    return bwrap;
}

/* derive a 4-char Meshtastic short tag from the long name (alnum, uppercased) */
static void suggest_short(const char *longn, char *out, size_t cap)
{
    size_t j = 0;
    for(const char *p = longn; *p && j < cap - 1 && j < 4; p++) {
        char ch = *p;
        if(ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        if((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) out[j++] = ch;
    }
    out[j] = 0;
    if(j == 0) snprintf(out, cap, "NODE");
}

void lz_onboard_advance(void)
{
    if(S.ob_step == 0) {
        /* long name: require something */
        size_t len = strlen(S.draft);
        while(len && S.draft[len - 1] == ' ') S.draft[--len] = 0;
        if(!len) return;
        snprintf(S.ob_long, sizeof S.ob_long, "%s", S.draft);
        suggest_short(S.ob_long, S.draft, sizeof S.draft);   /* prefill tag */
        S.ob_step = 1;
        lz_rebuild();
    } else if(S.ob_step == 1) {
        if(!S.draft[0]) suggest_short(S.ob_long, S.draft, sizeof S.draft);
        snprintf(S.ob_short, sizeof S.ob_short, "%s", S.draft);
        S.draft[0] = 0;
        S.ob_step = 2;
        S.focus = 2;          /* default to Continue: both nets on, Enter proceeds */
        lz_rebuild();
    } else if(S.ob_step == 2) {
        S.ob_step = 3;
        lz_rebuild();
    } else {
        /* finish: persist identity, drop into the unified inbox */
        lz_svc_set_identity(S.ob_long, S.ob_short);
        S.nav_depth = 0;
        S.focus = 0;
        S.view = LZ_V_MESSAGES;
        lz_rebuild();
    }
}

bool lz_settings_slider_focused(void)
{
    /* settings focus order: 0 Meshtastic, 1 MeshCore, then rows; brightness is focus 6 */
    return S.focus == 6;
}

void lz_settings_bright_adjust(int delta)
{
    int b = S.settings.bright + delta;
    if(b < 5) b = 5;
    if(b > 100) b = 100;
    if(b == S.settings.bright) return;
    S.settings.bright = b;
    lz_apply_brightness();               /* live backlight update on hardware */
    g_bright_dirty = true;               /* persisted on focus-change / screen-leave */
}
