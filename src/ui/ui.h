/**
 * LimitlezzOS UI core — trackball-first navigation engine.
 *
 * Focus model (from the design README): exactly one focusable element is
 * highlighted at all times. Grids are row-major with `cols` columns:
 * up = i-cols, down = i+cols, left = i-1, right = i+1 (clamped, no row
 * wrap). On screens with no focusables, up/down scroll the list.
 */
#ifndef LZ_UI_H
#define LZ_UI_H

#include "lvgl.h"
#include "data.h"
#include "theme.h"
#include "../services/mesh.h"
#include "../services/wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LZ_V_ONBOARD, LZ_V_LOCK, LZ_V_HOME, LZ_V_MESSAGES, LZ_V_CONVO,
    LZ_V_MESHTASTIC, LZ_V_MESHCORE, LZ_V_APPSTORE,
    LZ_V_LOCALAPP, LZ_V_LOCALAPP_RUN, LZ_V_CONTACTS, LZ_V_CONTACT, LZ_V_SETTINGS,
    LZ_V_SYSTEM, LZ_V_TERMINAL, LZ_V_FILES, LZ_V_WIFI, LZ_V_SETTIME, LZ_V_TZPICK,
    LZ_V_TOUCHCAL,
    LZ_V_COUNT
} lz_view_t;

typedef enum {
    LZ_K_UP, LZ_K_DOWN, LZ_K_LEFT, LZ_K_RIGHT,
    LZ_K_ENTER, LZ_K_BACK, LZ_K_CHAR,
    LZ_K_DEL    /* keyboard backspace: delete a char in text fields, else back */
} lz_key_t;

typedef enum { LZ_TAB_DMS, LZ_TAB_CHANNELS } lz_msg_tab_t;
typedef enum { LZ_FILT_ALL, LZ_FILT_MT, LZ_FILT_MC } lz_filter_t;

#define LZ_DRAFT_MAX   64
#define LZ_SENT_MAX    8

typedef struct {
    lz_view_t view;
    int focus;
    lz_view_t nav_stack[8];
    int nav_depth;

    bool net_mt, net_mc;

    lz_msg_tab_t msg_tab;
    lz_filter_t  msg_filter;

    lz_thread_rt *convo;                      /* open conversation (service-owned) */
    char draft[LZ_DRAFT_MAX];

    /* first-boot onboarding */
    int  ob_step;                             /* 0 long name, 1 short, 2 nets, 3 done */
    char ob_long[24];
    char ob_short[6];

    /* wifi password entry */
    bool wifi_pw_mode;
    char wifi_pw_ssid[33];

    int mt_tab;            /* 0 nodes, 1 channels  */
    int mc_tab;            /* 0 contacts, 1 rooms  */
    lz_node_rt *contact_sel;
    lz_local_app_t local_app_sel;
    lz_local_app_session_t local_app_run;
    int home_page;         /* launcher page, 8 cells per page */
    int cal_step;          /* touch calibration: which target (0..2) */

    struct {
        int region, preset, airtime, tx;      /* cycle indices */
        bool wifi, gps, dark, save;
        int bright;                           /* 5..100 */
        int timeout;                          /* cycle index */
        int kb_light;                         /* 0 Auto, 1 On, 2 Off */
        int tz_idx;                           /* timezone: offset hours = tz_idx - 12 */
        bool clock24;                         /* 24-hour vs 12-hour (AM/PM) */
        bool developer;                       /* show power-user tools on Home */
    } settings;
} lz_state_t;

extern lz_state_t S;

/* --- engine --- */
void lz_ui_init(lv_obj_t *root);
void lz_ui_key(lz_key_t k, char c);
void lz_go(lz_view_t v);
void lz_back(void);
void lz_rebuild(void);
void lz_apply_networks(void);   /* push net_mt/net_mc to the radio TDM scheduler */
void lz_settings_save(void);    /* persist user-facing settings after a change */

/* Registered by each screen during build */
void lz_nav_set(int cols, int count, void (*activate)(int idx));
void lz_nav_set_skip(bool (*is_disabled)(int idx));   /* focus skips these indices */
void lz_nav_set_scroll(lv_obj_t *scroll_container);
void lz_nav_track(lv_obj_t *obj, int idx);    /* scrolled into view when focused; tap = select */
void lz_on_click(lv_obj_t *obj, void (*fn)(void));  /* tap handler for chrome elements */

/* --- shared widgets / helpers --- */
lv_obj_t *lz_box(lv_obj_t *parent);                       /* plain styleless container */
lv_obj_t *lz_text(lv_obj_t *parent, const char *txt, const lv_font_t *font, lv_color_t color);
lv_obj_t *lz_icon(lv_obj_t *parent, const char *glyph, const lv_font_t *font, lv_color_t color);
lv_obj_t *lz_navbar(lv_obj_t *parent, const char *title, const char *back_label);
lv_obj_t *lz_row(lv_obj_t *parent, bool focused);         /* focus-ring list row */
lv_obj_t *lz_card(lv_obj_t *parent);                      /* inset-bordered card */
lv_obj_t *lz_dot(lv_obj_t *parent, int size, lv_color_t color);
lv_obj_t *lz_toggle(lv_obj_t *parent, bool on, lv_color_t on_color);
lv_obj_t *lz_vflex(lv_obj_t *parent);                     /* scrollable column body */
void      lz_status_bar(lv_obj_t *parent);
lv_obj_t *lz_battery_glyph(lv_obj_t *parent);   /* iPhone-style battery; caller aligns */

/* --- screen builders (screens/) --- */
void lz_scr_onboard(lv_obj_t *root);
void lz_onboard_advance(void);              /* commit current step (Enter / Continue) */
void lz_scr_lock(lv_obj_t *root);
void lz_scr_home(lv_obj_t *root);
bool lz_home_page_key(lz_key_t dir);
void lz_scr_messages(lv_obj_t *root);
void lz_scr_convo(lv_obj_t *root);
bool lz_convo_draft_refresh(void);       /* update compose text without a full rebuild */
void lz_scr_meshtastic(lv_obj_t *root);
void lz_scr_meshcore(lv_obj_t *root);
void lz_scr_appstore(lv_obj_t *root);
void lz_scr_local_app(lv_obj_t *root);
void lz_scr_local_app_run(lv_obj_t *root);
void lz_open_local_app(const lz_local_app_t *app);
void lz_start_local_app(void);
void lz_stop_local_app(void);
void lz_scr_contacts(lv_obj_t *root);
void lz_scr_contact(lv_obj_t *root);
void lz_scr_settings(lv_obj_t *root);
void lz_scr_system(lv_obj_t *root);
void lz_scr_terminal(lv_obj_t *root);
void lz_scr_files(lv_obj_t *root);
void lz_scr_wifi(lv_obj_t *root);
void lz_scr_settime(lv_obj_t *root);
int  lz_tz_offset(int idx);             /* standard offset (min) for a timezone index */
void lz_tz_apply(int idx);              /* push zone (offset+DST rule) into the service */
int  lz_tz_count(void);
const char *lz_tz_name(int idx);
int  lz_tz_find(const char *s);         /* by region name or abbrev; -1 if none */
void lz_scr_tzpick(lv_obj_t *root);     /* timezone picker list */
void lz_scr_touchcal(lv_obj_t *root);   /* 3-tap touch calibration */
#define LZ_CAL_MARGIN 26                 /* target inset from the screen edges */
void lz_settime_enter(void);            /* load current clock into the editor */
void lz_settime_key(lz_key_t k, char c);

/* open a network-bound conversation (Messages rows, Contact detail "Message") */
void lz_open_convo(lz_thread_rt *t);

/* manual lock (sym+L keyboard shortcut) — jump to the lock screen */
void lz_lock(void);

/* interactive serial console (Terminal app) keyboard handling */
void lz_term_key(lz_key_t k, char c);

/* screen-timeout ("Sleep after"): the platform loop calls lz_idle_tick()
 * each frame; any input calls lz_note_activity(). On timeout the screen
 * dims (via the backlight cb) and the OS returns to the lock screen. */
void lz_note_activity(void);
void lz_idle_tick(void);
bool lz_is_dimmed(void);    /* true while asleep (first tap should only wake) */
void lz_set_backlight_cb(void (*fn)(int pct));   /* hardware backlight control */
void lz_apply_brightness(void);                  /* push the brightness setting now */

/* settings helpers shared between key handling and the settings screen */
bool lz_settings_slider_focused(void);
void lz_settings_bright_adjust(int delta);
bool lz_settings_brightness_refresh(void);  /* update Settings slider without rebuilding */

#ifdef __cplusplus
}
#endif

#endif
