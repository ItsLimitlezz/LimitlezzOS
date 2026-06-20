/**
 * LimitlezzOS desktop simulator — SDL2 window at 2x scale, sharing the exact
 * UI code that runs on the T-Deck.
 *
 * Input mapping (mirrors the design prototype):
 *   Arrow keys  trackball roll (focus move)
 *   Page Up     trackball press (select)
 *   Enter       keyboard Enter (select / send)
 *   Esc         back; Backspace edits draft then back
 *   Mouse       touchscreen (tap rows, tabs, chips, back, send; drag scrolls)
 *   1 / 2 / 3   Messages network filter
 *   typing      conversation composer
 *
 * Simulated-radio controls (Ctrl + key, so they don't collide with the
 * conversation composer):
 *   Ctrl+D   receive a MeshCore DM from "Limitlezz"
 *   Ctrl+P   receive a MeshCore Public-channel message
 *   Ctrl+N   a new/refreshed MeshCore node appears (real ADVERT)
 *   Ctrl+M   receive a Meshtastic LongFast broadcast
 *   Ctrl+T   receive a Meshtastic DM to us
 *   Ctrl+I   receive Meshtastic NodeInfo / Position / Telemetry (rotates)
 *   Ctrl+B   burst: one of everything (stress the inbox)
 *   Ctrl+A   toggle ambient auto-traffic on/off
 *
 * `--shots <dir>` renders every screen headless and dumps BMPs.
 * `--simtest`    runs a deterministic radio scenario and asserts the resulting
 *                inbox state (threads/decode/dedup/self-echo/ACK/TDM), then
 *                exits non-zero on any failure. CI regression value.
 * `--selftest`   runs the codec round-trip tests.
 */
#include "lvgl.h"
#include "ui/ui.h"
#include "services/app_permissions.h"
#include "services/mesh.h"
#include "services/feedback.h"
#include "services/power_policy.h"
#include "services/emergency_guard.h"
#include "services/app_catalog_fetch.h"
#include "services/ota_boot.h"
#include "services/mtproto.h"
#include "services/mcproto.h"
#include "services/mc_x25519.h"
#include "sim_fs.h"
#include "sim_radio.h"
#include <SDL.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#define SCALE 2

static lv_color_t fb[LZ_W * LZ_H];           /* full-frame shadow buffer */
static lv_color_t draw_buf_mem[LZ_W * 60];
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;

extern void sim_backend_mc_clear_last_dm(void);
extern bool sim_backend_mc_last_dm(uint8_t out32[32], char *name, int name_cap,
                                   char *text, int text_cap);
extern lz_node_rt *lz_seed_node(uint32_t num, const char *id, lz_net_t net,
                                const char *name, const char *sc,
                                const char *role, float snr, int batt,
                                const char *hw, const char *dist,
                                uint32_t ago_s, bool contact);

uint32_t lz_tick_ms(void) { return SDL_GetTicks(); }

/* sim_reset_dir() lives in sim_fs.c (recursive, Windows-safe) — included via sim_fs.h */

static void sim_mkdirs(const char *path)
{
    char cmd[256];
#ifdef _WIN32
    snprintf(cmd, sizeof cmd, "if not exist \"%s\" mkdir \"%s\"", path, path);
#else
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", path);
#endif
    system(cmd);
}

static void sim_write_bytes(const char *path, int bytes)
{
    FILE *f = fopen(path, "wb");
    if(!f) return;
    for(int i = 0; i < bytes; i++) fputc('x', f);
    fclose(f);
}

static void sim_fwrite_repeat(FILE *f, char c, int bytes)
{
    if(!f) return;
    for(int i = 0; i < bytes; i++) fputc(c, f);
}

static void sim_write_local_app(const char *datadir, const char *slug,
                                const char *id, const char *name,
                                const char *entry_name, const char *icon,
                                int hue, const char *summary,
                                const char *permissions_json)
{
    char dir[160], path[192];
    snprintf(dir, sizeof dir, "%s/apps/%s", datadir, slug);
    sim_mkdirs(dir);

    snprintf(path, sizeof path, "%s/manifest.json", dir);
    FILE *mf = fopen(path, "wb");
    if(mf) {
        fprintf(mf, "{\"id\":\"%s\",\"name\":\"%s\",\"version\":\"0.1.0\","
                    "\"author\":\"Limitless\",\"entry\":\"%s\",\"icon\":\"%s\","
                    "\"hue\":%d,\"api_version\":\"0.1\",\"permissions\":%s,"
                    "\"summary\":\"%s\"}",
                id, name, entry_name, icon, hue, permissions_json, summary);
        fclose(mf);
    }

    snprintf(path, sizeof path, "%s/%s", dir, entry_name);
    FILE *entry = fopen(path, "wb");
    if(entry) {
        bool has_storage = strstr(permissions_json, "\"storage\"") != NULL;
        bool has_time = strstr(permissions_json, "\"system_time\"") != NULL;
        bool has_battery = strstr(permissions_json, "\"battery\"") != NULL;
        fprintf(entry, "-- title: %s\n"
                       "-- status: %s\n"
                       "-- body: %s\n",
                name,
                has_time ? "SDK 0.1 foreground sandbox at {time}"
                         : "SDK 0.1 foreground sandbox",
                summary);
        if(has_time) fprintf(entry, "-- body: Local time {time}\n");
        if(has_battery) fprintf(entry, "-- body: Device battery {battery}\n");
        fprintf(entry, "-- action: Refresh | %s | %s%s\n"
                       "return true\n",
                has_storage ? (has_time ? "SDK action {time} #{count}" : "SDK action #{count}")
                            : (has_time ? "SDK action {time}" : "SDK action handled"),
                name,
                has_storage ? " refresh count: {count} stored in scoped app data. | counter:refreshes"
                            : " refreshed from a bounded foreground action.");
        fclose(entry);
    }
}

static void sim_write_install_app(const char *dir, const char *id,
                                  const char *name, const char *version)
{
    char path[192];
    sim_mkdirs(dir);
    snprintf(path, sizeof path, "%s/manifest.json", dir);
    FILE *mf = fopen(path, "wb");
    if(mf) {
        fprintf(mf, "{\"id\":\"%s\",\"name\":\"%s\",\"version\":\"%s\","
                    "\"author\":\"Limitless\",\"entry\":\"main.lua\","
                    "\"icon\":\"package\",\"hue\":84,\"api_version\":\"0.1\","
                    "\"permissions\":[\"display\"],"
                    "\"summary\":\"Install pipeline test\"}",
                id, name, version);
        fclose(mf);
    }

    snprintf(path, sizeof path, "%s/main.lua", dir);
    FILE *entry = fopen(path, "wb");
    if(entry) {
        fprintf(entry, "-- title: %s\n-- body: Install pipeline test\nreturn true\n", name);
        fclose(entry);
    }
}

static void sim_seed_local_app(const char *datadir)
{
    char dir[160], path[192];
    sim_write_local_app(datadir, "weather", "weather.mesh", "Weather Mesh",
                        "main.lua", "weather", 48, "Local weather dashboard",
                        "[\"display\",\"input\",\"storage\",\"mesh_read\",\"system_time\",\"battery\"]");
    sim_write_local_app(datadir, "notes", "notes.local", "Field Notes",
                        "main.lua", "note", 175, "Scratchpad for field work",
                        "[\"display\",\"input\",\"storage\"]");
    sim_write_local_app(datadir, "scope", "scope.local", "Signal Scope",
                        "main.lua", "terminal", 210, "Packet and RSSI viewer",
                        "[\"display\",\"input\",\"mesh_read\",\"system_time\",\"battery\"]");
    sim_write_local_app(datadir, "maps", "maps.local", "Offline Maps",
                        "main.lua", "map", 110, "Local map shell",
                        "[\"display\",\"input\",\"storage\"]");

    snprintf(dir, sizeof dir, "%s/apps/badperm", datadir);
    sim_mkdirs(dir);
    snprintf(path, sizeof path, "%s/manifest.json", dir);
    FILE *badperm = fopen(path, "wb");
    if(badperm) {
        fputs("{\"id\":\"badperm.local\",\"name\":\"Bad Perm\",\"entry\":\"main.lua\","
              "\"permissions\":[\"display\",\"raw_radio\"]}", badperm);
        fclose(badperm);
    }
    snprintf(path, sizeof path, "%s/main.lua", dir);
    FILE *bpe = fopen(path, "wb");
    if(bpe) { fputs("return true\n", bpe); fclose(bpe); }

    snprintf(dir, sizeof dir, "%s/apps/missing-entry", datadir);
    sim_mkdirs(dir);
    snprintf(path, sizeof path, "%s/manifest.json", dir);
    FILE *missing = fopen(path, "wb");
    if(missing) {
        fputs("{\"id\":\"missing.entry\",\"name\":\"Missing Entry\",\"entry\":\"missing.lua\"}", missing);
        fclose(missing);
    }
}

static void sim_seed_appfs(const char *datadir)
{
    char appfs[160];
    snprintf(appfs, sizeof appfs, "%s/appfs", datadir);
    sim_mkdirs(appfs);
    sim_write_local_app(appfs, "calculator", "calc.local", "Calculator",
                        "main.lua", "calculate", 18, "Field calculator",
                        "[\"display\",\"input\"]");
}

static bool g_headless;
/* incoming radio data → rebuild the current screen (live and headless alike);
 * lz_rebuild pins an open conversation to the newest message */
static void on_dirty(void) { lz_rebuild(); }

/* mouse = touchscreen */
static int32_t m_x, m_y;
static bool m_down;

static void mouse_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    data->point.x = m_x;
    data->point.y = m_y;
    data->state = m_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
    for(int y = area->y1; y <= area->y2; y++) {
        memcpy(&fb[y * LZ_W + area->x1], px, (area->x2 - area->x1 + 1) * sizeof(lv_color_t));
        px += area->x2 - area->x1 + 1;
    }
    if(ren && lv_disp_flush_is_last(drv)) {
        SDL_UpdateTexture(tex, NULL, fb, LZ_W * sizeof(lv_color_t));
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
    }
    lv_disp_flush_ready(drv);
}

static void write_bmp(const char *path)
{
    lv_refr_now(NULL);          /* force a synchronous redraw into fb */
    /* RGB565 -> 24-bit BMP */
    FILE *f = fopen(path, "wb");
    if(!f) {
        perror(path);
        exit(2);
    }
    int w = LZ_W, h = LZ_H, stride = w * 3, img = stride * h;
    unsigned char hdr[54] = { 'B', 'M' };
    *(uint32_t *)(hdr + 2) = 54 + img;
    *(uint32_t *)(hdr + 10) = 54;
    *(uint32_t *)(hdr + 14) = 40;
    *(int32_t *)(hdr + 18) = w;
    *(int32_t *)(hdr + 22) = -h;          /* top-down */
    *(uint16_t *)(hdr + 26) = 1;
    *(uint16_t *)(hdr + 28) = 24;
    *(uint32_t *)(hdr + 34) = img;
    fwrite(hdr, 1, 54, f);
    for(int y = 0; y < h; y++) {
        for(int x = 0; x < w; x++) {
            lv_color_t c = fb[y * w + x];
            unsigned char px[3] = {
                (unsigned char)((c.ch.blue << 3) | (c.ch.blue >> 2)),
                (unsigned char)((c.ch.green << 2) | (c.ch.green >> 4)),
                (unsigned char)((c.ch.red << 3) | (c.ch.red >> 2)),
            };
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
}

/* pump background services; rebuild when wifi status changes on its screen */
static void services_tick(void)
{
    static int last_wifi = -1;
    lz_svc_loop();
    lz_wifi_loop();
    if(!g_headless) lz_idle_tick();   /* screen-timeout / sleep-after */
    int w = lz_wifi_status();
    if(w != last_wifi) { last_wifi = w; if(S.view == LZ_V_WIFI && !S.wifi_pw_mode) lz_rebuild(); }
}

static void pump(int ms)
{
    for(int t = 0; t < ms; t += 5) {
        services_tick();
        lv_timer_handler();
        SDL_Delay(5);   /* advance real wall-clock so tick-based timers fire */
    }
}

/* synthetic tap through the real LVGL pointer pipeline */
static void tap_at(int x, int y)
{
    m_x = x; m_y = y;
    m_down = true;
    pump(90);
    m_down = false;
    pump(150);
}

static int shots(const char *dir)
{
    int fails = 0;
    #define SHOT_CHECK(cond, msg) do { \
        if(cond) printf("ok  : %s\n", msg); \
        else { printf("FAIL: %s\n", msg); fails++; } } while(0)

    sim_mkdirs(dir);

    static const struct { lz_view_t v; const char *name; } SHOTS[] = {
        { LZ_V_LOCK, "01-lock" },           { LZ_V_HOME, "02-home" },
        { LZ_V_MESSAGES, "03-messages" },   { LZ_V_CONVO, "04-conversation" },
        { LZ_V_MESHTASTIC, "05-meshtastic" }, { LZ_V_MESHCORE, "06-meshcore" },
        { LZ_V_APPSTORE, "07-appstore" },   { LZ_V_CONTACTS, "08-contacts" },
        { LZ_V_CONTACT, "09-contact" },     { LZ_V_SETTINGS, "10-settings" },
        { LZ_V_SYSTEM, "11-system" },       { LZ_V_TERMINAL, "12-terminal" },
        { LZ_V_FILES, "13-files" },
    };
    char path[512];
    sim_mkdirs(dir);

    /* onboarding shots first (fresh boot has no identity) */
    if(lz_svc_needs_onboarding()) {
        S.view = LZ_V_ONBOARD; S.ob_step = 0; S.draft[0] = 0; lz_rebuild(); pump(30);
        snprintf(path, sizeof path, "%s/00a-onboard-name.bmp", dir);
        write_bmp(path); printf("wrote %s\n", path);
        for(const char *p = "Jess"; *p; p++) lz_ui_key(LZ_K_CHAR, *p);
        lz_ui_key(LZ_K_ENTER, 0); pump(30);             /* -> short tag step, prefilled */
        snprintf(path, sizeof path, "%s/00b-onboard-tag.bmp", dir);
        write_bmp(path); printf("wrote %s\n", path);
        lz_ui_key(LZ_K_ENTER, 0); pump(30);             /* -> networks */
        snprintf(path, sizeof path, "%s/00c-onboard-nets.bmp", dir);
        write_bmp(path); printf("wrote %s\n", path);
        lz_ui_key(LZ_K_ENTER, 0); pump(30);             /* Continue -> done */
        snprintf(path, sizeof path, "%s/00d-onboard-done.bmp", dir);
        write_bmp(path); printf("wrote %s\n", path);
        lz_ui_key(LZ_K_ENTER, 0); pump(30);             /* finish -> inbox */
    }

    /* Populate MeshCore via the simulated radio (REAL ADVERT frames decoded
     * through mc_parse/mc_advert_decode, plus Public chat + a DM from the
     * maintainer's test peer "Limitlezz") so the MeshCore screen renders active
     * and the inbox shows MeshCore threads. net_mc must be tuned in for the TDM
     * gate to deliver MeshCore traffic. */
    S.net_mc = true;
    lz_backend_set_networks(S.net_mt, S.net_mc);   /* tell the sim radio MC is tuned in */
    sim_inject_mc_advert();                          /* a few MeshCore nodes appear */
    sim_inject_mc_advert();
    sim_inject_mc_advert();
    sim_inject_mc_public("Limitlezz", "anyone copy on the public channel?");
    sim_inject_mc_dm_from_limitlezz("hey - testing MeshCore DM, you read me?");

    lz_node_rt *ava = lz_svc_node_by_name("Ava Reyes");
    for(unsigned i = 0; i < sizeof(SHOTS) / sizeof(SHOTS[0]); i++) {
        S.view = SHOTS[i].v;
        S.focus = 0;
        if(SHOTS[i].v == LZ_V_HOME) S.home_page = 0;
        if(SHOTS[i].v == LZ_V_CONVO) { S.convo = lz_svc_thread_at(0);   /* newest = Ava */
                                       lz_svc_open_thread(S.convo); S.draft[0] = 0; }
        if(SHOTS[i].v == LZ_V_CONTACT) S.contact_sel = ava;
        lz_rebuild();
        pump(60);
        snprintf(path, sizeof path, "%s/%s.bmp", dir, SHOTS[i].name);
        write_bmp(path);
        printf("wrote %s\n", path);
    }

    S.view = LZ_V_HOME;
    S.home_page = 0;
    S.focus = 3;
    lz_rebuild();
    pump(60);
    lz_ui_key(LZ_K_RIGHT, 0);
    pump(60);
    snprintf(path, sizeof path, "%s/02b-home-page2.bmp", dir);
    write_bmp(path);
    printf("wrote %s\n", path);

    {
        lz_local_app_t local[LZ_MAX_LOCAL_APPS];
        int local_n = lz_svc_scan_apps(local, LZ_MAX_LOCAL_APPS);
        if(local_n > 0) {
            lz_local_app_t *shot_app = &local[0];
            for(int i = 0; i < local_n; i++)
                if(strcmp(local[i].id, "weather.mesh") == 0) shot_app = &local[i];
            S.local_app_sel = *shot_app;
            S.view = LZ_V_LOCALAPP;
            S.focus = 0;
            lz_rebuild();
            pump(60);
            snprintf(path, sizeof path, "%s/07b-local-app.bmp", dir);
            write_bmp(path);
            printf("wrote %s\n", path);
            lz_ui_key(LZ_K_DOWN, 0);
            pump(60);
            snprintf(path, sizeof path, "%s/07c-local-app-data-actions.bmp", dir);
            write_bmp(path);
            printf("wrote %s\n", path);
            lz_ui_key(LZ_K_ENTER, 0);
            pump(60);
            snprintf(path, sizeof path, "%s/07c2-local-app-data-cleared.bmp", dir);
            write_bmp(path);
            printf("wrote %s\n", path);
            lz_ui_key(LZ_K_UP, 0);
            pump(30);
            lz_ui_key(LZ_K_ENTER, 0);
            pump(60);
            snprintf(path, sizeof path, "%s/07e-local-app-run.bmp", dir);
            write_bmp(path);
            printf("wrote %s\n", path);
            lz_ui_key(LZ_K_ENTER, 0);
            pump(60);
            snprintf(path, sizeof path, "%s/07f-local-app-action.bmp", dir);
            write_bmp(path);
            printf("wrote %s\n", path);
        }
    }

    {
        bool old_dev = S.settings.developer;
        S.settings.developer = true;
        S.view = LZ_V_APPSTORE;
        S.focus = 4;        /* first catalog row; keeps rejected-app diagnostics in view */
        lz_rebuild();
        pump(60);
        snprintf(path, sizeof path, "%s/07d-appstore-diagnostics.bmp", dir);
        write_bmp(path);
        printf("wrote %s\n", path);
        S.settings.developer = old_dev;
    }

    /* behavior scenarios, driven through the real key path */

    /* input regression: the Home grid is the launcher users hit most with the
     * trackball. Assert state transitions so the CI screenshot pass also fails
     * on focus/page regressions instead of only producing changed bitmaps. */
    S.view = LZ_V_HOME; S.home_page = 0; S.focus = 0; lz_rebuild(); pump(30);
    lz_ui_key(LZ_K_UP, 0); pump(30);
    SHOT_CHECK(S.view == LZ_V_HOME && S.home_page == 0 && S.focus == 0,
               "INPUT: Home up clamps at first tile");

    S.focus = 1; lz_rebuild(); pump(30);
    lz_ui_key(LZ_K_RIGHT, 0); pump(30);
    SHOT_CHECK(S.view == LZ_V_HOME && S.home_page == 0 && S.focus == 3,
               "INPUT: Home right skips disabled MeshCore tile");

    S.focus = 3; lz_rebuild(); pump(30);
    lz_ui_key(LZ_K_RIGHT, 0); pump(30);
    SHOT_CHECK(S.view == LZ_V_HOME && S.home_page == 1 && S.focus == 0,
               "INPUT: Home right pages from row edge");
    lz_ui_key(LZ_K_LEFT, 0); pump(30);
    SHOT_CHECK(S.view == LZ_V_HOME && S.home_page == 0 && S.focus == 3,
               "INPUT: Home left pages back from first column");

    lz_ui_key(LZ_K_ENTER, 0); pump(30);
    SHOT_CHECK(S.view == LZ_V_CONTACTS,
               "INPUT: Home Enter opens the focused tile");
    lz_ui_key(LZ_K_BACK, 0); pump(30);

    /* toggle MeshCore off in Settings -> airtime rebalances to 100% */
    S.view = LZ_V_SETTINGS; S.focus = 1; lz_rebuild();
    lz_ui_key(LZ_K_ENTER, 0);
    pump(60);
    snprintf(path, sizeof path, "%s/14-settings-mc-off.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);

    /* Messages with MeshCore disabled -> rows dimmed + note, history kept */
    S.view = LZ_V_MESSAGES; S.focus = 0; lz_rebuild();
    pump(60);
    snprintf(path, sizeof path, "%s/15-messages-mc-off.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);
    S.net_mc = true;

    /* filter to Meshtastic via key '2' (the active-filter pill should move) */
    lz_ui_key(LZ_K_CHAR, '2');
    pump(60);
    snprintf(path, sizeof path, "%s/16-messages-filter-mt.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);
    lz_ui_key(LZ_K_CHAR, '1');   /* back to All */

    /* type a draft and send it through the service */
    S.msg_filter = LZ_FILT_ALL; S.msg_tab = LZ_TAB_DMS;
    { lz_node_rt *a = lz_svc_node_by_name("Ava Reyes");
      S.convo = a ? lz_svc_thread_for_node(a) : lz_svc_thread_at(0); }
    lz_svc_open_thread(S.convo); S.draft[0] = 0; S.view = LZ_V_CONVO; lz_rebuild();
    for(const char *p = "on my way up now"; *p; p++) lz_ui_key(LZ_K_CHAR, *p);
    pump(60);
    snprintf(path, sizeof path, "%s/17-convo-draft.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);
    lz_ui_key(LZ_K_ENTER, 0);
    pump(60);
    snprintf(path, sizeof path, "%s/18-convo-sent.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);
    /* the simulated peer auto-replies ~2.2s later -> full RX pipeline */
    pump(2600);
    snprintf(path, sizeof path, "%s/18b-convo-reply.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);

    /* App Store install: GET -> "..." -> OPEN */
    { lz_local_app_t local[LZ_MAX_LOCAL_APPS];
      S.focus = lz_svc_scan_apps(local, LZ_MAX_LOCAL_APPS); }
    S.view = LZ_V_APPSTORE; lz_rebuild();
    lz_ui_key(LZ_K_ENTER, 0);
    pump(60);
    snprintf(path, sizeof path, "%s/19-store-installing.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);
    pump(1200);
    snprintf(path, sizeof path, "%s/20-store-open.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);

    /* regression: home must render its grid after a flex-layout screen
     * (screens style the root; rebuild has to fully reset it) */
    S.view = LZ_V_HOME; S.focus = 0; lz_rebuild();
    lz_go(LZ_V_SETTINGS);
    pump(30);
    lz_ui_key(LZ_K_BACK, 0);          /* Esc back to home, the real path */
    pump(60);
    snprintf(path, sizeof path, "%s/21-home-after-settings.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);

    /* touch: tap the Meshtastic tile on home -> stack manager opens */
    tap_at(122, 56);
    snprintf(path, sizeof path, "%s/22-touch-open-meshtastic.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);

    /* touch: tap the nav-bar back chevron -> home again */
    tap_at(20, 14);
    snprintf(path, sizeof path, "%s/23-touch-back-home.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);

    /* touch: into Messages, tap the Channels tab, then the MeshCore chip */
    tap_at(40, 56);                   /* Messages tile */
    tap_at(240, 41);                  /* Channels tab */
    tap_at(160, 70);                  /* MeshCore filter chip */
    snprintf(path, sizeof path, "%s/24-touch-channels-mc.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);

    /* regression: focus moving below the fold must autoscroll (settings
     * rows are nested in group cards -> needs recursive scroll-to-view) */
    S.view = LZ_V_SETTINGS; S.focus = 0; lz_rebuild();
    for(int i = 0; i < 9 && S.focus != 7; i++) lz_ui_key(LZ_K_DOWN, 0);   /* down to Brightness */
    pump(60);
    snprintf(path, sizeof path, "%s/25-settings-autoscroll.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);

    /* interactive serial console: type a command, get live output */
    S.view = LZ_V_TERMINAL; S.focus = 0; lz_rebuild();
    for(const char *p = "nodes"; *p; p++) lz_ui_key(LZ_K_CHAR, *p);
    lz_ui_key(LZ_K_ENTER, 0);
    for(const char *p = "info"; *p; p++) lz_ui_key(LZ_K_CHAR, *p);
    lz_ui_key(LZ_K_ENTER, 0);
    pump(60);
    snprintf(path, sizeof path, "%s/26-terminal.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);

    /* WiFi: enable -> scan list -> pick secured -> password -> connected */
    S.view = LZ_V_WIFI; S.focus = 0; lz_rebuild();
    lz_ui_key(LZ_K_ENTER, 0);          /* turn Wi-Fi on -> scanning */
    pump(800);                         /* scan completes */
    snprintf(path, sizeof path, "%s/27-wifi-list.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);
    lz_ui_key(LZ_K_DOWN, 0);           /* focus 1: auto-connect toggle */
    lz_ui_key(LZ_K_DOWN, 0);           /* focus 2: first network (Basecamp-2G) */
    lz_ui_key(LZ_K_ENTER, 0);          /* secured -> password entry */
    for(const char *p = "trailpass"; *p; p++) lz_ui_key(LZ_K_CHAR, *p);
    pump(40);
    snprintf(path, sizeof path, "%s/28-wifi-password.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);
    lz_ui_key(LZ_K_ENTER, 0);          /* connect */
    pump(1500);                        /* connecting -> connected */
    snprintf(path, sizeof path, "%s/29-wifi-connected.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);

    /* LongFast broadcast channel: open from the Channels tab, send a broadcast,
     * then simulate an inbound broadcast from another node */
    S.view = LZ_V_MESSAGES; S.msg_tab = LZ_TAB_CHANNELS; S.focus = 0; lz_rebuild();
    pump(40);
    snprintf(path, sizeof path, "%s/31-channels-tab.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);
    lz_ui_key(LZ_K_ENTER, 0);              /* open LongFast */
    for(const char *p = "anyone on the ridge?"; *p; p++) lz_ui_key(LZ_K_CHAR, *p);
    lz_ui_key(LZ_K_ENTER, 0);              /* broadcast it */
    pump(60);
    lz_core_on_text(0x336699cc, 0xFFFFFFFFu, "copy, heading up now", 2, -8.0f);
    pump(60);
    snprintf(path, sizeof path, "%s/32-longfast-convo.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);

    /* manual set-time editor */
    lz_settime_enter();
    S.view = LZ_V_SETTIME; lz_rebuild(); pump(40);
    snprintf(path, sizeof path, "%s/33-set-time.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);

    /* settings scrolled to the TIME group */
    S.view = LZ_V_SETTINGS; S.focus = 10; lz_rebuild(); pump(40);
    snprintf(path, sizeof path, "%s/34-settings-time.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);

    /* timezone picker list */
    S.view = LZ_V_TZPICK; S.focus = 0; lz_rebuild(); pump(40);
    snprintf(path, sizeof path, "%s/36-tz-picker.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);

    /* contact detail (Add button icon) */
    { lz_node_rt *a = lz_svc_node_by_name("Base-01");
      if(a) { S.contact_sel = a; S.view = LZ_V_CONTACT; S.focus = 0; lz_rebuild(); pump(40);
              snprintf(path, sizeof path, "%s/35-contact-add.bmp", dir);
              write_bmp(path); printf("wrote %s\n", path); } }

    #undef SHOT_CHECK
    return fails;
}

/* Codec round-trip verification — proves header framing, AES-CTR symmetry,
 * channel hash, and Data protobuf are internally consistent. Interop with
 * real Meshtastic additionally depends on the constants matching firmware,
 * which they are sourced to. Returns 0 on success. */
static int codec_selftest(void)
{
    int fails = 0;
    #define CHECK(cond, msg) do { if(!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
                                  else printf("ok  : %s\n", msg); } while(0)

    /* 1. default LongFast channel hash must be 0x08 */
    CHECK(mt_channel_hash() == 0x08, "channel hash(LongFast,defaultPSK) == 0x08");

    /* 2. text frame round-trip: build -> parse header -> decrypt -> decode */
    const char *msg = "hello mesh, this is limitlezzOS";
    uint32_t from = 0x7c3af1d0, to = 0xa1b2c3d4, id = 0x12345678;
    uint8_t frame[256];
    int flen = mt_build_text(frame, sizeof frame, from, to, id, 3, true, msg);
    CHECK(flen > MT_HEADER_LEN, "mt_build_text produced a frame");

    mt_frame_t f;
    CHECK(mt_header_read(frame, flen, &f), "header parses");
    CHECK(f.to == to && f.from == from && f.id == id, "header to/from/id round-trip");
    CHECK(f.hop_limit == 3 && f.hop_start == 3 && f.want_ack, "header flags round-trip");
    CHECK(f.channel_hash == 0x08, "frame carries channel hash 0x08");

    /* 2b. fixed Meshtastic header vector: little-endian IDs and packed flags */
    {
        mt_frame_t hv; memset(&hv, 0, sizeof hv);
        hv.to = 0x01020304u;
        hv.from = 0xa0b0c0d0u;
        hv.id = 0x11223344u;
        hv.hop_limit = 5;
        hv.hop_start = 6;
        hv.want_ack = true;
        hv.via_mqtt = true;
        hv.channel_hash = 0x08;
        hv.next_hop = 0x2a;
        hv.relay_node = 0x7f;
        uint8_t hb[MT_HEADER_LEN];
        int hn = mt_header_write(hb, &hv);
        static const uint8_t expect[] = {
            0x04, 0x03, 0x02, 0x01,
            0xd0, 0xc0, 0xb0, 0xa0,
            0x44, 0x33, 0x22, 0x11,
            0xdd, 0x08, 0x2a, 0x7f
        };
        mt_frame_t hr;
        CHECK(hn == MT_HEADER_LEN && memcmp(hb, expect, sizeof expect) == 0,
              "header fixed vector matches Meshtastic layout");
        CHECK(mt_header_read(expect, sizeof expect, &hr) &&
              hr.to == hv.to && hr.from == hv.from && hr.id == hv.id &&
              hr.flags == 0xdd && hr.hop_limit == 5 && hr.hop_start == 6 &&
              hr.want_ack && hr.via_mqtt && hr.channel_hash == 0x08 &&
              hr.next_hop == 0x2a && hr.relay_node == 0x7f,
              "header fixed vector decodes flags/hops");
    }

    uint8_t dec[251];
    memcpy(dec, f.payload, f.plen);
    mt_crypt(dec, f.plen, from, id);          /* decrypt (CTR symmetric) */
    mt_data_t d;
    CHECK(mt_data_decode(dec, f.plen, &d), "Data protobuf decodes");
    CHECK(d.portnum == MT_PORT_TEXT, "portnum == TEXT_MESSAGE_APP");
    CHECK((int)d.plen == (int)strlen(msg) && memcmp(d.payload, msg, d.plen) == 0,
          "decrypted text matches original");

    /* 3. ciphertext must differ from plaintext (encryption actually ran) */
    CHECK(memcmp(frame + MT_HEADER_LEN, msg, strlen(msg)) != 0, "payload is encrypted");

    /* 4. Data encode/decode with request_id (fixed32 path) */
    mt_data_t e; memset(&e, 0, sizeof e);
    e.portnum = MT_PORT_ROUTING; e.request_id = 0xdeadbeef; e.plen = 0;
    uint8_t pb[64]; int pl = mt_data_encode(pb, sizeof pb, &e);
    mt_data_t back;
    CHECK(pl > 0 && mt_data_decode(pb, pl, &back), "routing Data encodes/decodes");
    CHECK(back.request_id == 0xdeadbeef, "request_id (fixed32) round-trips");

    /* 5. Meshtastic guard vectors: channel variants, tight buffers, and
     *    malformed protobuf inputs must fail closed instead of decoding junk. */
    {
        uint8_t psk32[32];
        for(int i = 0; i < 32; i++) psk32[i] = (uint8_t)(i + 1);
        mt_set_channel("FieldOps", psk32, (int)sizeof psk32);
        CHECK(mt_channel_hash() == 0x2e, "channel hash(FieldOps,32B PSK) == 0x2e");

        uint8_t cframe[256];
        int cflen = mt_build_text(cframe, sizeof cframe, from, to, 0xabcdef01u, 5, false, "custom PSK");
        mt_frame_t cf;
        CHECK(cflen > MT_HEADER_LEN && mt_header_read(cframe, cflen, &cf) &&
              cf.channel_hash == 0x2e && cf.hop_limit == 5 && !cf.want_ack,
              "custom channel text frame carries expected hash/flags");

        uint8_t bad_psk[3] = { 1, 2, 3 };
        mt_set_channel("LongFast", bad_psk, (int)sizeof bad_psk);
        CHECK(mt_channel_hash() == 0x08, "invalid PSK length falls back to LongFast default");
        mt_set_channel("LongFast", NULL, 0);

        uint8_t tiny[20];
        CHECK(mt_build_text(tiny, sizeof tiny, from, to, 0x10203040u, 3, false,
                            "this message cannot fit") < 0,
              "mt_build_text rejects undersized output buffer");

        mt_frame_t short_header;
        CHECK(!mt_header_read(frame, MT_HEADER_LEN - 1, &short_header),
              "header truncation rejected");

        mt_data_t bd;
        uint8_t bad_tag[] = { 0x80 };
        CHECK(!mt_data_decode(bad_tag, sizeof bad_tag, &bd),
              "Data rejects unterminated tag varint");
        uint8_t bad_value[] = { 0x08, 0x80 };
        CHECK(!mt_data_decode(bad_value, sizeof bad_value, &bd),
              "Data rejects unterminated value varint");
        uint8_t bad_len[] = { 0x12, 0x80 };
        CHECK(!mt_data_decode(bad_len, sizeof bad_len, &bd),
              "Data rejects unterminated length varint");
        uint8_t bad_unknown[] = { 0x48, 0x80 };
        CHECK(!mt_data_decode(bad_unknown, sizeof bad_unknown, &bd),
              "Data rejects unterminated unknown varint field");
        uint8_t bad_wire[] = { 0x0F };
        CHECK(!mt_data_decode(bad_wire, sizeof bad_wire, &bd),
              "Data rejects unsupported wire type");

        mt_position_t bp;
        uint8_t bad_pos[] = { 0x18, 0x80 };
        CHECK(!mt_position_decode(bad_pos, sizeof bad_pos, &bp),
              "POSITION rejects unterminated altitude varint");
        mt_telemetry_t bt;
        uint8_t bad_tel[] = { 0x12, 0x80 };
        CHECK(!mt_telemetry_decode(bad_tel, sizeof bad_tel, &bt),
              "TELEMETRY rejects unterminated submessage length");
    }

    /* 6. POSITION decode: lat/lon fixed32, altitude varint, precision_bits */
    /* 4b. fixed Data protobuf vector: TEXT payload, want_response, request_id */
    {
        mt_data_t dv; memset(&dv, 0, sizeof dv);
        dv.portnum = MT_PORT_TEXT;
        memcpy(dv.payload, "hi", 2);
        dv.plen = 2;
        dv.want_response = true;
        dv.request_id = 0x12345678u;
        uint8_t db[32];
        int dn = mt_data_encode(db, sizeof db, &dv);
        static const uint8_t expect[] = {
            0x08, 0x01,
            0x12, 0x02, 'h', 'i',
            0x18, 0x01,
            0x35, 0x78, 0x56, 0x34, 0x12
        };
        mt_data_t dd;
        CHECK(dn == (int)sizeof expect && memcmp(db, expect, sizeof expect) == 0,
              "Data protobuf fixed vector matches wire layout");
        CHECK(mt_data_decode(expect, sizeof expect, &dd) &&
              dd.portnum == MT_PORT_TEXT && dd.plen == 2 &&
              memcmp(dd.payload, "hi", 2) == 0 &&
              dd.want_response && dd.request_id == 0x12345678u,
              "Data protobuf fixed vector decodes");
    }

    /* 5. POSITION decode: lat/lon fixed32, altitude varint, precision_bits */
    {
        uint8_t pos[] = {
            0x0D, 0x44,0x33,0x22,0x11,     /* field 1  latitude_i  = 0x11223344 */
            0x15, 0x0D,0x0C,0x0B,0x0A,     /* field 2  longitude_i = 0x0A0B0C0D */
            0x18, 0x32,                    /* field 3  altitude_m  = 50         */
            0xB8,0x01, 0x20,               /* field 23 precision_bits = 32      */
        };
        mt_position_t p;
        CHECK(mt_position_decode(pos, sizeof pos, &p), "POSITION decodes");
        CHECK(p.has_lat && p.latitude_i == 0x11223344, "POSITION latitude (fixed32)");
        CHECK(p.has_lon && p.longitude_i == 0x0A0B0C0D, "POSITION longitude (fixed32)");
        CHECK(p.has_alt && p.altitude_m == 50, "POSITION altitude (varint)");
        CHECK(p.precision_bits == 32, "POSITION precision_bits (field 23)");
        uint8_t unk[] = { 0x4A,0x01,0x00, 0x0D,0x01,0x00,0x00,0x00 }; /* unknown f9 + lat */
        mt_position_t pu;
        CHECK(mt_position_decode(unk, sizeof unk, &pu) && pu.has_lat,
              "POSITION skips unknown fields");
        uint8_t trunc[] = { 0x0D, 0x44, 0x33 };               /* fixed32 cut short */
        mt_position_t pt;
        CHECK(!mt_position_decode(trunc, sizeof trunc, &pt), "POSITION truncated rejected");
        uint8_t ovf[] = { 0x0A, 0x7F };                       /* wire-2 len past buffer */
        mt_position_t po;
        CHECK(!mt_position_decode(ovf, sizeof ovf, &po), "POSITION oversized length rejected");
    }

    /* 7. TELEMETRY device metrics: battery varint, voltage float, uptime varint */
    {
        float voltage = 4.10f;
        uint8_t dm[16]; int dn = 0;
        dm[dn++] = 0x08; dm[dn++] = 87;                          /* f1 battery = 87   */
        dm[dn++] = 0x15; memcpy(dm + dn, &voltage, 4); dn += 4;  /* f2 voltage float  */
        dm[dn++] = 0x28; dm[dn++] = 0x90; dm[dn++] = 0x1C;       /* f5 uptime = 3600  */
        uint8_t tel[24]; int tn = 0;
        tel[tn++] = 0x12; tel[tn++] = (uint8_t)dn;               /* f2 device_metrics */
        memcpy(tel + tn, dm, dn); tn += dn;
        mt_telemetry_t t;
        CHECK(mt_telemetry_decode(tel, tn, &t), "TELEMETRY device metrics decode");
        CHECK(t.has_battery && t.battery_level == 87, "TELEMETRY battery_level");
        CHECK(t.has_voltage && t.voltage > 4.09f && t.voltage < 4.11f, "TELEMETRY voltage (float)");
        CHECK(t.has_uptime && t.uptime_s == 3600, "TELEMETRY uptime");
    }

    /* 8. TELEMETRY env metrics: a hostile NaN float must decode without OOB and
     *    be preserved as NaN so the clamp layer (not the decoder) rejects it */
    {
        uint32_t nanbits = 0x7FC00000u; float humidity = 55.0f, pressure = 1013.0f;
        uint8_t em[16]; int en = 0;
        em[en++] = 0x0D; memcpy(em + en, &nanbits, 4); en += 4;   /* f1 temperature NaN */
        em[en++] = 0x15; memcpy(em + en, &humidity, 4); en += 4;  /* f2 humidity        */
        em[en++] = 0x1D; memcpy(em + en, &pressure, 4); en += 4;  /* f3 pressure        */
        uint8_t tel[24]; int tn = 0;
        tel[tn++] = 0x1A; tel[tn++] = (uint8_t)en;                /* f3 environment      */
        memcpy(tel + tn, em, en); tn += en;
        mt_telemetry_t t;
        CHECK(mt_telemetry_decode(tel, tn, &t), "TELEMETRY env metrics (NaN temp) decode");
        uint32_t tbits; memcpy(&tbits, &t.temperature_c, 4);
        CHECK(t.has_temperature && (tbits & 0x7F800000u) == 0x7F800000u &&
              (tbits & 0x007FFFFFu) != 0, "TELEMETRY NaN temperature preserved");
        CHECK(t.has_humidity && t.humidity_pct > 54.9f && t.humidity_pct < 55.1f, "TELEMETRY humidity");
        CHECK(t.has_pressure && t.pressure_hpa > 1012.0f && t.pressure_hpa < 1014.0f, "TELEMETRY pressure");
        uint8_t bad[] = { 0x12, 0x7F, 0x08 };                    /* submsg len past buffer */
        mt_telemetry_t tb;
        CHECK(!mt_telemetry_decode(bad, sizeof bad, &tb), "TELEMETRY oversized submsg rejected");
    }

    /* 8. feedback DND/priority policy: normal events can be queued silently,
     *    while critical and emergency events break through as designed. */
    {
        lz_feedback_decision_t d;
        CHECK(lz_feedback_dnd_clamp(99) == LZ_FEEDBACK_DND_OFF,
              "feedback DND mode clamps to off");
        CHECK(lz_feedback_event_clamp(99) == LZ_FEEDBACK_EVENT_MESSAGE,
              "feedback event clamps to message");
        d = lz_feedback_decide(LZ_FEEDBACK_DND_OFF, LZ_FEEDBACK_EVENT_MESSAGE);
        CHECK(d.queue && d.present && d.wake_screen && !d.buzz,
              "feedback normal message wakes silently when DND is off");
        d = lz_feedback_decide(LZ_FEEDBACK_DND_PRIORITY, LZ_FEEDBACK_EVENT_MESSAGE);
        CHECK(d.queue && !d.present && !d.wake_screen,
              "feedback priority DND queues normal messages");
        d = lz_feedback_decide(LZ_FEEDBACK_DND_PRIORITY, LZ_FEEDBACK_EVENT_DIRECT);
        CHECK(d.present && d.wake_screen && !d.bypass_dnd,
              "feedback priority DND allows direct messages");
        d = lz_feedback_decide(LZ_FEEDBACK_DND_SILENT, LZ_FEEDBACK_EVENT_DIRECT);
        CHECK(d.queue && !d.present,
              "feedback silent DND queues direct messages");
        d = lz_feedback_decide(LZ_FEEDBACK_DND_SILENT, LZ_FEEDBACK_EVENT_SYSTEM);
        CHECK(d.present && d.bypass_dnd && !d.buzz,
              "feedback silent DND lets critical system events through quietly");
        d = lz_feedback_decide(LZ_FEEDBACK_DND_EMERGENCY, LZ_FEEDBACK_EVENT_OTA_FAILURE);
        CHECK(d.queue && !d.present,
              "feedback emergency DND queues OTA failures");
        d = lz_feedback_decide(LZ_FEEDBACK_DND_EMERGENCY, LZ_FEEDBACK_EVENT_EMERGENCY);
        CHECK(d.present && d.bypass_dnd && d.buzz,
              "feedback emergency bypasses DND with buzzer intent");
        char diag[700];
        int dn = lz_feedback_policy_diag(diag, sizeof diag);
        CHECK(dn > 0 && strstr(diag, "silent: message=queued") != NULL &&
              strstr(diag, "emergency=show+buzz+bypass") != NULL,
              "feedback policy diagnostic summarizes DND matrix");
    }

    /* 8b. power warning policy: low and critical battery states request
     *    progressively stronger action, except while external power is present. */
    {
        lz_power_decision_t p;
        p = lz_power_assess(-1, 0, false, false);
        CHECK(p.state == LZ_POWER_UNKNOWN && !p.notify,
              "power policy unknown battery stays quiet");
        p = lz_power_assess(87, 4100, false, false);
        CHECK(p.state == LZ_POWER_OK && !p.notify,
              "power policy healthy battery is ok");
        p = lz_power_assess(20, 3900, false, false);
        CHECK(p.state == LZ_POWER_LOW && p.notify && !p.dim_screen && !p.allow_buzz,
              "power policy low percent notifies without aggressive actions");
        p = lz_power_assess(-1, 3650, false, false);
        CHECK(p.state == LZ_POWER_LOW && p.notify && p.reason &&
              strcmp(p.reason, "voltage") == 0,
              "power policy low voltage fallback works");
        p = lz_power_assess(5, 3800, false, false);
        CHECK(p.state == LZ_POWER_CRITICAL && p.notify && p.wake_screen &&
              p.dim_screen && p.force_power_save && p.allow_buzz,
              "power policy critical battery requests strong feedback");
        p = lz_power_assess(4, 3400, true, true);
        CHECK(p.state == LZ_POWER_CRITICAL && p.notify && !p.wake_screen &&
              !p.dim_screen && !p.allow_buzz,
              "power policy critical charging stays quiet");
        char diag[320];
        int dn = lz_power_policy_diag(diag, sizeof diag, 4, 3400, false, false);
        CHECK(dn > 0 && strstr(diag, "power: critical") != NULL &&
              strstr(diag, "buzz=yes") != NULL,
              "power policy diagnostic summarizes actions");
    }

    /* 8c. emergency guard: SOS requires a deliberate hold plus confirmation
     *    inside a bounded window before later TX code can run. */
    {
        lz_emergency_guard_t g;
        char diag[220];
        lz_emergency_guard_reset(&g);
        CHECK(g.state == LZ_EMERGENCY_IDLE, "emergency guard resets idle");
        CHECK(!lz_emergency_hold_ready(LZ_EMERGENCY_HOLD_MS - 1),
              "emergency guard rejects short hold");
        CHECK(lz_emergency_hold_ready(LZ_EMERGENCY_HOLD_MS),
              "emergency guard accepts deliberate hold");
        CHECK(!lz_emergency_guard_confirm(&g, 1000),
              "emergency guard rejects confirm before arm");
        CHECK(!lz_emergency_guard_arm(&g, 1000, LZ_EMERGENCY_HOLD_MS - 1),
              "emergency guard refuses to arm on short hold");
        CHECK(lz_emergency_guard_arm(&g, 1000, LZ_EMERGENCY_HOLD_MS),
              "emergency guard arms after hold");
        CHECK(g.state == LZ_EMERGENCY_ARMED &&
              lz_emergency_guard_remaining(&g, 6000) == 5000,
              "emergency guard reports confirmation window");
        CHECK(lz_emergency_guard_confirm(&g, 10999) &&
              g.state == LZ_EMERGENCY_TRIGGERED,
              "emergency guard confirms inside window");
        lz_emergency_guard_reset(&g);
        CHECK(lz_emergency_guard_arm(&g, 2000, LZ_EMERGENCY_HOLD_MS),
              "emergency guard re-arms after reset");
        CHECK(!lz_emergency_guard_confirm(&g, 2000 + LZ_EMERGENCY_CONFIRM_MS + 1) &&
              g.state == LZ_EMERGENCY_IDLE,
              "emergency guard expires stale confirmation");
        int dn = lz_emergency_guard_diag(&g, 0, diag, sizeof diag);
        CHECK(dn > 0 && strstr(diag, "hold -> arm -> confirm") != NULL,
              "emergency guard diagnostic explains flow");
    }

    /* 9. store delivery-metadata round-trip: updating a DM that is NOT the first
     *    self-record must not desync the scan over the preceding v3 meta record. */
    {
        extern void lz_store_init(const char *datadir);
        extern void lz_store_append(const char *addr, const lz_msg_rt *m);
        extern bool lz_store_update_delivery(const char *addr, uint32_t old_pkt_id,
                                             uint32_t new_pkt_id, uint8_t status,
                                             uint8_t retries, uint8_t fail_reason);
        extern int  lz_store_load_tail(const char *addr, lz_msg_rt *ring, int cap);
        const char *addr = "selftestdm";
        remove("./m_selftestdm.log");
        lz_store_init(".");
        for(int i = 0; i < 3; i++) {
            lz_msg_rt m; memset(&m, 0, sizeof m);
            m.self = true; m.ts = 1000 + i; m.status = LZ_MSG_SENDING;
            m.pkt_id = 0x1000 + i;
            snprintf(m.text, sizeof m.text, "dm number %d", i);
            lz_store_append(addr, &m);
        }
        /* mark the SECOND DM delivered; the first is a non-matching meta record */
        bool upd = lz_store_update_delivery(addr, 0x1001, 0x1001,
                                            LZ_MSG_DELIVERED, 0, LZ_FAIL_NONE);
        CHECK(upd, "store update_delivery finds a non-first DM");
        lz_msg_rt ring[LZ_TAIL_MAX];
        int rn = lz_store_load_tail(addr, ring, LZ_TAIL_MAX);
        CHECK(rn == 3, "store reloaded all 3 DMs (no desync)");
        CHECK(rn == 3 && ring[1].pkt_id == 0x1001 && ring[1].status == LZ_MSG_DELIVERED,
              "store: 2nd DM persisted as DELIVERED");
        CHECK(rn == 3 && ring[0].status == LZ_MSG_SENDING,
              "store: 1st DM left untouched");
        remove("./m_selftestdm.log");
        lz_store_init(NULL);              /* back to RAM-only */
    }

    /* 9. node DB schema regression: new rows carry an explicit schema tag, while
     * untagged legacy rows still load for field-preserving upgrades. */
    {
        extern void lz_store_init(const char *datadir);
        extern void lz_store_save_nodes(const lz_node_rt *nodes, int n);
        extern int  lz_store_load_nodes(lz_node_rt *out, int cap);
        sim_reset_dir("lzdata_nodeschema");
        lz_store_init("lzdata_nodeschema");

        lz_node_rt nd; memset(&nd, 0, sizeof nd);
        nd.num = 0x7c3af1d0u;
        snprintf(nd.id, sizeof nd.id, "!7c3af1d0");
        snprintf(nd.name, sizeof nd.name, "Ava Reyes");
        snprintf(nd.shortcode, sizeof nd.shortcode, "AVA");
        nd.net = LZ_NET_MT;
        snprintf(nd.role, sizeof nd.role, "Router");
        snprintf(nd.hw, sizeof nd.hw, "T-Beam");
        snprintf(nd.dist, sizeof nd.dist, "4.2 km");
        nd.snr = -7.5f; nd.batt = 87; nd.contact = true; nd.last_heard = 1781274000;
        nd.has_key = true;
        for(int i = 0; i < 32; i++) nd.pubkey[i] = (uint8_t)(i + 1);
        nd.pos_flags = LZ_NODE_POS_VALID | LZ_NODE_POS_ALT | LZ_NODE_POS_PREC;
        nd.lat_i = 451234567; nd.lon_i = -751234567; nd.alt_m = 320;
        nd.pos_time = 1781273900; nd.precision_bits = 28;
        nd.telem_flags = LZ_NODE_TEL_VOLT | LZ_NODE_TEL_TEMP | LZ_NODE_TEL_HUM |
                         LZ_NODE_TEL_PRESS | LZ_NODE_TEL_UPTIME;
        nd.voltage_mv = 4100; nd.temp_c10 = -35; nd.humidity10 = 650;
        nd.pressure10 = 10132; nd.uptime_s = 3600;
        lz_store_save_nodes(&nd, 1);

        lz_node_rt loaded[2];
        int ln = lz_store_load_nodes(loaded, 2);
        CHECK(ln == 1, "store: node DB v2 reload count");
        CHECK(ln == 1 && loaded[0].num == nd.num &&
              strcmp(loaded[0].id, nd.id) == 0 &&
              strcmp(loaded[0].name, nd.name) == 0 &&
              strcmp(loaded[0].shortcode, nd.shortcode) == 0 &&
              loaded[0].net == nd.net && strcmp(loaded[0].role, nd.role) == 0 &&
              strcmp(loaded[0].hw, nd.hw) == 0 && loaded[0].batt == nd.batt &&
              loaded[0].contact == nd.contact && loaded[0].last_heard == nd.last_heard,
              "store: node DB v2 identity fields round-trip");
        CHECK(ln == 1 && loaded[0].has_key && loaded[0].pubkey[0] == 1 &&
              loaded[0].pubkey[31] == 32 &&
              loaded[0].pos_flags == nd.pos_flags && loaded[0].lat_i == nd.lat_i &&
              loaded[0].lon_i == nd.lon_i && loaded[0].alt_m == nd.alt_m &&
              loaded[0].pos_time == nd.pos_time &&
              loaded[0].precision_bits == nd.precision_bits,
              "store: node DB v2 key/position fields round-trip");
        CHECK(ln == 1 && loaded[0].telem_flags == nd.telem_flags &&
              loaded[0].voltage_mv == nd.voltage_mv &&
              loaded[0].temp_c10 == nd.temp_c10 &&
              loaded[0].humidity10 == nd.humidity10 &&
              loaded[0].pressure10 == nd.pressure10 &&
              loaded[0].uptime_s == nd.uptime_s,
              "store: node DB v2 telemetry fields round-trip");

        FILE *nf = fopen("lzdata_nodeschema/nodes.db", "rb");
        char line[640] = {0};
        bool line_ok = nf && fgets(line, sizeof line, nf) != NULL;
        if(nf) fclose(nf);
        CHECK(line_ok && strncmp(line, "v2|", 3) == 0,
              "store: node DB save writes schema v2");

        nf = fopen("lzdata_nodeschema/nodes.db", "wb");
        if(nf) {
            fputs("305419896|!12345678|0|Client|Heltec V3|55|0|1781274000|-3.5|2.0 km|H3|Legacy Node|-|0|0|0|0|0|0|0|0|0|0|0|0\n", nf);
            fclose(nf);
        }
        memset(loaded, 0, sizeof loaded);
        ln = lz_store_load_nodes(loaded, 2);
        CHECK(ln == 1 && loaded[0].num == 305419896u &&
              strcmp(loaded[0].name, "Legacy Node") == 0 &&
              strcmp(loaded[0].hw, "Heltec V3") == 0 &&
              !loaded[0].has_key && loaded[0].batt == 55,
              "store: legacy untagged node DB row loads");

        nf = fopen("lzdata_nodeschema/nodes.db", "wb");
        if(nf) {
            fputs("2271560481|!87654321|1|Chat|T-Deck|-1|1|1781274100|4.0|-|MC1|MeshCore Peer\n", nf);
            fclose(nf);
        }
        memset(loaded, 0, sizeof loaded);
        ln = lz_store_load_nodes(loaded, 2);
        CHECK(ln == 1 && loaded[0].net == LZ_NET_MC &&
              strcmp(loaded[0].name, "MeshCore Peer") == 0 &&
              loaded[0].contact && loaded[0].pos_flags == 0 &&
              loaded[0].telem_flags == 0,
              "store: short legacy node DB row defaults optional fields");

        lz_store_init(NULL);
    }

    /* 10. Wi-Fi credential store round-trip. T-Deck uses an NVS backend under the
     * same API; native keeps the file path for simulator repeatability. */
    {
        extern void lz_store_init(const char *datadir);
        extern void lz_store_save_wifi(const char *ssid, const char *pass, int autoconnect);
        extern bool lz_store_load_wifi(char *ssid, int sn, char *pass, int pn, int *autoconnect);
        extern bool lz_store_settings_selftest(char *err, int err_cap);
        extern void lz_store_save_nodes(const lz_node_rt *nodes, int n);
        extern int  lz_store_load_nodes(lz_node_rt *out, int cap);
        extern bool lz_store_nodes_selftest(char *err, int err_cap);
        remove("./wifi.cfg");
        remove("./settings.cfg");
        remove("./nodes.db");
        lz_store_init(".");
        lz_store_save_wifi("TrailNet", "ridge-pass", 0);
        char ssid[33] = {0}, pass[64] = {0}; int ac = 1;
        CHECK(lz_store_load_wifi(ssid, sizeof ssid, pass, sizeof pass, &ac),
              "store: Wi-Fi credentials reload");
        CHECK(strcmp(ssid, "TrailNet") == 0 && strcmp(pass, "ridge-pass") == 0 && ac == 0,
              "store: Wi-Fi SSID/pass/autoconnect round-trip");
        lz_store_save_wifi("", "", 1);
        CHECK(!lz_store_load_wifi(ssid, sizeof ssid, pass, sizeof pass, &ac),
              "store: Wi-Fi forget clears saved network");
        char settings_err[64];
        CHECK(lz_store_settings_selftest(settings_err, sizeof settings_err),
              "store: settings schema migration selftest");
        lz_user_settings_t saved = {
            true, false, LZ_AIRTIME_MC_FIRST, 1, true, 63, 3, 2, 6,
            true, true, true,
        };
        lz_store_save_settings(&saved);
        lz_user_settings_t loaded;
        memset(&loaded, 0, sizeof loaded);
        CHECK(lz_store_load_settings(&loaded) &&
                  loaded.net_mt && !loaded.net_mc &&
                  loaded.airtime == LZ_AIRTIME_MC_FIRST &&
                  loaded.tx == 1 && loaded.gps && loaded.bright == 63 &&
                  loaded.timeout == 3 && loaded.kb_light == 2 &&
                  loaded.tz_idx == 6 && loaded.clock24 && loaded.save &&
                  loaded.developer,
              "store: settings v3 round-trip");
        char node_err[64];
        CHECK(lz_store_nodes_selftest(node_err, sizeof node_err),
              "store: node DB schema migration selftest");
        lz_node_rt saved_node;
        memset(&saved_node, 0, sizeof saved_node);
        saved_node.num = 0x7c3af1d0u;
        snprintf(saved_node.id, sizeof saved_node.id, "!7c3af1d0");
        snprintf(saved_node.name, sizeof saved_node.name, "Trail Relay");
        snprintf(saved_node.shortcode, sizeof saved_node.shortcode, "TRLY");
        saved_node.net = LZ_NET_MT;
        snprintf(saved_node.role, sizeof saved_node.role, "Router");
        saved_node.snr = -5.5f;
        saved_node.batt = 92;
        saved_node.contact = true;
        saved_node.last_heard = 1700000123u;
        saved_node.pos_flags = LZ_NODE_POS_VALID | LZ_NODE_POS_ALT;
        saved_node.lat_i = 451234567;
        saved_node.lon_i = -751234567;
        saved_node.alt_m = 321;
        saved_node.telem_flags = LZ_NODE_TEL_VOLT | LZ_NODE_TEL_UPTIME;
        saved_node.voltage_mv = 4010;
        saved_node.uptime_s = 1200;
        lz_store_save_nodes(&saved_node, 1);
        FILE *nf = fopen("./nodes.db", "r");
        char hdr[32] = {0};
        bool header_ok = nf && fgets(hdr, sizeof hdr, nf) &&
                         strncmp(hdr, "v2|", 3) == 0;
        if(nf) fclose(nf);
        CHECK(header_ok, "store: node DB writes schema v2 tag");
        lz_node_rt loaded_node[2];
        memset(loaded_node, 0, sizeof loaded_node);
        int loaded_n = lz_store_load_nodes(loaded_node, 2);
        CHECK(loaded_n == 1 && loaded_node[0].num == saved_node.num &&
                  loaded_node[0].contact && loaded_node[0].pos_flags == saved_node.pos_flags &&
                  loaded_node[0].telem_flags == saved_node.telem_flags &&
                  loaded_node[0].voltage_mv == 4010 && loaded_node[0].uptime_s == 1200,
              "store: node DB v2 round-trip");
        remove("./wifi.cfg");
        remove("./nodes.db");
        remove("./settings.cfg");
        lz_store_init(NULL);
    }

    /* 10. OTA manifest diagnostics: cached metadata is bounded and fail-closed
     *     before any downloader or inactive-slot writer trusts it. */
    {
        extern void lz_store_init(const char *datadir);
        extern bool lz_store_ota_manifest_status(lz_ota_manifest_t *out);
        extern int  lz_store_ota_manifest_selftest(char *buf, int n);
        sim_reset_dir("lzdata_ota");
        sim_mkdirs("lzdata_ota/ota");
        FILE *mf = fopen("lzdata_ota/ota/manifest.json", "wb");
        if(mf) {
            fputs("{\"schema\":\"limitlezz.ota_manifest.v1\",\"version\":\"0.97.0\","
                  "\"channel\":\"beta\",\"board\":\"tdeck\","
                  "\"firmware_url\":\"https://updates.limitlezz.example/tdeck/0.97.0/firmware.bin\","
                  "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
                  "\"size\":1539920}", mf);
            fclose(mf);
        }
        lz_store_init("lzdata_ota");
        lz_ota_manifest_t ota;
        CHECK(lz_store_ota_manifest_status(&ota) && ota.valid,
              "OTA manifest status accepts cached valid manifest");
        CHECK(strcmp(ota.version, "0.97.0") == 0 && ota.size_bytes == 1539920u,
              "OTA manifest status keeps version and size");

        mf = fopen("lzdata_ota/ota/manifest.json", "wb");
        if(mf) {
            fputs("{\"schema\":\"limitlezz.ota_manifest.v1\",\"version\":\"0.97.0\","
                  "\"channel\":\"beta\",\"board\":\"tdeck\","
                  "\"firmware_url\":\"ftp://updates.example/fw.bin\","
                  "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
                  "\"size\":1539920}", mf);
            fclose(mf);
        }
        CHECK(!lz_store_ota_manifest_status(&ota) && ota.found && !ota.valid &&
              strcmp(ota.error, "bad firmware_url") == 0,
              "OTA manifest status rejects unsafe firmware URL");
        char diag[160];
        lz_store_ota_manifest_selftest(diag, sizeof diag);
        CHECK(strstr(diag, "PASS") != NULL && strstr(diag, "bad sha256") != NULL,
              "OTA manifest selftest reports parser pass");
        lz_store_init(NULL);
        sim_reset_dir("lzdata_ota");
    }
    /* 10. Device PIN verifier: stores only a bounded salted verifier for the
     *     first Phase 12 setup gate; encryption lands in later slices. */
    {
        extern void lz_store_init(const char *datadir);
        extern bool lz_store_security_status(lz_security_status_t *out);
        extern bool lz_store_security_set_pin(const char *pin, char *err, int err_cap);
        extern bool lz_store_security_check_pin(const char *pin);
        extern bool lz_store_security_clear_pin(const char *pin, char *err, int err_cap);
        extern int  lz_store_security_selftest(char *buf, int n);
        sim_reset_dir("lzdata_security");
        lz_store_init("lzdata_security");
        lz_security_status_t sec;
        CHECK(!lz_store_security_status(&sec) && !sec.configured && sec.valid,
              "security status starts with no PIN");
        char err[64] = {0};
        CHECK(!lz_store_security_set_pin("12ab", err, sizeof err) &&
              strcmp(err, "PIN must use digits only") == 0,
              "security PIN rejects non-digits");
        CHECK(lz_store_security_set_pin("123456", err, sizeof err),
              "security PIN verifier stores");
        CHECK(lz_store_security_status(&sec) && sec.configured && sec.valid &&
              sec.rounds == LZ_SECURITY_KDF_ROUNDS,
              "security status reports configured verifier");
        CHECK(lz_store_security_check_pin("123456"), "security PIN accepts correct value");
        CHECK(!lz_store_security_check_pin("000000"), "security PIN rejects wrong value");
        CHECK(!lz_store_security_clear_pin("000000", err, sizeof err) &&
              strcmp(err, "PIN rejected") == 0,
              "security clear requires correct PIN");
        CHECK(lz_store_security_clear_pin("123456", err, sizeof err),
              "security PIN verifier clears");
        CHECK(!lz_store_security_status(&sec) && !sec.configured,
              "security status returns to no PIN");
        char diag[120];
        lz_store_security_selftest(diag, sizeof diag);
        CHECK(strstr(diag, "PASS") != NULL && strstr(diag, "rounds=") != NULL,
              "security PIN selftest reports parser/KDF pass");
        lz_store_init(NULL);
        sim_reset_dir("lzdata_security");
    }

    /* 11. local app scanner: valid manifests become local apps; broken packages
    /* 9b. User settings include the app catalog source selector while still
     *      loading older settings.cfg files with the beginner-safe default. */
    /* 10. settings schema regression: current saves write v3, and older v1/v2
     * files still load with safe defaults for fields that did not exist yet. */
    {
        extern void lz_store_init(const char *datadir);
        extern void lz_store_save_settings(const lz_user_settings_t *s);
        extern bool lz_store_load_settings(lz_user_settings_t *s);
        remove("./settings.cfg");
        lz_store_init(".");
        lz_user_settings_t saved;
        memset(&saved, 0, sizeof saved);
        saved.net_mt = true;
        saved.airtime = LZ_AIRTIME_BALANCED;
        saved.tx = 2;
        saved.bright = 80;
        saved.timeout = 2;
        saved.tz_idx = 1;
        saved.clock24 = true;
        saved.app_source = LZ_APP_SOURCE_COMMUNITY;
        lz_store_save_settings(&saved);
        lz_user_settings_t loaded;
        memset(&loaded, 0, sizeof loaded);
        CHECK(lz_store_load_settings(&loaded), "store: settings reload");
        CHECK(loaded.app_source == LZ_APP_SOURCE_COMMUNITY &&
              loaded.airtime == LZ_AIRTIME_BALANCED && loaded.clock24,
              "store: app source setting round-trip");
        FILE *legacy = fopen("./settings.cfg", "wb");
        if(legacy) {
            fputs("3 1 0 0 3 0 74 1 0 0 0 0 0\n", legacy);
            fclose(legacy);
        }
        memset(&loaded, 0, sizeof loaded);
        CHECK(lz_store_load_settings(&loaded) &&
              loaded.app_source == LZ_APP_SOURCE_OFFICIAL,
              "store: legacy settings default to official app source");

        lz_user_settings_t s; memset(&s, 0, sizeof s);
        s.net_mt = true; s.net_mc = true; s.airtime = LZ_AIRTIME_MC_FIRST;
        s.tx = 2; s.gps = true; s.bright = 61; s.timeout = 4; s.kb_light = 1;
        s.tz_idx = 5; s.clock24 = true; s.save = true; s.developer = true;
        lz_store_save_settings(&s);

        lz_user_settings_t got; memset(&got, 0, sizeof got);
        CHECK(lz_store_load_settings(&got), "store: settings v3 reload");
        CHECK(got.net_mt && got.net_mc && got.airtime == LZ_AIRTIME_MC_FIRST &&
              got.tx == 2 && got.gps && got.bright == 61 && got.timeout == 4 &&
              got.kb_light == 1 && got.tz_idx == 5 && got.clock24 &&
              got.save && got.developer,
              "store: settings v3 round-trip fields");
        FILE *sf = fopen("./settings.cfg", "rb");
        char line[160] = {0};
        bool line_ok = sf && fgets(line, sizeof line, sf) != NULL;
        if(sf) fclose(sf);
        CHECK(line_ok && strncmp(line, "4 ", 2) == 0,
              "store: settings save writes schema v4");

        sf = fopen("./settings.cfg", "wb");
        if(sf) { fputs("1 1 0 2 1 66 3 2 4 1 1\n", sf); fclose(sf); }
        memset(&got, 0, sizeof got);
        CHECK(lz_store_load_settings(&got), "store: legacy settings v1 loads");
        CHECK(got.net_mt && !got.net_mc && got.airtime == LZ_AIRTIME_DEFAULT &&
              got.tx == 2 && got.gps && got.bright == 66 && got.timeout == 3 &&
              got.kb_light == 2 && got.tz_idx == 4 && got.clock24 &&
              got.save && !got.developer,
              "store: legacy settings v1 defaults new fields");

        sf = fopen("./settings.cfg", "wb");
        if(sf) { fputs("2 0 1 1 0 44 2 1 3 0 0 1\n", sf); fclose(sf); }
        memset(&got, 0, sizeof got);
        CHECK(lz_store_load_settings(&got), "store: legacy settings v2 loads");
        CHECK(!got.net_mt && got.net_mc && got.airtime == LZ_AIRTIME_DEFAULT &&
              got.tx == 1 && !got.gps && got.bright == 44 && got.timeout == 2 &&
              got.kb_light == 1 && got.tz_idx == 3 && !got.clock24 &&
              !got.save && got.developer,
              "store: legacy settings v2 keeps developer flag");

        sf = fopen("./settings.cfg", "wb");
        if(sf) { fputs("3 1 1 99 3 0 74 1 0 0 0 0 0\n", sf); fclose(sf); }
        memset(&got, 0, sizeof got);
        CHECK(lz_store_load_settings(&got) && got.airtime == LZ_AIRTIME_DEFAULT,
              "store: settings v3 clamps unknown airtime");

        remove("./settings.cfg");
        lz_store_init(NULL);
    }

    /* 10. app package hash verification foundation for future install/update
     *     flows before staging promotion. */
    {
        extern bool lz_store_file_sha256(const char *path, char *out_hex, int out_cap,
                                         char *err, int err_cap);
        extern bool lz_store_verify_file_sha256(const char *path, const char *expected_hex,
                                                char *err, int err_cap);
        const char *abc_sha =
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        const char *zero_sha =
            "0000000000000000000000000000000000000000000000000000000000000000";
        remove("./pkg_hash.bin");
        FILE *hf = fopen("./pkg_hash.bin", "wb");
        if(hf) {
            fputs("abc", hf);
            fclose(hf);
        }
        char hash[65] = {0};
        char err[48] = {0};
        bool hash_ok = lz_store_file_sha256("./pkg_hash.bin", hash, sizeof hash,
                                            err, sizeof err);
        CHECK(hash_ok && strcmp(hash, abc_sha) == 0,
              "app package SHA256 file hash matches vector");
        CHECK(lz_store_verify_file_sha256(
                  "./pkg_hash.bin",
                  "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
                  err, sizeof err),
              "app package SHA256 verifier accepts expected hash");
        CHECK(!lz_store_verify_file_sha256("./pkg_hash.bin", zero_sha, err, sizeof err) &&
                  strcmp(err, "sha mismatch") == 0,
              "app package SHA256 verifier rejects mismatched hash");
        CHECK(!lz_store_verify_file_sha256("./pkg_hash.bin", "not-a-sha", err, sizeof err) &&
                  strcmp(err, "bad sha256") == 0,
              "app package SHA256 verifier rejects malformed hash");
        remove("./pkg_hash.bin");
    }
    /* 10. app install staging: extracted packages stay hidden until a checked
     *     staging directory is promoted into the live apps tree. */
    {
        extern void lz_store_init(const char *datadir);
        extern int  lz_store_scan_apps(lz_local_app_t *out, int cap);
        extern bool lz_store_prepare_app_install(const char *id, char *staging_path, int staging_cap,
                                                 char *live_path, int live_cap,
                                                 char *err, int err_cap);
        extern bool lz_store_promote_app_install(const char *id, char *err, int err_cap);
        extern bool lz_store_discard_app_install(const char *id, char *err, int err_cap);

        sim_reset_dir("lzdata_appinstall");
        lz_store_init("lzdata_appinstall");
        char staging[160], live[160], err[48];
        bool prep = lz_store_prepare_app_install("weather.mesh", staging, sizeof staging,
                                                 live, sizeof live, err, sizeof err);
        CHECK(prep && strstr(staging, ".install-weather.mesh") != NULL,
              "app install staging path prepares");
        CHECK(prep && strstr(live, "apps/weather.mesh") != NULL,
              "app install live path stays in apps root");
        sim_write_install_app(staging, "weather.mesh", "Weather Mesh", "0.2.0");

        lz_local_app_t install_apps[4];
        CHECK(lz_store_scan_apps(install_apps, 4) == 0,
              "app install staging stays hidden from scanner");
        CHECK(lz_store_promote_app_install("weather.mesh", err, sizeof err),
              "app install staging promotes live package");
        int installed = lz_store_scan_apps(install_apps, 4);
        CHECK(installed == 1 && strcmp(install_apps[0].version, "0.2.0") == 0,
              "app install promoted package scans as live app");

        CHECK(lz_store_prepare_app_install("weather.mesh", staging, sizeof staging,
                                           live, sizeof live, err, sizeof err),
              "app install update staging prepares");
        sim_write_install_app(staging, "wrong.mesh", "Wrong Mesh", "9.9.9");
        CHECK(!lz_store_promote_app_install("weather.mesh", err, sizeof err) &&
                  strcmp(err, "id mismatch") == 0,
              "app install rejects staged id mismatch");
        installed = lz_store_scan_apps(install_apps, 4);
        CHECK(installed == 1 && strcmp(install_apps[0].version, "0.2.0") == 0,
              "app install failed promote leaves prior app intact");
        CHECK(lz_store_discard_app_install("weather.mesh", err, sizeof err),
              "app install staging discard succeeds");
    }
    /* 10. local app uninstall: users can delete a package while either
     *     retaining scoped data for reinstall or deleting everything. */
    {
        extern void lz_store_init(const char *datadir);
        extern int  lz_store_scan_apps(lz_local_app_t *out, int cap);
        extern int  lz_store_scan_app_issues(lz_local_app_issue_t *out, int cap);
        extern bool lz_store_prepare_app_data(const lz_local_app_t *app, char *path_out, int path_cap,
                                              char *err, int err_cap);
        extern bool lz_store_app_data_usage(const lz_local_app_t *app, uint32_t *used, uint32_t *quota,
                                            char *err, int err_cap);
        extern bool lz_store_uninstall_local_app(const lz_local_app_t *app, bool keep_data,
                                                 char *err, int err_cap);
        sim_reset_dir("lzdata_appuninstall");
        sim_write_local_app("lzdata_appuninstall", "weather", "weather.mesh", "Weather Mesh",
                            "main.lua", "weather", 48, "Local weather dashboard",
                            "[\"display\",\"input\",\"storage\"]");
        lz_store_init("lzdata_appuninstall");
        lz_local_app_t apps[4];
        int an = lz_store_scan_apps(apps, 4);
        CHECK(an == 1, "local app uninstall test app scans");
        char data_path[128], err[48];
        CHECK(an == 1 && lz_store_prepare_app_data(&apps[0], data_path, sizeof data_path,
                                                   err, sizeof err),
              "local app uninstall data prepares");
        char cache_path[160];
        snprintf(cache_path, sizeof cache_path, "%s/cache.bin", data_path);
        sim_write_bytes(cache_path, 1536);

        CHECK(an == 1 && lz_store_uninstall_local_app(&apps[0], true, err, sizeof err),
              "local app uninstall can retain data");
        CHECK(lz_store_scan_apps(apps, 4) == 0,
              "local app uninstall removes live package");
        lz_local_app_issue_t issues[4];
        CHECK(lz_store_scan_app_issues(issues, 4) == 0,
              "local app retained data stays hidden from diagnostics");

        sim_write_local_app("lzdata_appuninstall", "weather", "weather.mesh", "Weather Mesh",
                            "main.lua", "weather", 48, "Local weather dashboard",
                            "[\"display\",\"input\",\"storage\"]");
        an = lz_store_scan_apps(apps, 4);
        CHECK(an == 1 && lz_store_prepare_app_data(&apps[0], data_path, sizeof data_path,
                                                   err, sizeof err),
              "local app reinstall restores retained data");
        uint32_t used = 0, quota = 0;
        CHECK(an == 1 && lz_store_app_data_usage(&apps[0], &used, &quota, err, sizeof err) &&
                  used == 1536,
              "local app retained data survives reinstall");
        CHECK(an == 1 && lz_store_uninstall_local_app(&apps[0], false, err, sizeof err),
              "local app uninstall can delete data");
        CHECK(lz_store_scan_apps(apps, 4) == 0,
              "local app delete-data uninstall removes package");
    }
    /* 10. app catalog cache: future Wi-Fi fetches can persist bounded catalog
     *     JSON for offline browsing without parsing it in the store layer. */
    {
        extern void lz_store_init(const char *datadir);
        extern bool lz_store_save_app_catalog_cache(const char *json, int len,
                                                    char *err, int err_cap);
        extern bool lz_store_load_app_catalog_cache(char *out, int cap, int *out_len,
                                                    char *err, int err_cap);
        extern bool lz_store_clear_app_catalog_cache(char *err, int err_cap);
        const char *catalog =
            "{\"apps\":[{\"id\":\"weather\",\"version\":\"0.2.0\",\"sha256\":\"abc\"}]}";
        char out[160];
        char err[48];
        int out_len = 0;
        remove("./app_catalog.json");
        lz_store_init(".");
        CHECK(!lz_store_load_app_catalog_cache(out, sizeof out, &out_len, err, sizeof err) &&
                  strcmp(err, "catalog missing") == 0,
              "app catalog cache reports missing cache");
        CHECK(lz_store_save_app_catalog_cache(catalog, (int)strlen(catalog), err, sizeof err),
              "app catalog cache saves bounded JSON");
        CHECK(lz_store_load_app_catalog_cache(out, sizeof out, &out_len, err, sizeof err) &&
                  out_len == (int)strlen(catalog) && strcmp(out, catalog) == 0,
              "app catalog cache reloads exact JSON");
        CHECK(!lz_store_load_app_catalog_cache(out, 8, &out_len, err, sizeof err) &&
                  strcmp(err, "catalog buffer small") == 0,
              "app catalog cache rejects undersized read buffer");
        static char too_big[LZ_APP_CATALOG_CACHE_MAX + 1];
        memset(too_big, 'x', sizeof too_big);
        CHECK(!lz_store_save_app_catalog_cache(too_big, (int)sizeof too_big, err, sizeof err) &&
                  strcmp(err, "catalog too large") == 0,
              "app catalog cache rejects oversized JSON");
        CHECK(lz_store_clear_app_catalog_cache(err, sizeof err),
              "app catalog cache clears saved catalog");
        CHECK(!lz_store_load_app_catalog_cache(out, sizeof out, &out_len, err, sizeof err) &&
                  strcmp(err, "catalog missing") == 0,
              "app catalog cache clear removes file");
        remove("./app_catalog.json");
        lz_store_init(NULL);
    }
    /* 10. app catalog fetch transport: native keeps this as a deterministic
     *     no-network stub; T-Deck provides the Wi-Fi HTTP implementation. */
    {
        char body[96];
        char err[48];
        int body_len = 7;
        CHECK(!lz_app_catalog_fetch("ftp://example.invalid/index.json", body, sizeof body,
                                    &body_len, err, sizeof err) &&
                  body_len == 0 && strcmp(err, "bad url") == 0,
              "app catalog fetch rejects unsupported URL schemes");
        body_len = 7;
        CHECK(!lz_app_catalog_fetch("https://example.invalid/index.json", body, 1,
                                    &body_len, err, sizeof err) &&
                  body_len == 0 && strcmp(err, "catalog buffer small") == 0,
              "app catalog fetch rejects tiny output buffer");
        body_len = 7;
        CHECK(!lz_app_catalog_fetch("https://example.invalid/index.json", body, sizeof body,
                                    &body_len, err, sizeof err) &&
                  body_len == 0 && strcmp(err, "fetch unavailable") == 0,
              "app catalog fetch native stub is explicit");
    }
    /* 10. OTA boot policy: fail closed while a new image is pending verify. */
    {
        char err[64];
        CHECK(lz_ota_boot_selftest(err, sizeof err),
              "OTA boot policy selftest");
    }

    /* 11. local app scanner: valid manifests become local apps; broken packages
     *    are ignored before they can reach Home/App Store. */
    {
        extern void lz_store_init(const char *datadir);
        extern void lz_store_set_appfs_root(const char *root);
        extern int  lz_store_file_roots(const char **out, int cap);
        extern int  lz_store_scan_apps(lz_local_app_t *out, int cap);
        extern int  lz_store_scan_app_issues(lz_local_app_issue_t *out, int cap);
        extern bool lz_store_prepare_app_data(const lz_local_app_t *app, char *path_out, int path_cap,
                                              char *err, int err_cap);
        extern bool lz_store_app_data_usage(const lz_local_app_t *app, uint32_t *used, uint32_t *quota,
                                            char *err, int err_cap);
        extern bool lz_store_clear_app_data(const lz_local_app_t *app, char *err, int err_cap);
        extern bool lz_store_start_local_app(const lz_local_app_t *app, lz_local_app_session_t *out);
        extern bool lz_store_local_app_action(lz_local_app_session_t *session, int idx);
        extern void lz_store_stop_local_app(lz_local_app_session_t *session);
        sim_reset_dir("lzdata_appscan");
        sim_mkdirs("lzdata_appscan/apps/weather");
        sim_mkdirs("lzdata_appscan/apps/bad");
        sim_mkdirs("lzdata_appscan/apps/badperm");
        FILE *mf = fopen("lzdata_appscan/apps/weather/manifest.json", "wb");
        if(mf) {
            fputs("{\"id\":\"weather.mesh\",\"name\":\"Weather Mesh\",\"version\":\"0.1.0\","
                  "\"author\":\"Limitless\",\"entry\":\"main.lua\",\"icon\":\"weather\","
                  "\"hue\":48,\"api_version\":\"0.1\","
                  "\"permissions\":[\"display\",\"input\",\"storage\"],"
                  "\"summary\":\"Local weather dashboard\"}", mf);
            fclose(mf);
        }
        FILE *entry = fopen("lzdata_appscan/apps/weather/main.lua", "wb");
        if(entry) {
            fputs("-- title: Weather Mesh\n"
                  "-- status: SDK 0.1 foreground sandbox\n"
                  "-- body: Local weather dashboard\n"
                  "-- action: Refresh | Forecast refreshed #{count} | Fresh local forecast count {count} | counter:refreshes\n"
                  "return true\n", entry);
            fclose(entry);
        }
        FILE *bad = fopen("lzdata_appscan/apps/bad/manifest.json", "wb");
        if(bad) { fputs("{\"id\":\"../bad\",\"name\":\"Bad\",\"entry\":\"missing.lua\"}", bad); fclose(bad); }
        FILE *bpm = fopen("lzdata_appscan/apps/badperm/manifest.json", "wb");
        if(bpm) {
            fputs("{\"id\":\"badperm.local\",\"name\":\"Bad Perm\",\"entry\":\"main.lua\","
                  "\"permissions\":[\"display\",\"raw_radio\"]}", bpm);
            fclose(bpm);
        }
        FILE *bpe = fopen("lzdata_appscan/apps/badperm/main.lua", "wb");
        if(bpe) { fputs("return true\n", bpe); fclose(bpe); }

        lz_store_init("lzdata_appscan");
        lz_local_app_t apps[4];
        int an = lz_store_scan_apps(apps, 4);
        CHECK(an == 1, "local app scanner loads one valid manifest");
        CHECK(an == 1 && strcmp(apps[0].id, "weather.mesh") == 0, "local app scanner keeps manifest id");
        CHECK(an == 1 && strcmp(apps[0].entry, "main.lua") == 0, "local app scanner keeps safe entry");
        CHECK(an == 1 && apps[0].hue == 48, "local app scanner keeps tile hue");
        CHECK(an == 1 && strcmp(apps[0].api_version, "0.1") == 0, "local app scanner keeps API version");
        CHECK(an == 1 && (apps[0].permissions & LZ_APP_PERM_DISPLAY) &&
              (apps[0].permissions & LZ_APP_PERM_INPUT) &&
              (apps[0].permissions & LZ_APP_PERM_STORAGE) &&
              !(apps[0].permissions & LZ_APP_PERM_MESH_SEND),
              "local app scanner keeps allowlisted permissions");
        char perm_ids[96], perm_text[220], broad_text[220], none_text[64];
        lz_app_permissions_list(apps[0].permissions, perm_ids, sizeof perm_ids);
        lz_app_permissions_summary(apps[0].permissions, perm_text, sizeof perm_text);
        lz_app_permissions_summary(LZ_APP_PERM_MESH_SEND | LZ_APP_PERM_NETWORK_WIFI |
                                   LZ_APP_PERM_NOTIFICATIONS,
                                   broad_text, sizeof broad_text);
        lz_app_permissions_summary(0, none_text, sizeof none_text);
        CHECK(strstr(perm_ids, "storage") && strstr(perm_text, "own data"),
              "local app permissions get user-facing storage summary");
        CHECK(strstr(broad_text, "send mesh messages") &&
              strstr(broad_text, "Wi-Fi") &&
              strstr(broad_text, "notifications"),
              "local app permissions explain broad access requests");
        CHECK(strcmp(none_text, "No app permissions requested.") == 0,
              "local app permissions explain empty access request");
        char data_path[128], data_err[48];
        bool data_ok = an == 1 && lz_store_prepare_app_data(&apps[0], data_path, sizeof data_path,
                                                            data_err, sizeof data_err);
        CHECK(data_ok, "local app storage sandbox prepares");
        CHECK(data_ok && strstr(data_path, "weather/data") != NULL,
              "local app storage sandbox stays inside package");
        sim_write_bytes("lzdata_appscan/apps/weather/data/cache.bin", 1536);
        uint32_t used = 0, quota = 0;
        bool usage_ok = an == 1 && lz_store_app_data_usage(&apps[0], &used, &quota,
                                                           data_err, sizeof data_err);
        CHECK(usage_ok && used == 1536, "local app data quota counts bytes");
        CHECK(usage_ok && quota == LZ_LOCAL_APP_DATA_QUOTA_BYTES, "local app data quota is reported");
        lz_local_app_session_t run;
        bool run_ok = an == 1 && lz_store_start_local_app(&apps[0], &run);
        CHECK(run_ok, "local app foreground session starts");
        CHECK(run_ok && run.entry_loaded && strcmp(run.title, "Weather Mesh") == 0,
              "local app foreground session reads entry metadata");
        CHECK(run_ok && strstr(run.body, "Local weather dashboard") != NULL,
              "local app foreground session renders bounded body text");
        CHECK(run_ok && run.storage_ready && strstr(run.data_path, "weather/data") != NULL &&
              run.data_used_bytes == 1536,
              "local app foreground session keeps scoped storage");
        CHECK(run_ok && run.action_count == 1 && strcmp(run.actions[0].label, "Refresh") == 0,
              "local app foreground session exposes bounded action");
        CHECK(run_ok && run.runtime_used_bytes > 0 &&
              run.runtime_used_bytes <= run.runtime_budget_bytes &&
              run.runtime_budget_bytes == LZ_LOCAL_APP_RUNTIME_BUDGET_BYTES,
              "local app foreground session reports runtime memory budget");
        bool action_ok = run_ok && lz_store_local_app_action(&run, 0);
        CHECK(action_ok && run.action_last == 1 &&
              strcmp(run.status, "Forecast refreshed #1") == 0 &&
              strstr(run.body, "count 1") != NULL,
              "local app foreground action updates sandbox session");
        bool action2_ok = run_ok && lz_store_local_app_action(&run, 0);
        CHECK(action2_ok && strcmp(run.status, "Forecast refreshed #2") == 0 &&
              strstr(run.body, "count 2") != NULL,
              "local app foreground storage counter persists");
        CHECK(action2_ok && run.data_used_bytes > 1536,
              "local app foreground storage counter stays in app data quota");
        lz_local_app_session_t action_fault = run;
        snprintf(action_fault.data_path, sizeof action_fault.data_path,
                 "lzdata_appscan/missing-counter-data");
        bool action_fault_ok = lz_store_local_app_action(&action_fault, 0);
        CHECK(action_fault_ok &&
                  strcmp(action_fault.fault, "action: storage write failed") == 0 &&
                  strcmp(action_fault.status, "Action failed") == 0,
              "local app foreground captures action storage faults");
        snprintf(action_fault.data_path, sizeof action_fault.data_path, "%s", run.data_path);
        bool action_recover_ok = lz_store_local_app_action(&action_fault, 0);
        CHECK(action_recover_ok && action_fault.fault[0] == 0 &&
                  strcmp(action_fault.status, "Forecast refreshed #3") == 0,
              "local app foreground clears recovered action faults");
        lz_store_stop_local_app(&run);
        CHECK(!run.entry_loaded && run.action_count == 0 && run.data_path[0] == 0,
              "local app foreground session terminates cleanly");
        CHECK(!lz_store_local_app_action(&run, 0),
              "local app foreground actions stop after termination");
        bool clear_ok = an == 1 && lz_store_clear_app_data(&apps[0], data_err, sizeof data_err);
        CHECK(clear_ok, "local app clear data succeeds inside scoped storage");
        used = 123;
        usage_ok = an == 1 && lz_store_app_data_usage(&apps[0], &used, &quota,
                                                      data_err, sizeof data_err);
        CHECK(usage_ok && used == 0, "local app clear data resets quota usage");
        sim_write_bytes("lzdata_appscan/apps/weather/data/too-big.bin",
                        (int)LZ_LOCAL_APP_DATA_QUOTA_BYTES + 1);
        lz_local_app_session_t over;
        bool over_ok = an == 1 && lz_store_start_local_app(&apps[0], &over);
        CHECK(!over_ok && strcmp(over.error, "data quota exceeded") == 0 &&
                  strcmp(over.fault, "launch: data quota exceeded") == 0,
              "local app foreground session blocks over-quota data");
        sim_mkdirs("lzdata_appscan/apps/noinput");
        FILE *nim = fopen("lzdata_appscan/apps/noinput/manifest.json", "wb");
        if(nim) {
            fputs("{\"id\":\"noinput.local\",\"name\":\"No Input\",\"entry\":\"main.lua\","
                  "\"permissions\":[\"display\"]}", nim);
            fclose(nim);
        }
        FILE *nie = fopen("lzdata_appscan/apps/noinput/main.lua", "wb");
        if(nie) {
            fputs("-- body: Display-only app\n"
                  "-- action: Press me | Should not run | Missing input permission\n", nie);
            fclose(nie);
        }
        lz_local_app_t action_apps[4];
        int actn = lz_store_scan_apps(action_apps, 4);
        lz_local_app_t *noinput = NULL;
        for(int i = 0; i < actn; i++)
            if(strcmp(action_apps[i].id, "noinput.local") == 0) noinput = &action_apps[i];
        lz_local_app_session_t noinput_run;
        bool noinput_ok = noinput && lz_store_start_local_app(noinput, &noinput_run);
        CHECK(!noinput_ok && noinput && strcmp(noinput_run.error, "input permission missing") == 0,
              "local app foreground action requires input permission");
        sim_mkdirs("lzdata_appscan/apps/nostore");
        FILE *nsm = fopen("lzdata_appscan/apps/nostore/manifest.json", "wb");
        if(nsm) {
            fputs("{\"id\":\"nostore.local\",\"name\":\"No Store\",\"entry\":\"main.lua\","
                  "\"permissions\":[\"display\",\"input\"]}", nsm);
            fclose(nsm);
        }
        FILE *nse = fopen("lzdata_appscan/apps/nostore/main.lua", "wb");
        if(nse) {
            fputs("-- body: Input app without storage\n"
                  "-- action: Count | Count #{count} | Missing storage permission | counter:presses\n", nse);
            fclose(nse);
        }
        lz_local_app_t perm_apps[LZ_MAX_LOCAL_APPS];
        int permn = lz_store_scan_apps(perm_apps, LZ_MAX_LOCAL_APPS);
        lz_local_app_t *nostore = NULL;
        for(int i = 0; i < permn; i++)
            if(strcmp(perm_apps[i].id, "nostore.local") == 0) nostore = &perm_apps[i];
        lz_local_app_session_t nostore_run;
        bool nostore_ok = nostore && lz_store_start_local_app(nostore, &nostore_run);
        CHECK(!nostore_ok && nostore && strcmp(nostore_run.error, "storage permission missing") == 0,
              "local app foreground storage action requires storage permission");
        sim_mkdirs("lzdata_appscan/apps/badcounter");
        FILE *bcm = fopen("lzdata_appscan/apps/badcounter/manifest.json", "wb");
        if(bcm) {
            fputs("{\"id\":\"badcounter.local\",\"name\":\"Bad Counter\",\"entry\":\"main.lua\","
                  "\"permissions\":[\"display\",\"input\",\"storage\"]}", bcm);
            fclose(bcm);
        }
        FILE *bce = fopen("lzdata_appscan/apps/badcounter/main.lua", "wb");
        if(bce) {
            fputs("-- body: Counter effect with unsafe key\n"
                  "-- action: Count | Counted | Should not run | counter:../shared\n", bce);
            fclose(bce);
        }
        sim_mkdirs("lzdata_appscan/apps/badeffect");
        FILE *bem = fopen("lzdata_appscan/apps/badeffect/manifest.json", "wb");
        if(bem) {
            fputs("{\"id\":\"badeffect.local\",\"name\":\"Bad Effect\",\"entry\":\"main.lua\","
                  "\"permissions\":[\"display\",\"input\"]}", bem);
            fclose(bem);
        }
        FILE *bee = fopen("lzdata_appscan/apps/badeffect/main.lua", "wb");
        if(bee) {
            fputs("-- body: Unsupported action effect\n"
                  "-- action: Raw TX | Should not run | Raw radio must fail closed | radio:raw\n", bee);
            fclose(bee);
        }
        lz_local_app_t effect_apps[LZ_MAX_LOCAL_APPS];
        int effectn = lz_store_scan_apps(effect_apps, LZ_MAX_LOCAL_APPS);
        lz_local_app_t *badcounter = NULL, *badeffect = NULL;
        for(int i = 0; i < effectn; i++) {
            if(strcmp(effect_apps[i].id, "badcounter.local") == 0) badcounter = &effect_apps[i];
            if(strcmp(effect_apps[i].id, "badeffect.local") == 0) badeffect = &effect_apps[i];
        }
        lz_local_app_session_t badcounter_run;
        bool badcounter_ok = badcounter && lz_store_start_local_app(badcounter, &badcounter_run);
        CHECK(!badcounter_ok && badcounter &&
              strcmp(badcounter_run.error, "bad action effect") == 0 &&
              strcmp(badcounter_run.fault, "launch: bad action effect") == 0,
              "local app foreground rejects malformed action effects");
        lz_local_app_session_t badeffect_run;
        bool badeffect_ok = badeffect && lz_store_start_local_app(badeffect, &badeffect_run);
        CHECK(!badeffect_ok && badeffect &&
              strcmp(badeffect_run.error, "unsupported action effect") == 0,
              "local app foreground rejects unsupported action effects");
        sim_mkdirs("lzdata_appscan/apps/fatmeta");
        FILE *fmm = fopen("lzdata_appscan/apps/fatmeta/manifest.json", "wb");
        if(fmm) {
            fputs("{\"id\":\"fatmeta.local\",\"name\":\"Fat Metadata\",\"entry\":\"main.lua\","
                  "\"permissions\":[\"display\",\"input\",\"storage\"]}", fmm);
            fclose(fmm);
        }
        FILE *fme = fopen("lzdata_appscan/apps/fatmeta/main.lua", "wb");
        if(fme) {
            fputs("-- title: Fat Metadata\n-- status: ", fme);
            sim_fwrite_repeat(fme, 's', 63);
            fputs("\n-- body: ", fme);
            sim_fwrite_repeat(fme, 'b', 95);
            fputs("\n-- body: ", fme);
            sim_fwrite_repeat(fme, 'c', 95);
            fputs("\n-- body: ", fme);
            sim_fwrite_repeat(fme, 'd', 95);
            fputs("\n-- action: ", fme);
            sim_fwrite_repeat(fme, 'l', 23);
            fputs(" | ", fme);
            sim_fwrite_repeat(fme, 'a', 47);
            fputs(" | ", fme);
            sim_fwrite_repeat(fme, 'c', 10);
            fputs(" | counter:abcdefghijklmnopqrs\n-- action: ", fme);
            sim_fwrite_repeat(fme, 'm', 23);
            fputs(" | ", fme);
            sim_fwrite_repeat(fme, 'd', 47);
            fputs(" | ", fme);
            sim_fwrite_repeat(fme, 'e', 10);
            fputs(" | counter:tsrqponmlkjihgfedcb\n", fme);
            fclose(fme);
        }
        lz_local_app_t budget_apps[LZ_MAX_LOCAL_APPS];
        int budgetn = lz_store_scan_apps(budget_apps, LZ_MAX_LOCAL_APPS);
        lz_local_app_t *fatmeta = NULL;
        for(int i = 0; i < budgetn; i++)
            if(strcmp(budget_apps[i].id, "fatmeta.local") == 0) fatmeta = &budget_apps[i];
        lz_local_app_session_t fatmeta_run;
        bool fatmeta_ok = fatmeta && lz_store_start_local_app(fatmeta, &fatmeta_run);
        CHECK(!fatmeta_ok && fatmeta &&
              strcmp(fatmeta_run.error, "runtime memory cap exceeded") == 0,
              "local app foreground session blocks runtime metadata over budget");
        CHECK(fatmeta && fatmeta_run.runtime_used_bytes > fatmeta_run.runtime_budget_bytes,
              "local app runtime budget records overage");
        sim_mkdirs("lzdata_appscan/apps/huge");
        FILE *hm = fopen("lzdata_appscan/apps/huge/manifest.json", "wb");
        if(hm) {
            fputs("{\"id\":\"huge.local\",\"name\":\"Huge Entry\",\"entry\":\"main.lua\"}", hm);
            fclose(hm);
        }
        sim_write_bytes("lzdata_appscan/apps/huge/main.lua", (int)LZ_LOCAL_APP_ENTRY_MAX + 1);
        lz_local_app_issue_t issues[4];
        int in = lz_store_scan_app_issues(issues, 4);
        bool bad_id = false, bad_perm = false;
        lz_local_app_t huge_apps[LZ_MAX_LOCAL_APPS];
        int hn = lz_store_scan_apps(huge_apps, LZ_MAX_LOCAL_APPS);
        lz_local_app_t *huge = NULL;
        for(int i = 0; i < hn; i++)
            if(strcmp(huge_apps[i].id, "huge.local") == 0) huge = &huge_apps[i];
        lz_local_app_session_t huge_run;
        bool huge_ok = huge && lz_store_start_local_app(huge, &huge_run);
        CHECK(!huge_ok && huge && strcmp(huge_run.error, "entry too large") == 0,
              "local app foreground session blocks oversized entry");
        for(int i = 0; i < in; i++) {
            if(strcmp(issues[i].package, "bad") == 0 &&
               strcmp(issues[i].reason, "unsafe id") == 0) bad_id = true;
            if(strcmp(issues[i].package, "badperm") == 0 &&
               strcmp(issues[i].reason, "bad permissions") == 0) bad_perm = true;
        }
        CHECK(in == 2, "local app diagnostics report rejected packages");
        CHECK(bad_id, "local app diagnostics explain unsafe id");
        CHECK(bad_perm, "local app diagnostics explain bad permissions");
        lz_store_init(NULL);
        lz_store_set_appfs_root(NULL);
        sim_reset_dir("lzdata_appscan");
    }

    /* 11. network app catalog schema: validate bounded future install indexes
     *     before fetch/download/install code trusts them. */
    {
        static const char valid_catalog[] =
            "{\"schema\":\"limitlezz.app_catalog.v1\",\"updated\":\"2026-06-18T00:00:00Z\","
            "\"apps\":["
            "{\"id\":\"weather.mesh\",\"name\":\"Weather Mesh\",\"version\":\"0.1.0\","
            "\"author\":\"Limitless\",\"description\":\"Local weather reports\","
            "\"icon\":\"weather\",\"hue\":48,\"api_version\":\"0.1\","
            "\"compatibility\":\"tdeck\",\"permissions\":[\"display\",\"network_wifi\"],"
            "\"download_url\":\"https://apps.example.invalid/weather.mesh.zip\","
            "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
            "\"size\":32768,\"screenshots\":[\"https://apps.example.invalid/weather.bmp\"]},"
            "{\"id\":\"notes.local\",\"name\":\"Field Notes\",\"version\":\"0.1.0\","
            "\"author\":\"Limitless\",\"description\":\"Simple local notes\","
            "\"icon\":\"notes\",\"api_version\":\"0.1\",\"compatibility\":\"tdeck\","
            "\"permissions\":[\"display\",\"input\",\"storage\"],"
            "\"download_url\":\"https://apps.example.invalid/notes.local.zip\","
            "\"sha256\":\"abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd\","
            "\"size\":49152}]}";
        static const char bad_catalog[] =
            "{\"schema\":\"limitlezz.app_catalog.v1\",\"apps\":["
            "{\"id\":\"weather.mesh\",\"name\":\"Weather Mesh\",\"version\":\"0.1.0\","
            "\"author\":\"Limitless\",\"description\":\"Bad catalog\",\"icon\":\"weather\","
            "\"api_version\":\"0.1\",\"compatibility\":\"tdeck\","
            "\"permissions\":[\"display\",\"raw_radio\"],"
            "\"download_url\":\"file:///sd/apps/weather.zip\","
            "\"sha256\":\"bad\",\"size\":0}]}";

        lz_app_catalog_report_t report;
        CHECK(lz_svc_validate_app_catalog_json(valid_catalog, &report) &&
              report.ok && report.app_count == 2 && report.rejected_count == 0,
              "app catalog schema accepts valid bounded index");
        CHECK(!lz_svc_validate_app_catalog_json(bad_catalog, &report) &&
              !report.ok && report.rejected_count == 1 &&
              strcmp(report.first_error, "bad download_url") == 0,
              "app catalog schema rejects unsafe download URL");
        char self[160];
        lz_svc_app_catalog_selftest(self, sizeof self);
        CHECK(strstr(self, "PASS") != NULL,
              "app catalog selftest covers valid and invalid indexes");

        extern void lz_store_init(const char *datadir);
        sim_reset_dir("lzdata_catalog");
        sim_mkdirs("lzdata_catalog/catalog");
        FILE *cf = fopen("lzdata_catalog/catalog/index.json", "wb");
        if(cf) { fputs(valid_catalog, cf); fclose(cf); }
        lz_store_init("lzdata_catalog");
        char diag[160];
        lz_svc_app_catalog_diag(diag, sizeof diag);
        CHECK(strstr(diag, "ready apps=2") != NULL,
              "app catalog diagnostics report cached index");
        lz_store_init(NULL);
        sim_reset_dir("lzdata_catalog");
    }

    /* 12. service-level SDK token injection: dynamic read-only values are
     *     expanded only when the matching permissions are declared. */
    {
        extern void lz_store_init(const char *datadir);
        sim_reset_dir("lzdata_apptokens");
        sim_mkdirs("lzdata_apptokens/apps/status");
        FILE *mf = fopen("lzdata_apptokens/apps/status/manifest.json", "wb");
        if(mf) {
            fputs("{\"id\":\"status.local\",\"name\":\"Status Card\",\"entry\":\"main.lua\","
                  "\"permissions\":[\"display\",\"input\",\"storage\",\"system_time\",\"battery\"]}",
                  mf);
            fclose(mf);
        }
        FILE *entry = fopen("lzdata_apptokens/apps/status/main.lua", "wb");
        if(entry) {
            fputs("-- status: Opened at {time}\n"
                  "-- body: Battery {battery} at {time}\n"
                  "-- action: Refresh | Refreshed {time} | Battery {battery} count {count} | counter:refreshes\n",
                  entry);
            fclose(entry);
        }
        sim_mkdirs("lzdata_apptokens/apps/notime");
        FILE *ntm = fopen("lzdata_apptokens/apps/notime/manifest.json", "wb");
        if(ntm) {
            fputs("{\"id\":\"notime.local\",\"name\":\"No Time\",\"entry\":\"main.lua\","
                  "\"permissions\":[\"display\",\"input\"]}", ntm);
            fclose(ntm);
        }
        FILE *nte = fopen("lzdata_apptokens/apps/notime/main.lua", "wb");
        if(nte) { fputs("-- body: Needs {time}\n", nte); fclose(nte); }
        sim_mkdirs("lzdata_apptokens/apps/nobattery");
        FILE *nbm = fopen("lzdata_apptokens/apps/nobattery/manifest.json", "wb");
        if(nbm) {
            fputs("{\"id\":\"nobattery.local\",\"name\":\"No Battery\",\"entry\":\"main.lua\","
                  "\"permissions\":[\"display\",\"input\",\"system_time\"]}", nbm);
            fclose(nbm);
        }
        FILE *nbe = fopen("lzdata_apptokens/apps/nobattery/main.lua", "wb");
        if(nbe) { fputs("-- body: Needs {battery}\n", nbe); fclose(nbe); }
        sim_mkdirs("lzdata_apptokens/apps/notifier");
        FILE *nfm = fopen("lzdata_apptokens/apps/notifier/manifest.json", "wb");
        if(nfm) {
            fputs("{\"id\":\"notify.local\",\"name\":\"Notifier\",\"entry\":\"main.lua\","
                  "\"permissions\":[\"display\",\"input\",\"notifications\"]}", nfm);
            fclose(nfm);
        }
        FILE *nfe = fopen("lzdata_apptokens/apps/notifier/main.lua", "wb");
        if(nfe) {
            fputs("-- body: Permissioned app notification\n"
                  "-- action: Alert | Notification sent | Feedback service recorded it | notify:Field alert ready\n",
                  nfe);
            fclose(nfe);
        }
        sim_mkdirs("lzdata_apptokens/apps/nonotify");
        FILE *nnm = fopen("lzdata_apptokens/apps/nonotify/manifest.json", "wb");
        if(nnm) {
            fputs("{\"id\":\"nonotify.local\",\"name\":\"No Notify\",\"entry\":\"main.lua\","
                  "\"permissions\":[\"display\",\"input\"]}", nnm);
            fclose(nnm);
        }
        FILE *nne = fopen("lzdata_apptokens/apps/nonotify/main.lua", "wb");
        if(nne) {
            fputs("-- body: Missing notification permission\n"
                  "-- action: Alert | Notification sent | Should not route | notify:Denied alert\n",
                  nne);
            fclose(nne);
        }

        lz_svc_init("lzdata_apptokens", false);
        lz_svc_set_time(1781274180);
        lz_local_app_t apps[LZ_MAX_LOCAL_APPS];
        int an = lz_svc_scan_apps(apps, LZ_MAX_LOCAL_APPS);
        lz_local_app_t *status = NULL, *notime = NULL, *nobattery = NULL;
        lz_local_app_t *notify = NULL, *nonotify = NULL;
        for(int i = 0; i < an; i++) {
            if(strcmp(apps[i].id, "status.local") == 0) status = &apps[i];
            if(strcmp(apps[i].id, "notime.local") == 0) notime = &apps[i];
            if(strcmp(apps[i].id, "nobattery.local") == 0) nobattery = &apps[i];
            if(strcmp(apps[i].id, "notify.local") == 0) notify = &apps[i];
            if(strcmp(apps[i].id, "nonotify.local") == 0) nonotify = &apps[i];
        }
        lz_local_app_session_t run;
        bool run_ok = status && lz_svc_start_local_app(status, &run);
        CHECK(run_ok && strstr(run.status, "{time}") == NULL &&
              strstr(run.body, "{battery}") == NULL && strstr(run.body, "87%") != NULL,
              "local app SDK tokens expand with declared permissions");
        bool action_ok = run_ok && lz_svc_local_app_action(&run, 0);
        CHECK(action_ok && strstr(run.status, "{time}") == NULL &&
              strstr(run.body, "87%") != NULL && strstr(run.body, "count 1") != NULL,
              "local app SDK tokens expand after foreground action");
        lz_local_app_session_t denied_time;
        bool denied_time_ok = notime && lz_svc_start_local_app(notime, &denied_time);
        CHECK(!denied_time_ok && notime && strcmp(denied_time.error, "time permission missing") == 0,
              "local app SDK time token requires system_time permission");
        lz_local_app_session_t denied_battery;
        bool denied_battery_ok = nobattery && lz_svc_start_local_app(nobattery, &denied_battery);
        CHECK(!denied_battery_ok && nobattery &&
              strcmp(denied_battery.error, "battery permission missing") == 0,
              "local app SDK battery token requires battery permission");
        lz_feedback_status_t fb_before, fb_after;
        lz_svc_feedback_status(&fb_before);
        lz_local_app_session_t notify_run;
        bool notify_ok = notify && lz_svc_start_local_app(notify, &notify_run);
        bool notify_action_ok = notify_ok && lz_svc_local_app_action(&notify_run, 0);
        lz_svc_feedback_status(&fb_after);
        CHECK(notify_action_ok &&
              fb_after.request_count == fb_before.request_count + 1 &&
              strcmp(fb_after.last_source, "Notifier") == 0 &&
              strcmp(fb_after.last_title, "Alert") == 0 &&
              strcmp(fb_after.last_body, "Field alert ready") == 0,
              "local app notification action routes through feedback service");
        lz_local_app_session_t nonotify_run;
        bool nonotify_ok = nonotify && lz_svc_start_local_app(nonotify, &nonotify_run);
        CHECK(!nonotify_ok && nonotify &&
              strcmp(nonotify_run.error, "notifications permission missing") == 0,
              "local app notification action requires notifications permission");
        char fb_diag[180], fb_test[120];
        int fb_diag_n = lz_svc_feedback_diag(fb_diag, sizeof fb_diag);
        int fb_test_ok = lz_svc_feedback_selftest(fb_test, sizeof fb_test);
        CHECK(fb_diag_n > 0 && strstr(fb_diag, "feedback: ready") != NULL,
              "feedback diagnostics report service status");
        CHECK(fb_test_ok == 1 && strstr(fb_test, "PASS") != NULL,
              "feedback selftest records a notification request");
        lz_store_init(NULL);
        sim_reset_dir("lzdata_apptokens");
    }

    /* 13. appfs root support: apps can be discovered from appfs even when
     *     SD-backed persistence is absent, and Files can expose both roots. */
    {
        extern void lz_store_init(const char *datadir);
        extern void lz_store_set_appfs_root(const char *root);
        extern int  lz_store_scan_apps(lz_local_app_t *out, int cap);
        extern int  lz_store_file_roots(const char **out, int cap);
        sim_reset_dir("lzdata_appfsroot");
        sim_mkdirs("lzdata_appfsroot/local");
        sim_seed_appfs("lzdata_appfsroot");

        lz_store_init(NULL);
        lz_store_set_appfs_root("lzdata_appfsroot/appfs");
        lz_local_app_t apps[4];
        int an = lz_store_scan_apps(apps, 4);
        CHECK(an == 1 && strcmp(apps[0].id, "calc.local") == 0,
              "appfs-only scanner loads local app");
        const char *roots[3];
        int rn = lz_store_file_roots(roots, 3);
        CHECK(rn == 1 && strcmp(roots[0], "lzdata_appfsroot/appfs") == 0,
              "appfs-only Files root is exposed");

        lz_store_init("lzdata_appfsroot/local");
        rn = lz_store_file_roots(roots, 3);
        CHECK(rn == 2, "Files exposes SD/local and appfs roots");
        lz_store_set_appfs_root(NULL);
        lz_store_init(NULL);
        sim_reset_dir("lzdata_appfsroot");
    }

    /* 12. MeshCore companion v0 line surface: snapshot helpers emit stable
     *     markers and send through the service boundary. */
    {
        lz_svc_init(NULL, false);
        uint8_t pub[32] = {0};
        pub[0] = 0x42;
        char pub_addr[65];
        for(int i = 0; i < 32; i++) sprintf(pub_addr + i * 2, "%02x", pub[i]);
        lz_core_on_mc_node(pub, "CompanionPeer", 1, -7.5f);
        lz_core_on_mc_channel_text("CompanionPeer", "public hello", -7.5f);
        lz_core_on_mc_dm(pub, "CompanionPeer", "dm hello", -7.5f);
        char hello[240], status[280], nodes[700], threads[520];
        lz_svc_mc_companion_hello(hello, sizeof hello);
        lz_svc_mc_companion_status(status, sizeof status);
        lz_svc_mc_companion_nodes(nodes, sizeof nodes);
        lz_svc_mc_companion_threads(threads, sizeof threads);
        CHECK(strstr(hello, "mccomp: hello v0") != NULL,
              "MeshCore companion v0 hello reports protocol");
        CHECK(strstr(status, "nodes=1") != NULL && strstr(status, "threads=2") != NULL,
              "MeshCore companion v0 status counts snapshots");
        CHECK(strstr(nodes, "CompanionPeer") != NULL &&
              strstr(nodes, pub_addr) != NULL &&
              strstr(nodes, "short_id=MC-42000000") != NULL &&
              strstr(nodes, "public_key=present") != NULL &&
              strstr(nodes, "dm=yes") != NULL,
              "MeshCore companion v0 node snapshot lists exact-address peer");
        CHECK(strstr(threads, "public hello") != NULL && strstr(threads, "dm hello") != NULL,
              "MeshCore companion v0 thread snapshot lists public and DM threads");
        CHECK(lz_svc_mc_companion_send_public("public from companion"),
              "MeshCore companion v0 public send uses service boundary");
        sim_backend_mc_clear_last_dm();
        CHECK(lz_svc_mc_companion_send_dm("CompanionPeer", "dm from companion"),
              "MeshCore companion v0 DM send uses service boundary");
        uint8_t last_key[32]; char last_name[28], last_text[160];
        CHECK(sim_backend_mc_last_dm(last_key, last_name, sizeof last_name,
                                     last_text, sizeof last_text) &&
              memcmp(last_key, pub, 32) == 0 &&
              strcmp(last_name, "CompanionPeer") == 0 &&
              strcmp(last_text, "dm from companion") == 0,
              "MeshCore companion v0 DM sends exact public key");
        char mc0[900], proto[120];
        bool mc0_exit = false;
        lz_svc_mc_companion_handle_line("MC0 1 HELLO proto=0 app=selftest", mc0, sizeof mc0, &mc0_exit);
        CHECK(strstr(mc0, "MC0 1 OK proto=0") != NULL && strstr(mc0, "caps=") != NULL,
              "MeshCore MC0 HELLO reports protocol capabilities");
        lz_svc_mc_companion_handle_line("MC0 2 STATUS", mc0, sizeof mc0, &mc0_exit);
        CHECK(strstr(mc0, "MC0 2 OK") != NULL && strstr(mc0, "nodes=1") != NULL &&
              strstr(mc0, "threads=2") != NULL,
              "MeshCore MC0 STATUS counts snapshots");
        lz_svc_mc_companion_handle_line("MC0 3 NODES since=0 limit=5", mc0, sizeof mc0, &mc0_exit);
        CHECK(strstr(mc0, "MC0 3 BEGIN type=nodes") != NULL &&
              strstr(mc0, pub_addr) != NULL &&
              strstr(mc0, "short_id=MC-42000000") != NULL &&
              strstr(mc0, "public_key=present") != NULL &&
              strstr(mc0, "dm=ready") != NULL &&
              strstr(mc0, "name=CompanionPeer") != NULL &&
              strstr(mc0, "MC0 3 END type=nodes") != NULL,
              "MeshCore MC0 NODES snapshot lists full-address peer");
        lz_svc_mc_companion_handle_line("MC0 4 THREADS", mc0, sizeof mc0, &mc0_exit);
        CHECK(strstr(mc0, "MC0 4 BEGIN type=threads") != NULL &&
              strstr(mc0, "text=CompanionPeer%3A%20public%20hello") != NULL &&
              strstr(mc0, "text=dm%20hello") != NULL,
              "MeshCore MC0 THREADS snapshot lists encoded thread text");
        lz_svc_mc_companion_handle_line("MC0 5 SEND_PUBLIC text=mc0%20public", mc0, sizeof mc0, &mc0_exit);
        CHECK(strstr(mc0, "MC0 5 OK accepted=1") != NULL,
              "MeshCore MC0 SEND_PUBLIC uses service boundary");
        sim_backend_mc_clear_last_dm();
        char mc0_cmd[180];
        snprintf(mc0_cmd, sizeof mc0_cmd, "MC0 6 SEND_DM to_addr=%s text=mc0%%20dm", pub_addr);
        lz_svc_mc_companion_handle_line(mc0_cmd, mc0, sizeof mc0, &mc0_exit);
        CHECK(strstr(mc0, "MC0 6 OK accepted=1") != NULL &&
              strstr(mc0, pub_addr) != NULL,
              "MeshCore MC0 SEND_DM accepts exact public-key address");
        CHECK(sim_backend_mc_last_dm(last_key, last_name, sizeof last_name,
                                     last_text, sizeof last_text) &&
              memcmp(last_key, pub, 32) == 0 &&
              strcmp(last_text, "mc0 dm") == 0,
              "MeshCore MC0 SEND_DM routes by exact public key");
        lz_svc_mc_companion_handle_line("MC0 7 SEND_DM to_name=companionpeer text=mc0%20dm%20name", mc0, sizeof mc0, &mc0_exit);
        CHECK(strstr(mc0, "MC0 7 OK accepted=1") != NULL,
              "MeshCore MC0 SEND_DM keeps unambiguous name fallback");
        lz_seed_node(0x00009999u, "MC-nokey", LZ_NET_MC, "NoKey", "NOK",
                     "Chat", -5.0f, 90, "MeshCore", "1.0 km", 10, false);
        lz_svc_mc_companion_handle_line("MC0 8 SEND_DM to_name=NoKey text=x", mc0, sizeof mc0, &mc0_exit);
        CHECK(strstr(mc0, "ERR code=no_key") != NULL,
              "MeshCore MC0 SEND_DM rejects chat nodes without a key");
        uint8_t other[32] = {0};
        other[0] = 0x44; other[3] = 0x01;
        lz_core_on_mc_node(other, "OtherPeer", 1, -7.5f);
        lz_svc_mc_companion_handle_line("MC0 9 SEND_DM to_addr=4200000000000000000000000000000000000000000000000000000000000000 to_name=OtherPeer text=x",
                                        mc0, sizeof mc0, &mc0_exit);
        CHECK(strstr(mc0, "ERR code=target_mismatch") != NULL,
              "MeshCore MC0 SEND_DM rejects mismatched address/name targets");
        uint8_t dup1[32] = {0}, dup2[32] = {0};
        dup1[0] = 0x50; dup1[3] = 0x01;
        dup2[0] = 0x51; dup2[3] = 0x02;
        lz_core_on_mc_node(dup1, "DupePeer", 1, -7.5f);
        lz_core_on_mc_node(dup2, "DupePeer", 1, -7.5f);
        lz_svc_mc_companion_handle_line("MC0 10 SEND_DM to_name=dupepeer text=x", mc0, sizeof mc0, &mc0_exit);
        CHECK(strstr(mc0, "ERR code=ambiguous_name") != NULL,
              "MeshCore MC0 SEND_DM rejects duplicate display names");
        lz_svc_mc_companion_handle_line("MC0 11 EXIT", mc0, sizeof mc0, &mc0_exit);
        CHECK(mc0_exit && strstr(mc0, "state=detached") != NULL,
              "MeshCore MC0 EXIT returns to console");
        lz_svc_mc_companion_selftest(proto, sizeof proto);
        CHECK(strstr(proto, "PASS") != NULL, "MeshCore MC0 protocol selftest passes");
    }

    /* 13. MeshCore Public-channel GRP_TXT: decode a known reference vector,
    /* 14. MeshCore Public-channel GRP_TXT: decode a known reference vector,
     *    reject a wrong key (MAC), and round-trip an encode. Vector generated
     *    against the documented scheme (AES-128-ECB + HMAC-SHA256 trunc-2). */
    {
        /* on-air frame: header(GRP_TXT|FLOOD)=0x15, path_len=0, then
         * [chash 0x11][mac 99 04][ciphertext 32B] -> "Alice: hello mesh" */
        static const uint8_t gframe[] = {
            0x15, 0x00,
            0x11, 0x99, 0x04,
            0xb0,0xf3,0xee,0x44,0xb9,0x2e,0x56,0xc5,0x41,0x80,0x6d,0x64,0x1b,0x02,0x4a,0x58,
            0x5b,0xe4,0xe2,0x76,0x1c,0x62,0xb2,0x46,0x4f,0xd9,0xdd,0xf4,0x22,0xcd,0x3f,0xd3 };
        mc_pkt_t gp;
        CHECK(mc_parse(gframe, (int)sizeof gframe, &gp), "MeshCore frame parses");
        CHECK(gp.payload_type == MC_PAYLOAD_GRP_TXT, "MeshCore payload type GRP_TXT");
        CHECK(mc_group_channel_hash(&gp) == MC_PUBLIC_CHANNEL_HASH, "MeshCore Public channel hash 0x11");
        mc_group_msg_t gm;
        CHECK(mc_group_decode(&gp, MC_PUBLIC_SECRET, &gm), "MeshCore GRP_TXT decodes + MAC ok");
        CHECK(gm.timestamp == 0x6843B2A0u, "MeshCore GRP_TXT timestamp");
        CHECK(strcmp(gm.sender, "Alice") == 0, "MeshCore GRP_TXT sender parsed");
        CHECK(strcmp(gm.text, "hello mesh") == 0, "MeshCore GRP_TXT text parsed");
        uint8_t wrong[16]; memset(wrong, 0x11, sizeof wrong);
        mc_group_msg_t gbad;
        CHECK(!mc_group_decode(&gp, wrong, &gbad), "MeshCore GRP_TXT wrong key rejected (MAC)");
        /* encode -> parse -> decode round-trip */
        uint8_t out[128];
        int ol = mc_group_encode(out, sizeof out, MC_PUBLIC_SECRET, 0x12345678u, "Bob", "hi there");
        CHECK(ol > 5, "MeshCore GRP_TXT encodes");
        mc_pkt_t rp; mc_group_msg_t rm;
        CHECK(mc_parse(out, ol, &rp) && mc_group_decode(&rp, MC_PUBLIC_SECRET, &rm),
              "MeshCore GRP_TXT round-trip decodes");
        CHECK(rm.timestamp == 0x12345678u && strcmp(rm.sender, "Bob") == 0 &&
              strcmp(rm.text, "hi there") == 0, "MeshCore GRP_TXT round-trip fields");
    }

    /* 15. MeshCore DM (TXT_MSG): ECDH derive (vs orlp/standard reference) then a
     *     full encode->parse->decode round-trip + ACK match + MAC tamper check. */
    {
        uint8_t seedA[32], pubA[32], pubB[32], ref[32];
        #define HX(s,b) do{ const char*h=(s); for(int i=0;i<32;i++){unsigned v;sscanf(h+i*2,"%2x",&v);(b)[i]=(uint8_t)v;} }while(0)
        HX("18469d6140447f77de13cd8d761e605431f52269fbff43b0925752ed9e674543", seedA);
        HX("269a00cc46663e159ec2b90132e584743ebd3324910199e72ac09fff54c4a64d", pubA);
        HX("1013c0e1ff475a1cd1afaf7e2c2d4a396acec5b650dbe24696a21711d8405ab0", pubB);
        HX("75335905e5b18ec51967d9a1db1c76187cfc0a537a5b9ac5dfc1eacc6db37653", ref);
        #undef HX
        uint8_t sh[32];
        mc_ed25519_dh(sh, pubB, seedA);
        CHECK(memcmp(sh, ref, 32) == 0, "MeshCore DM ECDH matches MeshCore/standard reference");

        uint8_t ack[4], frame[160];
        int fl = mc_dm_encode(frame, sizeof frame, sh, pubB[0], pubA[0], 0x6843B2A0u,
                              MC_TXT_TYPE_PLAIN, 0, "hi cedar from limit", pubA, ack);
        CHECK(fl > 6, "MeshCore DM encodes");
        mc_pkt_t dp; mc_dm_msg_t dm;
        CHECK(mc_parse(frame, fl, &dp) && dp.payload_type == MC_PAYLOAD_TXT_MSG, "MeshCore DM parses (TXT_MSG)");
        CHECK(mc_dm_decode(&dp, sh, &dm), "MeshCore DM decodes + MAC ok");
        CHECK(dm.timestamp == 0x6843B2A0u && dm.txt_type == 0 && dm.attempt == 0 &&
              dm.src_hash == pubA[0] && strcmp(dm.text, "hi cedar from limit") == 0,
              "MeshCore DM fields (no name prefix)");
        uint8_t ack_r[4];                        /* recipient recomputes from decoded fields + sender pub */
        mc_dm_ack4(ack_r, dm.timestamp, (uint8_t)((dm.txt_type<<2)|dm.attempt), dm.text, pubA);
        CHECK(memcmp(ack, ack_r, 4) == 0, "MeshCore DM ACK checksum matches (sender vs recipient)");
        frame[7] ^= 0x40;                        /* corrupt a ciphertext byte */
        mc_pkt_t dp2; mc_dm_msg_t dm2;
        CHECK(mc_parse(frame, fl, &dp2) && !mc_dm_decode(&dp2, sh, &dm2), "MeshCore DM MAC rejects tampering");
    }

    #undef CHECK
    printf(fails ? "\nCODEC SELFTEST: %d FAILURE(S)\n" : "\nCODEC SELFTEST: all passed\n", fails);
    return fails;
}

int main(int argc, char **argv)
{
    if(argc >= 2 && strcmp(argv[1], "--selftest") == 0) return codec_selftest();
    if(argc >= 2 && strcmp(argv[1], "--simtest") == 0)  return sim_scenario_run();

    bool headless = argc >= 3 && strcmp(argv[1], "--shots") == 0;
    g_headless = headless;

    SDL_Init(headless ? 0 : SDL_INIT_VIDEO);
    lv_init();

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, draw_buf_mem, NULL, LZ_W * 60);
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LZ_W;
    disp_drv.ver_res = LZ_H;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = mouse_read_cb;
    lv_indev_drv_register(&indev_drv);

    if(!headless) {
        win = SDL_CreateWindow("LimitlezzOS — T-Deck simulator",
                               SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               LZ_W * SCALE, LZ_H * SCALE, 0);
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565,
                                SDL_TEXTUREACCESS_STREAMING, LZ_W, LZ_H);
    }

    /* persistent store on disk so history survives restarts. Headless uses a
     * dedicated dir wiped first, so seeded history persists within the run
     * (the conversation shows full threads) yet shots stay deterministic. */
    lz_svc_set_dirty_cb(on_dirty);
    const char *datadir = "lzdata";
    if(headless) {
        datadir = "lzdata_shots";
        sim_reset_dir(datadir);
        sim_seed_local_app(datadir);
        sim_seed_appfs(datadir);
    }
    {
        char appfs[160];
        snprintf(appfs, sizeof appfs, "%s/appfs", datadir);
        lz_svc_set_appfs_root(appfs);
    }
    lz_svc_init(datadir, true);
    lz_svc_set_time(1781274180);   /* sim: pretend NTP synced so the clock shows */
    lz_wifi_init();
    lz_ui_init(lv_scr_act());

    if(headless) {
        return shots(argv[2]);
    }

    bool quit = false;
    while(!quit) {
        SDL_Event e;
        while(SDL_PollEvent(&e)) {
            if(e.type == SDL_QUIT) quit = true;
            else if(e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                /* Ctrl+key: simulated-radio injection controls (don't reach the
                 * UI composer). Ctrl held -> SDL_TEXTINPUT won't fire for these. */
                if(e.key.keysym.mod & KMOD_CTRL) {
                    switch(k) {
                        case SDLK_d: sim_inject_mc_dm_from_limitlezz("hey, you on MeshCore now?"); break;
                        case SDLK_p: sim_inject_mc_public("Limitlezz", "public net check, all good here"); break;
                        case SDLK_n: sim_inject_mc_advert(); break;
                        case SDLK_m: sim_inject_mt_channel_text("anyone on the ridge?"); break;
                        case SDLK_t: sim_inject_mt_dm_to_us("Ridge Hiker", "you copy? need a relay check"); break;
                        case SDLK_i: { static int r; switch(r++ % 3) {
                                          case 0: sim_inject_mt_nodeinfo(); break;
                                          case 1: sim_inject_mt_position(); break;
                                          case 2: sim_inject_mt_telemetry(); break; } break; }
                        case SDLK_b: sim_inject_burst(); break;
                        case SDLK_a: sim_set_auto_traffic(!sim_get_auto_traffic());
                                     printf("[sim] ambient traffic %s\n",
                                            sim_get_auto_traffic() ? "ON" : "OFF"); break;
                        default: break;
                    }
                    lz_note_activity();   /* injected radio activity wakes the screen */
                    lz_rebuild();
                    break;                /* consume; don't fall through to UI keys */
                }
                if(k == SDLK_UP)            lz_ui_key(LZ_K_UP, 0);
                else if(k == SDLK_DOWN)     lz_ui_key(LZ_K_DOWN, 0);
                else if(k == SDLK_LEFT)     lz_ui_key(LZ_K_LEFT, 0);
                else if(k == SDLK_RIGHT)    lz_ui_key(LZ_K_RIGHT, 0);
                else if(k == SDLK_PAGEUP)   lz_ui_key(LZ_K_ENTER, 0);  /* trackball press */
                else if(k == SDLK_RETURN)   lz_ui_key(LZ_K_ENTER, 0);
                else if(k == SDLK_ESCAPE)    lz_ui_key(LZ_K_BACK, 0);   /* nav back */
                else if(k == SDLK_BACKSPACE) lz_ui_key(LZ_K_DEL, 0);    /* delete char / back */
            }
            else if(e.type == SDL_TEXTINPUT) {
                for(const char *p = e.text.text; *p; p++)
                    if(*p > 0) lz_ui_key(LZ_K_CHAR, *p);
            }
            else if(e.type == SDL_MOUSEMOTION) {
                m_x = e.motion.x / SCALE;
                m_y = e.motion.y / SCALE;
            }
            else if(e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                m_x = e.button.x / SCALE;
                m_y = e.button.y / SCALE;
                m_down = true;
                lz_note_activity();         /* touch counts as activity */
            }
            else if(e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
                m_x = e.button.x / SCALE;
                m_y = e.button.y / SCALE;
                m_down = false;
            }
        }
        services_tick();
        lv_timer_handler();
        SDL_Delay(5);
    }
    SDL_Quit();
    return 0;
}
