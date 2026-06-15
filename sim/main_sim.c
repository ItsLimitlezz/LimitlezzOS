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
#include "services/mesh.h"
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

uint32_t lz_tick_ms(void) { return SDL_GetTicks(); }

static bool g_headless;
/* incoming radio data → rebuild the current screen (live and headless alike);
 * lz_rebuild pins an open conversation to the newest message */
static void on_dirty(void) { lz_rebuild(); }

/* sim_reset_dir() lives in sim_fs.c (recursive, Windows-safe) — included via sim_fs.h */

static int sim_mkdir_one(const char *path)
{
#ifdef _WIN32
    return (_mkdir(path) == 0 || errno == EEXIST) ? 0 : -1;
#else
    return (mkdir(path, 0777) == 0 || errno == EEXIST) ? 0 : -1;
#endif
}

static void sim_mkdirs(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    for(char *p = tmp; *p; p++) {
        if(*p != '/' && *p != '\\') continue;
        char sep = *p;
        *p = 0;
        if(tmp[0] && !(strlen(tmp) == 2 && tmp[1] == ':'))
            sim_mkdir_one(tmp);
        *p = sep;
    }
    if(tmp[0] && sim_mkdir_one(tmp) != 0) {
        fprintf(stderr, "error: could not create %s\n", path);
        exit(2);
    }
}

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

static void shots(const char *dir)
{
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
        if(SHOTS[i].v == LZ_V_CONVO) { S.convo = lz_svc_thread_at(0);   /* newest = Ava */
                                       lz_svc_open_thread(S.convo); S.draft[0] = 0; }
        if(SHOTS[i].v == LZ_V_CONTACT) S.contact_sel = ava;
        lz_rebuild();
        pump(60);
        snprintf(path, sizeof path, "%s/%s.bmp", dir, SHOTS[i].name);
        write_bmp(path);
        printf("wrote %s\n", path);
    }

    /* behavior scenarios, driven through the real key path */

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
    S.view = LZ_V_APPSTORE; S.focus = 0; lz_rebuild();
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
    for(int i = 0; i < 8 && S.focus != 6; i++) lz_ui_key(LZ_K_DOWN, 0);   /* down to Brightness */
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

    /* 6. TELEMETRY device metrics: battery varint, voltage float, uptime varint */
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

    /* 7. TELEMETRY env metrics: a hostile NaN float must decode without OOB and
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

    /* 8. store delivery-metadata round-trip: updating a DM that is NOT the first
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

    /* 9. Wi-Fi credential store round-trip. T-Deck uses an NVS backend under the
     * same API; native keeps the file path for simulator repeatability. */
    {
        extern void lz_store_init(const char *datadir);
        extern void lz_store_save_wifi(const char *ssid, const char *pass, int autoconnect);
        extern bool lz_store_load_wifi(char *ssid, int sn, char *pass, int pn, int *autoconnect);
        remove("./wifi.cfg");
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
        remove("./wifi.cfg");
        lz_store_init(NULL);
    }

    /* 10. MeshCore Public-channel GRP_TXT: decode a known reference vector,
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

    /* 10. MeshCore DM (TXT_MSG): ECDH derive (vs orlp/standard reference) then a
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
    }
    lz_svc_init(datadir, true);
    lz_svc_set_time(1781274180);   /* sim: pretend NTP synced so the clock shows */
    lz_wifi_init();
    lz_ui_init(lv_scr_act());

    if(headless) {
        shots(argv[2]);
        return 0;
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
