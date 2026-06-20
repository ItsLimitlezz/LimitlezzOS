/* App Store (install flow), Contacts directory, Contact detail,
 * Terminal, Files */
#include "../ui.h"
#include "../vlist.h"
#include "../../services/app_permissions.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static void fmt_location(const lz_node_rt *n, char *out, size_t cap)
{
    if(n && (n->pos_flags & LZ_NODE_POS_VALID)) {
        snprintf(out, cap, "%.4f, %.4f",
                 (double)n->lat_i / 10000000.0,
                 (double)n->lon_i / 10000000.0);
    } else {
        snprintf(out, cap, "-");
    }
}

static void fmt_altitude(const lz_node_rt *n, char *out, size_t cap)
{
    if(n && (n->pos_flags & LZ_NODE_POS_ALT)) snprintf(out, cap, "%ld m", (long)n->alt_m);
    else snprintf(out, cap, "-");
}

static void fmt_telemetry(const lz_node_rt *n, char *out, size_t cap)
{
    if(!n || !n->telem_flags) { snprintf(out, cap, "-"); return; }
    if((n->telem_flags & LZ_NODE_TEL_TEMP) && (n->telem_flags & LZ_NODE_TEL_HUM)) {
        snprintf(out, cap, "%.1fC %.0f%%",
                 (double)n->temp_c10 / 10.0,
                 (double)n->humidity10 / 10.0);
    } else if(n->telem_flags & LZ_NODE_TEL_TEMP) {
        snprintf(out, cap, "%.1fC", (double)n->temp_c10 / 10.0);
    } else if(n->telem_flags & LZ_NODE_TEL_VOLT) {
        snprintf(out, cap, "%.2fV", (double)n->voltage_mv / 1000.0);
    } else if(n->telem_flags & LZ_NODE_TEL_PRESS) {
        snprintf(out, cap, "%.0fhPa", (double)n->pressure10 / 10.0);
    } else {
        snprintf(out, cap, "updated");
    }
}

/* ===== App Store ===== */

#define STORE_LOCAL_MAX LZ_MAX_LOCAL_APPS   /* match Home; was 4 — hid local apps 5..12 */
#define STORE_ISSUE_MAX LZ_MAX_LOCAL_APP_ISSUES

static int store_local_n;
static int store_issue_n;
static char local_app_note_id[24];
static char local_app_note[64];

static void local_app_note_clear(void)
{
    local_app_note_id[0] = 0;
    local_app_note[0] = 0;
}

static void local_app_note_set(const lz_local_app_t *app, const char *note)
{
    if(!app || !app->id[0] || !note || !note[0]) { local_app_note_clear(); return; }
    snprintf(local_app_note_id, sizeof local_app_note_id, "%s", app->id);
    snprintf(local_app_note, sizeof local_app_note, "%s", note);
}

static const char *local_app_note_for(const lz_local_app_t *app)
{
    if(!app || strcmp(local_app_note_id, app->id) != 0) return NULL;
    return local_app_note[0] ? local_app_note : NULL;
}

static int app_version_part(const char **p)
{
    int v = 0;
    while(**p >= '0' && **p <= '9') {
        if(v < 1000) v = v * 10 + (**p - '0');
        (*p)++;
    }
    if(**p == '.') (*p)++;
    return v;
}

static int app_version_cmp(const char *a, const char *b)
{
    if(!a) a = "";
    if(!b) b = "";
    const char *pa = a, *pb = b;
    for(int i = 0; i < 3; i++) {
        int av = app_version_part(&pa);
        int bv = app_version_part(&pb);
        if(av != bv) return av > bv ? 1 : -1;
    }
    return 0;
}

static bool catalog_matches_local(const lz_store_app_t *cat, const lz_local_app_t *app)
{
    if(!cat || !app || !cat->id || !app->id[0]) return false;
    char local_id[24];
    size_t n = 0;
    while(app->id[n] && app->id[n] != '.' && n + 1 < sizeof local_id) {
        local_id[n] = app->id[n];
        n++;
    }
    local_id[n] = 0;
    return local_id[0] && strcmp(cat->id, local_id) == 0;
}

static const lz_store_app_t *local_app_update_for(const lz_local_app_t *app)
{
    for(int i = 0; i < 8; i++) {
        if(catalog_matches_local(&LZ_STORE[i], app) &&
           app_version_cmp(LZ_STORE[i].version, app->version) > 0)
            return &LZ_STORE[i];
    }
    return NULL;
}

static void store_timer_cb(lv_timer_t *tm)
{
    int idx = (int)(intptr_t)tm->user_data;
    LZ_STORE[idx].state = LZ_ST_OPEN;
    if(S.view == LZ_V_APPSTORE) lz_rebuild();
}

static void store_activate(int idx)
{
    if(idx < 0) return;
    bool show_catalog = S.settings.app_source != LZ_APP_SOURCE_LOCAL_ONLY;
    if(idx < store_local_n) {
        lz_local_app_t local[STORE_LOCAL_MAX];
        int n = lz_svc_scan_apps(local, STORE_LOCAL_MAX);
        if(idx < n) {
            S.local_app_sel = local[idx];
            local_app_note_clear();
            lz_go(LZ_V_LOCALAPP);
        }
        return;
    }
    idx -= store_local_n;
    if(!show_catalog) return;
    if(idx >= 8) return;
    if(LZ_STORE[idx].state == LZ_ST_OPEN || LZ_STORE[idx].state == LZ_ST_INSTALLING) return;
    LZ_STORE[idx].state = LZ_ST_INSTALLING;
    lv_timer_t *tm = lv_timer_create(store_timer_cb, 1100, (void *)(intptr_t)idx);
    lv_timer_set_repeat_count(tm, 1);
    lz_rebuild();
}

void lz_scr_appstore(lv_obj_t *root)
{
    lz_local_app_t local[STORE_LOCAL_MAX];
    store_local_n = lz_svc_scan_apps(local, STORE_LOCAL_MAX);
    lz_local_app_issue_t issues[STORE_ISSUE_MAX];
    store_issue_n = S.settings.developer ? lz_svc_scan_app_issues(issues, STORE_ISSUE_MAX) : 0;

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

    bool show_catalog = S.settings.app_source != LZ_APP_SOURCE_LOCAL_ONLY;
    char source_line[48];
    snprintf(source_line, sizeof source_line, "Source: %s", lz_app_source_label(S.settings.app_source));
    lv_obj_t *src = lz_text(body, source_line, LZ_F_SMALL, LZ_TEXT_META);
    lv_obj_set_style_pad_left(src, 4, 0);
    lv_obj_set_style_pad_bottom(src, 3, 0);

    if(show_catalog) {
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
    }

    if(store_local_n > 0) {
        lv_obj_t *lh = lz_text(body, "Installed locally", LZ_F_BODY, lv_color_hex(0xCFD4DA));
        lv_obj_set_style_pad_top(lh, 3, 0);
        lv_obj_set_style_pad_bottom(lh, 3, 0);

        for(int i = 0; i < store_local_n; i++) {
            lz_local_app_t *a = &local[i];
            const lz_store_app_t *update = local_app_update_for(a);
            lv_obj_t *row = lz_row(body, i == S.focus);
            lv_obj_set_style_radius(row, 11, 0);

            lv_obj_t *tile = lz_box(row);
            lv_obj_set_size(tile, 36, 36);
            lv_obj_set_style_radius(tile, 10, 0);
            lv_obj_set_style_bg_color(tile, lz_tile_color(a->hue), 0);
            lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
            lv_obj_t *ic = lz_icon(tile, lz_app_icon_glyph(a->icon), &lz_icons_18, lv_color_white());
            lv_obj_center(ic);

            lv_obj_t *cl = lz_box(row);
            lv_obj_set_flex_grow(cl, 1);
            lv_obj_set_height(cl, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(cl, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(cl, 1, 0);
            lv_obj_t *nm = lz_text(cl, a->name, LZ_F_BODY, LZ_TEXT);
            lv_obj_set_width(nm, 156);
            lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
            lv_obj_t *meta = lz_box(cl);
            lv_obj_set_size(meta, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(meta, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(meta, 4, 0);
            lz_icon(meta, LZ_I_FOLDER, &lz_icons_14, LZ_TEXT_3);
            char cs[56];
            if(update) snprintf(cs, sizeof cs, "v%s -> %s - %s", a->version, update->version, a->author);
            else snprintf(cs, sizeof cs, "v%s - %s", a->version, a->author);
            lz_text(meta, cs, LZ_F_SMALL, LZ_TEXT_3);

            lv_obj_t *btn = lz_box(row);
            lv_obj_set_size(btn, LV_SIZE_CONTENT, 21);
            lv_obj_set_style_min_width(btn, update ? 66 : 52, 0);
            lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(btn, update ? LZ_STORE_BTN : lv_color_hex(0x222A33), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            if(!update) {
                lv_obj_set_style_border_width(btn, 1, 0);
                lv_obj_set_style_border_color(btn, lv_color_hex(0x3A414B), 0);
            }
            lv_obj_set_style_pad_hor(btn, 11, 0);
            lv_obj_t *bl = lz_text(btn, update ? "UPDATE" : "INFO", LZ_F_SMALL,
                                   update ? LZ_ON_MINT : lv_color_hex(0xCFD4DA));
            lv_obj_center(bl);
            lz_nav_track(row, i);
        }
    }

    if(store_issue_n > 0) {
        lv_obj_t *ih = lz_text(body, "Rejected local apps", LZ_F_BODY, lv_color_hex(0xE9B05F));
        lv_obj_set_style_pad_top(ih, 3, 0);
        lv_obj_set_style_pad_bottom(ih, 3, 0);

        for(int i = 0; i < store_issue_n; i++) {
            lz_local_app_issue_t *it = &issues[i];
            lv_obj_t *row = lz_row(body, false);
            lv_obj_set_style_radius(row, 11, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(0x4B3320), 0);

            lv_obj_t *tile = lz_box(row);
            lv_obj_set_size(tile, 36, 36);
            lv_obj_set_style_radius(tile, 10, 0);
            lv_obj_set_style_bg_color(tile, lv_color_hex(0x4B3320), 0);
            lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
            lv_obj_t *bang = lz_text(tile, "!", LZ_F_BODY, lv_color_hex(0xE9B05F));
            lv_obj_center(bang);

            lv_obj_t *cl = lz_box(row);
            lv_obj_set_flex_grow(cl, 1);
            lv_obj_set_height(cl, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(cl, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(cl, 1, 0);
            lv_obj_t *pkg = lz_text(cl, it->package, LZ_F_BODY, LZ_TEXT);
            lv_obj_set_width(pkg, 190);
            lv_label_set_long_mode(pkg, LV_LABEL_LONG_DOT);
            lv_obj_t *why = lz_text(cl, it->reason, LZ_F_SMALL, lv_color_hex(0xC98E54));
            lv_obj_set_width(why, 190);
            lv_label_set_long_mode(why, LV_LABEL_LONG_DOT);
        }
    }

    if(show_catalog) {
        lv_obj_t *hd = lz_text(body, store_local_n > 0 ? "Catalog examples" : "Apps & utilities",
                               LZ_F_BODY, lv_color_hex(0xCFD4DA));
        lv_obj_set_style_pad_bottom(hd, 3, 0);

        for(int i = 0; i < 8; i++) {
            int nav_idx = store_local_n + i;
            lz_store_app_t *a = &LZ_STORE[i];
            lv_obj_t *row = lz_row(body, nav_idx == S.focus);
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
            char cs[44]; snprintf(cs, sizeof cs, "- v%s - %s - %s", a->version, a->cat, a->size);
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
            lz_nav_track(row, nav_idx);
        }
    }
    lz_nav_set(1, store_local_n + (show_catalog ? 8 : 0), store_activate);
}

static void app_data_quota_label(uint32_t used, uint32_t quota, char *out, size_t cap)
{
    if(!out || cap == 0) return;
    uint32_t uk = (used + 1023u) / 1024u;
    uint32_t qk = quota ? (quota + 1023u) / 1024u : 0;
    if(quota && used > quota)
        snprintf(out, cap, "over quota: %lu / %lu KB", (unsigned long)uk, (unsigned long)qk);
    else if(quota)
        snprintf(out, cap, "data/ %lu / %lu KB", (unsigned long)uk, (unsigned long)qk);
    else
        snprintf(out, cap, "data/ ready");
}

void lz_start_local_app(void)
{
    (void)lz_svc_start_local_app(&S.local_app_sel, &S.local_app_run);
    lz_go(LZ_V_LOCALAPP_RUN);
}

void lz_stop_local_app(void)
{
    lz_svc_stop_local_app(&S.local_app_run);
}

void lz_open_local_app(const lz_local_app_t *app)
{
    if(!app) return;
    S.local_app_sel = *app;
    local_app_note_clear();
    lz_start_local_app();
}

static void local_app_detail_activate(int idx)
{
    lz_local_app_t *a = &S.local_app_sel;
    bool can_clear = a->permissions & LZ_APP_PERM_STORAGE;
    int clear_idx = can_clear ? 1 : -1;
    int keep_idx = can_clear ? 2 : -1;
    int delete_idx = can_clear ? 3 : 1;
    if(idx == 0) {
        lz_start_local_app();
    } else if(idx == clear_idx) {
        char err[48];
        if(lz_svc_clear_app_data(a, err, sizeof err))
            local_app_note_set(a, "Local app data cleared");
        else {
            char note[64];
            snprintf(note, sizeof note, "Clear failed: %s", err[0] ? err : "unknown");
            local_app_note_set(a, note);
        }
        lz_rebuild();
    } else if(idx == keep_idx || idx == delete_idx) {
        bool keep_data = idx == keep_idx;
        char err[48];
        if(lz_svc_uninstall_local_app(a, keep_data, err, sizeof err)) {
            local_app_note_clear();
            memset(&S.local_app_sel, 0, sizeof S.local_app_sel);
            lz_go(LZ_V_APPSTORE);
        } else {
            char note[64];
            snprintf(note, sizeof note, "Remove failed: %s", err[0] ? err : "unknown");
            local_app_note_set(a, note);
            lz_rebuild();
        }
    }
}

static void local_app_action_row(lv_obj_t *body, int idx, const char *title,
                                 const char *subtitle, const char *icon,
                                 lv_color_t icon_color)
{
    lv_obj_t *row = lz_row(body, S.focus == idx);
    lv_obj_set_style_radius(row, 11, 0);
    lz_icon(row, icon, &lz_icons_18, icon_color);
    lv_obj_t *cl = lz_box(row);
    lv_obj_set_flex_grow(cl, 1);
    lv_obj_set_height(cl, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cl, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cl, 1, 0);
    lz_text(cl, title, LZ_F_BODY, LZ_TEXT);
    lz_text(cl, subtitle, LZ_F_SMALL, LZ_TEXT_META);
    lz_nav_track(row, idx);
}

void lz_scr_local_app(lv_obj_t *root)
{
    lz_local_app_t *a = &S.local_app_sel;
    if(!a->id[0]) { lz_back(); return; }

    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lz_navbar(root, NULL, "Back");

    lv_obj_t *body = lz_vflex(root);
    lv_obj_set_style_pad_top(body, 10, 0);
    lv_obj_set_style_pad_hor(body, 12, 0);
    lv_obj_set_style_pad_bottom(body, 10, 0);
    lv_obj_set_style_pad_row(body, 8, 0);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lz_nav_set_scroll(body);

    lv_obj_t *tile = lz_box(body);
    lv_obj_set_size(tile, 54, 54);
    lv_obj_set_style_radius(tile, 13, 0);
    lv_obj_set_style_bg_color(tile, lz_tile_color(a->hue), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_t *ic = lz_icon(tile, lz_app_icon_glyph(a->icon), &lz_icons_18, lv_color_white());
    lv_obj_center(ic);

    lv_obj_t *name = lz_text(body, a->name, LZ_F_TITLE, LZ_TEXT);
    lv_obj_set_width(name, lv_pct(100));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);

    char sub[56];
    snprintf(sub, sizeof sub, "v%s - %s", a->version, a->author);
    lv_obj_t *sv = lz_text(body, sub, LZ_F_SMALL, LZ_TEXT_META);
    lv_obj_set_width(sv, lv_pct(100));
    lv_label_set_long_mode(sv, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(sv, LV_TEXT_ALIGN_CENTER, 0);

    if(a->summary[0]) {
        lv_obj_t *sum = lz_text(body, a->summary, LZ_F_SMALL, lv_color_hex(0xCFD4DA));
        lv_obj_set_width(sum, lv_pct(100));
        lv_label_set_long_mode(sum, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(sum, LV_TEXT_ALIGN_CENTER, 0);
    }

    lv_obj_t *open = lz_box(body);
    lv_obj_set_size(open, LV_SIZE_CONTENT, 26);
    lv_obj_set_style_min_width(open, 92, 0);
    lv_obj_set_style_radius(open, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(open, LZ_STORE_BTN, 0);
    lv_obj_set_style_bg_opa(open, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(open, S.focus == 0 ? LZ_FOCUS_RING_W : 0, 0);
    lv_obj_set_style_border_color(open, LZ_FOCUS, 0);
    lv_obj_set_style_pad_hor(open, 16, 0);
    lv_obj_t *ol = lz_text(open, "OPEN", LZ_F_SMALL, LZ_ON_MINT);
    lv_obj_center(ol);
    lz_nav_track(open, 0);

    const char *note = local_app_note_for(a);
    int action_idx = 1;
    if(a->permissions & LZ_APP_PERM_STORAGE) {
        const char *clear_note = note && strncmp(note, "Clear", 5) == 0 ? note : NULL;
        bool note_fail = clear_note && strncmp(clear_note, "Clear failed", 12) == 0;
        local_app_action_row(body, action_idx++, "Clear local data",
                             clear_note ? clear_note : "remove files in this app's data folder",
                             LZ_I_FOLDER, note_fail ? lv_color_hex(0xE9B05F) : LZ_TEXT_META);
        local_app_action_row(body, action_idx++, "Remove app, keep data",
                             "save data for a future reinstall",
                             LZ_I_FOLDER, LZ_TEXT_META);
    }
    local_app_action_row(body, action_idx++, "Remove app and data",
                         "delete package and scoped data",
                         LZ_I_DESCRIPTION, lv_color_hex(0xE9B05F));

    if(note) {
        lv_color_t c = strstr(note, "failed") ? lv_color_hex(0xE9B05F) : LZ_STORE_BTN;
        lv_obj_t *nl = lz_text(body, note, LZ_F_SMALL, c);
        lv_obj_set_width(nl, lv_pct(100));
        lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(nl, LV_TEXT_ALIGN_CENTER, 0);
    }

    lv_obj_t *card = lz_card(body);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    char api[28];
    char perms[104];
    char access[220];
    char storage[80];
    char status[48];
    char data_path[112];
    char data_err[48];
    snprintf(api, sizeof api, "SDK %s", a->api_version);
    lz_app_permissions_list(a->permissions, perms, sizeof perms);
    lz_app_permissions_summary(a->permissions, access, sizeof access);
    const lz_store_app_t *update = local_app_update_for(a);
    if(update) snprintf(status, sizeof status, "Update available: v%s", update->version);
    else snprintf(status, sizeof status, "Manifest ready");
    if(a->permissions & LZ_APP_PERM_STORAGE) {
        if(lz_svc_prepare_app_data(a, data_path, sizeof data_path, data_err, sizeof data_err)) {
            uint32_t used = 0, quota = 0;
            if(lz_svc_app_data_usage(a, &used, &quota, data_err, sizeof data_err))
                app_data_quota_label(used, quota, storage, sizeof storage);
            else
                snprintf(storage, sizeof storage, "unavailable: %s", data_err[0] ? data_err : "unknown");
        } else {
            snprintf(storage, sizeof storage, "unavailable: %s", data_err[0] ? data_err : "unknown");
        }
    } else {
        snprintf(storage, sizeof storage, "not requested");
    }

    const char *ks[8] = { "Status", "API", "Permissions", "Access", "Storage", "App ID", "Entry", "Folder" };
    const char *vs[8] = { status, api, perms, access, storage, a->id, a->entry, a->path };
    for(int i = 0; i < 8; i++) {
        lv_obj_t *r = lz_box(card);
        lv_obj_set_width(r, lv_pct(100));
        lv_obj_set_height(r, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_hor(r, 11, 0);
        lv_obj_set_style_pad_ver(r, 7, 0);
        if(i < 7) {
            lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(r, 1, 0);
            lv_obj_set_style_border_color(r, lv_color_hex(0x21262D), 0);
        }
        lz_text(r, ks[i], LZ_F_SMALL, lv_color_hex(0x8B939C));
        lv_obj_t *v = lz_text(r, vs[i], LZ_F_SMALL, LZ_TEXT_STRONG);
        lv_obj_set_width(v, lv_pct(100));
        lv_label_set_long_mode(v, strcmp(ks[i], "Access") == 0 ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
    }

    lz_nav_set(1, action_idx, local_app_detail_activate);
}

static void local_app_run_activate(int idx)
{
    lz_local_app_session_t *r = &S.local_app_run;
    int actions = (!r->error[0]) ? r->action_count : 0;
    if(idx >= 0 && idx < actions) {
        if(lz_svc_local_app_action(r, idx)) lz_rebuild();
        return;
    }
    if(idx == actions) {
        lz_stop_local_app();
        lz_back();
    }
}

void lz_scr_local_app_run(lv_obj_t *root)
{
    lz_local_app_t *a = &S.local_app_sel;
    lz_local_app_session_t *r = &S.local_app_run;
    if(!a->id[0]) { lz_back(); return; }

    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lz_navbar(root, r->title[0] ? r->title : a->name, "Close");

    lv_obj_t *body = lz_vflex(root);
    lv_obj_set_style_pad_top(body, 10, 0);
    lv_obj_set_style_pad_hor(body, 12, 0);
    lv_obj_set_style_pad_bottom(body, 10, 0);
    lv_obj_set_style_pad_row(body, 8, 0);
    lz_nav_set_scroll(body);

    lv_obj_t *head = lz_box(body);
    lv_obj_set_width(head, lv_pct(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 10, 0);

    lv_obj_t *tile = lz_box(head);
    lv_obj_set_size(tile, 42, 42);
    lv_obj_set_style_radius(tile, 11, 0);
    lv_obj_set_style_bg_color(tile, lz_tile_color(a->hue), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_t *ic = lz_icon(tile, lz_app_icon_glyph(a->icon), &lz_icons_18, lv_color_white());
    lv_obj_center(ic);

    lv_obj_t *hcol = lz_box(head);
    lv_obj_set_flex_grow(hcol, 1);
    lv_obj_set_height(hcol, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hcol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hcol, 2, 0);
    lv_obj_t *title = lz_text(hcol, r->title[0] ? r->title : a->name, LZ_F_HEAD, LZ_TEXT);
    lv_obj_set_width(title, 220);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_t *status = lz_text(hcol, r->status[0] ? r->status : "Foreground sandbox",
                               LZ_F_SMALL, r->error[0] ? lv_color_hex(0xE9B05F) : LZ_TEXT_META);
    lv_obj_set_width(status, 220);
    lv_label_set_long_mode(status, LV_LABEL_LONG_DOT);

    lv_obj_t *card = lz_card(body);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, 11, 0);
    lv_obj_set_style_pad_row(card, 7, 0);

    if(r->error[0]) {
        lz_text(card, "Launch blocked", LZ_F_BODY, lv_color_hex(0xE9B05F));
        lv_obj_t *err = lz_text(card, r->error, LZ_F_SMALL, LZ_TEXT);
        lv_obj_set_width(err, lv_pct(100));
        lv_label_set_long_mode(err, LV_LABEL_LONG_WRAP);
    } else {
        if(r->fault[0]) {
            lz_text(card, "Runtime fault", LZ_F_BODY, lv_color_hex(0xE9B05F));
            lv_obj_t *fault = lz_text(card, r->fault, LZ_F_SMALL, LZ_TEXT);
            lv_obj_set_width(fault, lv_pct(100));
            lv_label_set_long_mode(fault, LV_LABEL_LONG_WRAP);
        }
        lv_obj_t *body_txt = lz_text(card, r->body, LZ_F_BODY, LZ_TEXT);
        lv_obj_set_width(body_txt, lv_pct(100));
        lv_label_set_long_mode(body_txt, LV_LABEL_LONG_WRAP);
    }

    int action_count = (!r->error[0]) ? r->action_count : 0;
    for(int i = 0; i < action_count; i++) {
        lz_local_app_action_t *act = &r->actions[i];
        lv_obj_t *row = lz_row(body, i == S.focus);
        lv_obj_set_style_radius(row, 11, 0);

        lv_obj_t *icbox = lz_box(row);
        lv_obj_set_size(icbox, 32, 32);
        lv_obj_set_style_radius(icbox, 9, 0);
        lv_obj_set_style_bg_color(icbox, lv_color_hex(0x1E2B33), 0);
        lv_obj_set_style_bg_opa(icbox, LV_OPA_COVER, 0);
        lv_obj_t *ai = lz_icon(icbox, LZ_I_SEND, &lz_icons_18, LZ_STORE_BTN);
        lv_obj_center(ai);

        lv_obj_t *cl = lz_box(row);
        lv_obj_set_flex_grow(cl, 1);
        lv_obj_set_height(cl, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cl, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(cl, 1, 0);
        lv_obj_t *lbl = lz_text(cl, act->label, LZ_F_BODY, LZ_TEXT);
        lv_obj_set_width(lbl, 220);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lz_text(cl, "SDK foreground action", LZ_F_SMALL, LZ_TEXT_META);
        lz_nav_track(row, i);
    }

    char perms[104];
    char access[220];
    char storage[80];
    char runtime[48];
    char memory[48];
    lz_app_permissions_list(a->permissions, perms, sizeof perms);
    lz_app_permissions_summary(a->permissions, access, sizeof access);
    if(a->permissions & LZ_APP_PERM_STORAGE) {
        if(r->storage_ready)
            app_data_quota_label(r->data_used_bytes, r->data_quota_bytes, storage, sizeof storage);
        else
            snprintf(storage, sizeof storage, "storage unavailable");
    } else {
        snprintf(storage, sizeof storage, "not requested");
    }
    if(r->fault[0])
        snprintf(runtime, sizeof runtime, "%s", r->fault);
    else if(r->action_last > 0 && r->action_last <= r->action_count)
        snprintf(runtime, sizeof runtime, "action: %s", r->actions[r->action_last - 1].label);
    else if(r->action_count > 0)
        snprintf(runtime, sizeof runtime, "%u action%s",
                 (unsigned)r->action_count, r->action_count == 1 ? "" : "s");
    else
        snprintf(runtime, sizeof runtime, "%s", r->entry_loaded ? "foreground only" : "not loaded");
    if(r->runtime_budget_bytes)
        snprintf(memory, sizeof memory, "%lu / %lu B",
                 (unsigned long)r->runtime_used_bytes,
                 (unsigned long)r->runtime_budget_bytes);
    else
        snprintf(memory, sizeof memory, "not measured");
    const char *ks[6] = { "Permissions", "Access", "Storage", "Entry", "Runtime", "Memory" };
    const char *vs[6] = { perms, access, storage, a->entry, runtime, memory };
    lv_obj_t *meta = lz_card(body);
    lv_obj_set_height(meta, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_COLUMN);
    for(int i = 0; i < 6; i++) {
        lv_obj_t *row = lz_box(meta);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_hor(row, 11, 0);
        lv_obj_set_style_pad_ver(row, 7, 0);
        if(i < 5) {
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(0x21262D), 0);
        }
        lz_text(row, ks[i], LZ_F_SMALL, lv_color_hex(0x8B939C));
        lv_obj_t *v = lz_text(row, vs[i], LZ_F_SMALL, LZ_TEXT_STRONG);
        lv_obj_set_width(v, lv_pct(100));
        lv_label_set_long_mode(v, strcmp(ks[i], "Access") == 0 ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
    }

    lv_obj_t *close = lz_row(body, action_count == S.focus);
    lv_obj_set_style_radius(close, 11, 0);
    lz_text(close, "Close app", LZ_F_BODY, LZ_TEXT);
    lz_nav_track(close, action_count);
    lz_nav_set(1, action_count + 1, local_app_run_activate);
}

/* ===== Contacts =====
 * Only people the user purposely added; not every node ever heard. Add a
 * contact from its detail page (reached via the network managers). */

static lz_node_rt *contact_list[LZ_MAX_NODES];
static int contact_n;
static char contact_trace_id[16];
static char contact_trace_note[112];

#define CONTACT_ROW_H 46
#define CONTACT_STRIDE 49

static void contact_trace_clear(void)
{
    contact_trace_id[0] = 0;
    contact_trace_note[0] = 0;
}

static void contact_trace_set(lz_node_rt *n)
{
    if(!n) { contact_trace_clear(); return; }
    snprintf(contact_trace_id, sizeof contact_trace_id, "%s", n->id);
    lz_svc_node_trace(n, contact_trace_note, sizeof contact_trace_note);
}

static const char *contact_trace_for(const lz_node_rt *n)
{
    if(!n || !contact_trace_note[0] || strcmp(contact_trace_id, n->id) != 0)
        return NULL;
    return contact_trace_note;
}

static bool contact_locked(int idx)   /* MeshCore contacts inert until Stage 2 */
{
    return idx >= 0 && idx < contact_n &&
           !LZ_MESHCORE_ENABLED && contact_list[idx]->net == LZ_NET_MC;
}

static void contacts_activate(int idx)
{
    if(idx >= 0 && idx < contact_n && !contact_locked(idx)) {
        S.contact_sel = contact_list[idx];
        lz_go(LZ_V_CONTACT);
    }
}

static lv_obj_t *contact_row_cb(lv_obj_t *content, int index, int y, bool focused, void *ctx)
{
    (void)ctx;
    lz_node_rt *n = contact_list[index];
    char ago[8], snrs[8];
    lz_fmt_ago(n->last_heard, ago, sizeof ago);
    snprintf(snrs, sizeof snrs, "%+.1f", (double)n->snr);

    lv_obj_t *row = lz_row(content, focused);
    lv_obj_set_height(row, CONTACT_ROW_H);
    lv_obj_set_y(row, y);
    lv_obj_set_style_radius(row, 10, 0);
    if(!LZ_MESHCORE_ENABLED && n->net == LZ_NET_MC)
        lv_obj_set_style_opa(row, LV_OPA_50, 0);   /* MeshCore locked: Stage 2 */

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
    if(n->net == LZ_NET_MT)
        lz_text(r, snrs, LZ_F_SMALL, lz_snr_color(n->snr));
    else
        lz_text(r, n->dist, LZ_F_SMALL, LZ_TEXT_3);

    lz_nav_track(row, index);
    return row;
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

    lz_vlist(body, 0, contact_n, CONTACT_STRIDE, 0, contact_row_cb, NULL);
    lz_nav_set(1, contact_n, contacts_activate);
    lz_nav_set_skip(contact_locked);
}

/* ===== Contact detail ===== */

static void contact_activate(int idx)
{
    lz_node_rt *n = S.contact_sel;
    if(!n) return;
    bool messageable = lz_node_messageable(n);
    if(idx == 0) {
        if(messageable) {
            lz_thread_rt *t = lz_svc_thread_for_node(n);
            lz_open_convo(t);
        } else if(!n->contact) {
            contact_trace_clear();
            lz_svc_add_contact(n);
            lz_rebuild();
        }
        return;
    }
    /* Add-contact / Trace button */
    if(idx == 1) {
        if(messageable && !n->contact) {
            contact_trace_clear();
            lz_svc_add_contact(n);
            lz_rebuild();
        } else if(messageable) {
            lz_rebuild();
        } else {
            contact_trace_set(n);
            lz_rebuild();
        }
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
        lz_icon(add, LZ_I_PERSON, &lz_icons_16f, n->contact ? LZ_GREEN_TXT : lv_color_hex(0xCFD4DA));
        lz_text(add, n->contact ? "Saved" : "Add", LZ_F_SMALL,
                n->contact ? LZ_GREEN_TXT : lv_color_hex(0xCFD4DA));
        lz_nav_track(add, 1);
    } else {
        LZ_BTN(add, true, 0, n->contact ? lv_color_hex(0x1A2520) : LZ_TILE_242, f0);
        if(n->contact) {
            lv_obj_set_style_border_width(add, 1, 0);
            lv_obj_set_style_border_color(add, LZ_GREEN_BG, 0);
        }
        lz_icon(add, LZ_I_PERSON, &lz_icons_16f, n->contact ? LZ_GREEN_TXT : lv_color_white());
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

    const char *trace_note = contact_trace_for(n);
    if(trace_note) {
        lv_obj_t *trace_card = lz_card(body);
        lv_obj_set_height(trace_card, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(trace_card, 11, 0);
        lv_obj_set_style_pad_ver(trace_card, 8, 0);
        lv_obj_t *tl = lz_text(trace_card, trace_note, LZ_F_SMALL, LZ_TEXT_VALUE);
        lv_obj_set_width(tl, lv_pct(100));
        lv_label_set_long_mode(tl, LV_LABEL_LONG_WRAP);
    }

    /* spec table */
    lv_obj_t *card = lz_card(body);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    char batt[8];
    if(n->batt >= 0) snprintf(batt, sizeof batt, "%d%%", n->batt);
    else snprintf(batt, sizeof batt, "-");
    char loc[32], alt[16], telem[24];
    fmt_location(n, loc, sizeof loc);
    fmt_altitude(n, alt, sizeof alt);
    fmt_telemetry(n, telem, sizeof telem);
    const char *ks[8] = { "Node ID", "Hardware", "Location", "Altitude",
                          "SNR", "Battery", "Telemetry", "Last heard" };
    const char *vs[8] = { n->id, n->hw, loc, alt, snrs, batt, telem, ago };
    for(int i = 0; i < 8; i++) {
        lv_obj_t *r = lz_box(card);
        lv_obj_set_width(r, lv_pct(100));
        lv_obj_set_height(r, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(r, 11, 0);
        lv_obj_set_style_pad_ver(r, 7, 0);
        if(i < 7) {
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
    term_put(0, "LimitlezzOS Beta 0.6  -  serial console");
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
        int mt_pct, mc_pct;
        lz_airtime_split_pct(S.settings.airtime, &mt_pct, &mc_pct);
        char l[64];
        snprintf(l, sizeof l, "split %d/%d  %s", mt_pct, mc_pct,
                 lz_airtime_mode_label(S.settings.airtime));
        term_put(2, l);
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
    } else if(k == LZ_K_DEL) {               /* backspace: delete a char, else back */
        if(g_term_in[0]) { g_term_in[strlen(g_term_in) - 1] = 0; lz_rebuild(); }
        else lz_back();
    } else if(k == LZ_K_BACK) {
        lz_back();
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

#if !defined(S_IFMT) && defined(_S_IFMT)
#define S_IFMT _S_IFMT
#endif
#if !defined(S_IFDIR) && defined(_S_IFDIR)
#define S_IFDIR _S_IFDIR
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

#define FILE_ROW_MAX 24

typedef struct {
    char name[40];
    char meta[16];
    char path[112];
    bool dir;
    bool parent;
    bool storage_root;
} file_row_t;

static file_row_t file_rows[FILE_ROW_MAX];
static int file_n;
static bool file_more;
static char file_root[96];
static char file_path[112];
static const char *file_roots[3];
static int file_root_n;

static bool path_under_root(const char *root, const char *path)
{
    size_t n = strlen(root);
    return strcmp(root, path) == 0 ||
           (strncmp(path, root, n) == 0 && path[n] == '/');
}

static int files_root_index(const char *path)
{
    if(!path || !path[0]) return -1;
    for(int i = 0; i < file_root_n; i++)
        if(file_roots[i] && path_under_root(file_roots[i], path)) return i;
    return -1;
}

static bool files_at_storage_picker(void)
{
    return file_root_n > 1 && !file_path[0];
}

static const char *files_prepare_root(void)
{
    file_root_n = lz_svc_file_roots(file_roots, (int)(sizeof file_roots / sizeof file_roots[0]));
    if(file_root_n <= 0) {
        file_root[0] = 0;
        file_path[0] = 0;
        return NULL;
    }

    if(file_root_n > 1) {
        int idx = files_root_index(file_path);
        if(idx >= 0) {
            snprintf(file_root, sizeof file_root, "%s", file_roots[idx]);
            return file_root;
        }
        file_root[0] = 0;
        file_path[0] = 0;
        S.focus = 0;
        return "Storage";
    }

    const char *root = file_roots[0];
    if(strcmp(file_root, root) != 0 || !file_path[0] || !path_under_root(root, file_path)) {
        snprintf(file_root, sizeof file_root, "%s", root);
        snprintf(file_path, sizeof file_path, "%s", root);
        S.focus = 0;
    }
    return file_root;
}

static bool files_at_root(void)
{
    if(files_at_storage_picker()) return true;
    return file_root[0] && strcmp(file_path, file_root) == 0;
}

static void files_parent(void)
{
    if(files_at_storage_picker()) return;
    if(files_at_root() && file_root_n > 1) {
        file_root[0] = 0;
        file_path[0] = 0;
        return;
    }
    if(files_at_root()) return;
    char *slash = strrchr(file_path, '/');
    if(slash && slash > file_path) *slash = 0;
    else snprintf(file_path, sizeof file_path, "%s", file_root);
    if(!path_under_root(file_root, file_path))
        snprintf(file_path, sizeof file_path, "%s", file_root);
}

static void files_join(char *out, size_t n, const char *base, const char *name)
{
    size_t bl = strlen(base);
    snprintf(out, n, "%s%s%s", base, (bl && base[bl - 1] == '/') ? "" : "/", name);
}

static void files_size_meta(char *out, size_t n, const struct stat *st, bool dir)
{
    if(dir) { snprintf(out, n, "dir"); return; }
    unsigned long sz = st->st_size < 0 ? 0 : (unsigned long)st->st_size;
    if(sz < 1024UL) {
        snprintf(out, n, "%lu B", sz);
    } else if(sz < 1024UL * 1024UL) {
        unsigned long kb10 = (sz * 10UL + 512UL) / 1024UL;
        if(kb10 < 100UL) snprintf(out, n, "%lu.%lu KB", kb10 / 10UL, kb10 % 10UL);
        else             snprintf(out, n, "%lu KB", (sz + 512UL) / 1024UL);
    } else {
        unsigned long mb10 = (sz * 10UL + 524288UL) / (1024UL * 1024UL);
        snprintf(out, n, "%lu.%lu MB", mb10 / 10UL, mb10 % 10UL);
    }
}

static int files_cmp(const file_row_t *a, const file_row_t *b)
{
    if(a->parent != b->parent) return a->parent ? -1 : 1;
    if(a->dir != b->dir) return a->dir ? -1 : 1;
    return strcmp(a->name, b->name);
}

static void files_sort(void)
{
    for(int i = 1; i < file_n; i++) {
        file_row_t key = file_rows[i];
        int j = i - 1;
        while(j >= 0 && files_cmp(&key, &file_rows[j]) < 0) {
            file_rows[j + 1] = file_rows[j];
            j--;
        }
        file_rows[j + 1] = key;
    }
}

static bool files_load(void)
{
    file_n = 0;
    file_more = false;
    if(files_at_storage_picker()) {
        for(int i = 0; i < file_root_n && file_n < FILE_ROW_MAX; i++) {
            file_row_t *r = &file_rows[file_n++];
            memset(r, 0, sizeof *r);
            const char *root = file_roots[i];
            snprintf(r->name, sizeof r->name, "%s",
                     strstr(root, "appfs") ? "App flash" : "SD / local store");
            snprintf(r->meta, sizeof r->meta, "%s", strstr(root, "appfs") ? "appfs" : "store");
            snprintf(r->path, sizeof r->path, "%s", root);
            r->dir = true;
            r->storage_root = true;
        }
        return file_n > 0;
    }

    DIR *d = opendir(file_path);
    if(!d) return false;

    if(!files_at_root() && file_n < FILE_ROW_MAX) {
        file_row_t *r = &file_rows[file_n++];
        memset(r, 0, sizeof *r);
        snprintf(r->name, sizeof r->name, "..");
        snprintf(r->meta, sizeof r->meta, "up");
        r->dir = true;
        r->parent = true;
    }

    struct dirent *e;
    while((e = readdir(d)) != NULL) {
        if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if(file_n >= FILE_ROW_MAX) { file_more = true; break; }

        char full[112];
        files_join(full, sizeof full, file_path, e->d_name);

        struct stat st;
        if(stat(full, &st) != 0) continue;

        file_row_t *r = &file_rows[file_n++];
        memset(r, 0, sizeof *r);
        snprintf(r->name, sizeof r->name, "%s", e->d_name);
        snprintf(r->path, sizeof r->path, "%s", full);
        r->dir = S_ISDIR(st.st_mode);
        files_size_meta(r->meta, sizeof r->meta, &st, r->dir);
    }
    closedir(d);
    files_sort();
    if(S.focus >= file_n) S.focus = file_n > 0 ? file_n - 1 : 0;
    return true;
}

static void files_activate(int idx)
{
    if(idx < 0 || idx >= file_n) return;
    file_row_t *r = &file_rows[idx];
    if(r->parent) files_parent();
    else if(r->dir) {
        snprintf(file_path, sizeof file_path, "%s", r->path);
        int idx = files_root_index(file_path);
        if(idx >= 0) snprintf(file_root, sizeof file_root, "%s", file_roots[idx]);
    }
    else return;
    S.focus = 0;
    lz_rebuild();
}

void lz_scr_files(lv_obj_t *root)
{
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lz_navbar(root, "Files", NULL);

    const char *root_path = files_prepare_root();
    bool have_root = root_path != NULL;
    bool opened = have_root && files_load();

    lv_obj_t *path = lz_box(root);
    lv_obj_set_size(path, LZ_W, 19);
    lv_obj_set_style_border_side(path, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(path, 1, 0);
    lv_obj_set_style_border_color(path, lv_color_hex(0x171B21), 0);
    const char *path_label = !have_root ? "/sd unavailable"
                           : files_at_storage_picker() ? "Storage"
                           : file_path;
    lv_obj_t *pl = lz_text(path, path_label,
                           LZ_F_MONO, lv_color_hex(0x7F868F));
    lv_obj_set_width(pl, LZ_W - 22);
    lv_label_set_long_mode(pl, LV_LABEL_LONG_DOT);
    lv_obj_align(pl, LV_ALIGN_LEFT_MID, 11, 0);

    lv_obj_t *body = lz_vflex(root);
    lv_obj_set_style_pad_top(body, 5, 0);
    lv_obj_set_style_pad_hor(body, 7, 0);
    lv_obj_set_style_pad_bottom(body, 8, 0);
    lv_obj_set_style_pad_row(body, 2, 0);
    lz_nav_set_scroll(body);

    if(!have_root || !opened || file_n == 0) {
        const char *msg = !have_root ? "No SD/appfs mounted.\nFiles is read-only once storage is available."
                        : !opened    ? "Cannot read this folder."
                                     : "Folder is empty.";
        lv_obj_t *empty = lz_text(body, msg, LZ_F_SMALL, LZ_TEXT_3);
        lv_obj_set_width(empty, lv_pct(100));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(empty, 60, 0);
        lz_nav_set(1, 0, NULL);
        return;
    }

    for(int i = 0; i < file_n; i++) {
        file_row_t *f = &file_rows[i];
        lv_obj_t *row = lz_row(body, i == S.focus);
        lz_icon(row, f->dir ? LZ_I_FOLDER : LZ_I_DESCRIPTION, &lz_icons_18,
                f->dir ? LZ_FILE_FOLDER : lv_color_hex(0x8B939C));
        lv_obj_t *name = lz_text(row, f->name, LZ_F_BODY, LZ_TEXT_SETTING);
        lv_obj_set_flex_grow(name, 1);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lz_text(row, f->meta, LZ_F_MONO, LZ_TEXT_META);
        lz_nav_track(row, i);
    }

    if(file_more) {
        lv_obj_t *more = lz_text(body, "Showing first 24 entries", LZ_F_SMALL, LZ_TEXT_3);
        lv_obj_set_style_pad_top(more, 3, 0);
    }

    lz_nav_set(1, file_n, files_activate);
}
