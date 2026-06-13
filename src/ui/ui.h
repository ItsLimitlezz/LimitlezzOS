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

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LZ_V_ONBOARD, LZ_V_LOCK, LZ_V_HOME, LZ_V_MESSAGES, LZ_V_CONVO,
    LZ_V_MESHTASTIC, LZ_V_MESHCORE, LZ_V_APPSTORE,
    LZ_V_CONTACTS, LZ_V_CONTACT, LZ_V_SETTINGS,
    LZ_V_SYSTEM, LZ_V_TERMINAL, LZ_V_FILES,
    LZ_V_COUNT
} lz_view_t;

typedef enum {
    LZ_K_UP, LZ_K_DOWN, LZ_K_LEFT, LZ_K_RIGHT,
    LZ_K_ENTER, LZ_K_BACK, LZ_K_CHAR
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

    int mt_tab;            /* 0 nodes, 1 channels  */
    int mc_tab;            /* 0 contacts, 1 rooms  */
    lz_node_rt *contact_sel;

    struct {
        int region, preset, tx;               /* cycle indices */
        bool wifi, gps, dark, save;
        int bright;                           /* 5..100 */
        int timeout;                          /* cycle index */
    } settings;
} lz_state_t;

extern lz_state_t S;

/* --- engine --- */
void lz_ui_init(lv_obj_t *root);
void lz_ui_key(lz_key_t k, char c);
void lz_go(lz_view_t v);
void lz_back(void);
void lz_rebuild(void);

/* Registered by each screen during build */
void lz_nav_set(int cols, int count, void (*activate)(int idx));
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

/* --- screen builders (screens/) --- */
void lz_scr_onboard(lv_obj_t *root);
void lz_onboard_advance(void);              /* commit current step (Enter / Continue) */
void lz_scr_lock(lv_obj_t *root);
void lz_scr_home(lv_obj_t *root);
void lz_scr_messages(lv_obj_t *root);
void lz_scr_convo(lv_obj_t *root);
void lz_scr_meshtastic(lv_obj_t *root);
void lz_scr_meshcore(lv_obj_t *root);
void lz_scr_appstore(lv_obj_t *root);
void lz_scr_contacts(lv_obj_t *root);
void lz_scr_contact(lv_obj_t *root);
void lz_scr_settings(lv_obj_t *root);
void lz_scr_system(lv_obj_t *root);
void lz_scr_terminal(lv_obj_t *root);
void lz_scr_files(lv_obj_t *root);

/* open a network-bound conversation (Messages rows, Contact detail "Message") */
void lz_open_convo(lz_thread_rt *t);

/* settings helpers shared between key handling and the settings screen */
bool lz_settings_slider_focused(void);
void lz_settings_bright_adjust(int delta);

#ifdef __cplusplus
}
#endif

#endif
