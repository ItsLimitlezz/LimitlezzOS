/* First-boot onboarding (master spec §5): three screens, under a minute, zero
 * jargon. Screen 1 captures the Meshtastic long name, screen 2 the short tag,
 * screen 3 the networks — then it drops into the unified inbox. The name is
 * used on both networks; on hardware it's broadcast as the node's User info. */
#include "../ui.h"
#include <stdio.h>
#include <string.h>

static void net_toggle_mt(void) { S.net_mt = !S.net_mt; lz_apply_networks(); lz_settings_save(); lz_rebuild(); }
static void net_toggle_mc(void)
{
    if(!LZ_MESHCORE_ENABLED) return;
    S.net_mc = !S.net_mc;
    lz_apply_networks();
    lz_settings_save();
    lz_rebuild();
}
static void step_continue(void) { lz_onboard_advance(); }

/* step-2 focus engine: Enter on a network row toggles it, Enter on Continue advances */
static void onboard_net_activate(int idx)
{
    if(idx == 0) net_toggle_mt();
    else if(idx == 1) net_toggle_mc();
    else step_continue();
}

/* a labeled text field showing the current draft + a block cursor */
static void text_field(lv_obj_t *parent, const char *value, const char *placeholder, int maxw)
{
    lv_obj_t *field = lz_box(parent);
    lv_obj_set_size(field, maxw, 34);
    lv_obj_set_style_radius(field, 9, 0);
    lv_obj_set_style_bg_color(field, lv_color_hex(0x191D24), 0);
    lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(field, 2, 0);
    lv_obj_set_style_border_color(field, LZ_MINT, 0);
    lv_obj_set_flex_flow(field, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(field, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(field, 2, 0);
    bool empty = !value[0];
    lv_obj_t *t = lz_text(field, empty ? placeholder : value, LZ_F_TITLE,
                          empty ? lv_color_hex(0x5E656E) : LZ_TEXT);
    (void)t;
    if(!empty) {
        lv_obj_t *cur = lz_box(field);
        lv_obj_set_size(cur, 2, 18);
        lv_obj_set_style_bg_color(cur, LZ_MINT, 0);
        lv_obj_set_style_bg_opa(cur, LV_OPA_COVER, 0);
    }
}

static void hint(lv_obj_t *parent, const char *txt)
{
    lv_obj_t *h = lz_text(parent, txt, LZ_F_SMALL, lv_color_hex(0x6F7882));
    lv_obj_set_width(h, lv_pct(100));
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
}

static void continue_button(lv_obj_t *parent, const char *label, bool focused)
{
    lv_obj_t *btn = lz_box(parent);
    lv_obj_set_size(btn, 150, 30);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, LZ_TILE_165, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    if(focused) {
        lv_obj_set_style_outline_width(btn, 2, 0);
        lv_obj_set_style_outline_color(btn, LZ_FOCUS, 0);
    }
    lv_obj_t *l = lz_text(btn, label, LZ_F_BODY, LZ_ON_MINT);
    lv_obj_center(l);
    lz_on_click(btn, step_continue);
}

void lz_scr_onboard(lv_obj_t *root)
{
    lv_obj_set_style_bg_color(root, lv_color_hex(0x0B0E13), 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(root, 22, 0);
    lv_obj_set_style_pad_row(root, 9, 0);

    /* step dots */
    lv_obj_t *dots = lz_box(root);
    lv_obj_set_size(dots, LV_SIZE_CONTENT, 8);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(dots, 5, 0);
    for(int i = 0; i < 4; i++)
        lz_dot(dots, 6, i <= S.ob_step ? LZ_MINT : lv_color_hex(0x2A2F37));

    if(S.ob_step == 0) {
        lz_text(root, "What should people call you?", LZ_F_TITLE, LZ_TEXT);
        hint(root, "Your name on the mesh - shown to everyone nearby.");
        text_field(root, S.draft, "Type your name", 220);
        hint(root, "Type a name, then press Enter");
        lz_nav_set(1, 0, NULL);
    } else if(S.ob_step == 1) {
        lz_text(root, "Pick a short tag", LZ_F_TITLE, LZ_TEXT);
        hint(root, "Up to 4 letters - your callsign on the map.");
        text_field(root, S.draft, "TAG", 120);
        char sub[40]; snprintf(sub, sizeof sub, "for %s", S.ob_long);
        hint(root, sub);
        lz_nav_set(1, 0, NULL);
    } else if(S.ob_step == 2) {
        lz_text(root, "Which networks?", LZ_F_TITLE, LZ_TEXT);
        hint(root, "Leave both on unless you know otherwise.");

        struct { const char *name; bool on; void (*tap)(void); lv_color_t col; const char *icon; } rows[2] = {
            { "Meshtastic", S.net_mt, net_toggle_mt, LZ_IDTILE_MT, LZ_I_HUB },
            { "MeshCore",   S.net_mc, net_toggle_mc, LZ_IDTILE_MC, LZ_I_LAN },
        };
        for(int i = 0; i < 2; i++) {
            bool locked = (i == 1) && !LZ_MESHCORE_ENABLED;   /* MeshCore: Stage 2 */
            lv_obj_t *row = lz_row(root, S.focus == i);
            lv_obj_set_width(row, 240);
            if(locked) lv_obj_set_style_opa(row, LV_OPA_50, 0);
            lv_obj_t *tile = lz_box(row);
            lv_obj_set_size(tile, 26, 26);
            lv_obj_set_style_radius(tile, 7, 0);
            lv_obj_set_style_bg_color(tile, rows[i].col, 0);
            lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
            lv_obj_t *ic = lz_icon(tile, rows[i].icon, &lz_icons_16f,
                                   i == 0 ? LZ_ON_CYAN : LZ_ON_AMBER);
            lv_obj_center(ic);
            lv_obj_t *nm = lz_text(row, rows[i].name, LZ_F_BODY, LZ_TEXT);
            lv_obj_set_flex_grow(nm, 1);
            if(locked) {
                lv_obj_t *chip = lz_box(row);
                lv_obj_set_size(chip, LV_SIZE_CONTENT, 16);
                lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_bg_color(chip, lv_color_hex(0x252A31), 0);
                lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
                lv_obj_set_style_pad_hor(chip, 7, 0);
                lv_obj_t *cl = lz_text(chip, "Soon", LZ_F_SMALL, lv_color_hex(0x868F99));
                lv_obj_center(cl);
            } else {
                lz_toggle(row, rows[i].on, i == 0 ? LZ_TRACK_MT : LZ_TRACK_MC);
            }
            lz_nav_track(row, i);
        }

        lv_obj_t *cont = lz_row(root, S.focus == 2);
        lv_obj_set_width(cont, 240);
        lv_obj_set_style_bg_color(cont, LZ_TILE_165, 0);
        lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lz_text(cont, "Continue", LZ_F_BODY, LZ_ON_MINT);
        lz_nav_track(cont, 2);

        lz_nav_set(1, 3, onboard_net_activate);
    } else {
        lz_text(root, "You're on the mesh.", LZ_F_TITLE, LZ_MINT);
        char who[48]; snprintf(who, sizeof who, "%s  -  %s", S.ob_long, S.ob_short);
        lz_text(root, who, LZ_F_BODY, LZ_TEXT);
        int peers = lz_svc_node_count(LZ_NET_MT) + lz_svc_node_count(LZ_NET_MC);
        char near[40]; snprintf(near, sizeof near, "%d node%s already nearby",
                                peers, peers == 1 ? "" : "s");
        hint(root, peers > 0 ? near : "Listening for nearby nodes...");
        continue_button(root, "Start messaging", true);
        lz_nav_set(1, 0, NULL);   /* Enter handled by onboard_key -> advance/finish */
    }
}
