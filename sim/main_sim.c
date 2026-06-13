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
 * `--shots <dir>` renders every screen headless and dumps BMPs for visual
 * verification, then exits.
 */
#include "lvgl.h"
#include "ui/ui.h"
#include "services/mesh.h"
#include "services/mtproto.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    if(!f) return;
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
    for(int i = 0; i < 7; i++) lz_ui_key(LZ_K_DOWN, 0);   /* down to Brightness */
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
    lz_ui_key(LZ_K_DOWN, 0);           /* focus first network (Basecamp-2G) */
    lz_ui_key(LZ_K_ENTER, 0);          /* secured -> password entry */
    for(const char *p = "trailpass"; *p; p++) lz_ui_key(LZ_K_CHAR, *p);
    pump(40);
    snprintf(path, sizeof path, "%s/28-wifi-password.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);
    lz_ui_key(LZ_K_ENTER, 0);          /* connect */
    pump(1500);                        /* connecting -> connected */
    snprintf(path, sizeof path, "%s/29-wifi-connected.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);

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

    #undef CHECK
    printf(fails ? "\nCODEC SELFTEST: %d FAILURE(S)\n" : "\nCODEC SELFTEST: all passed\n", fails);
    return fails;
}

int main(int argc, char **argv)
{
    if(argc >= 2 && strcmp(argv[1], "--selftest") == 0) return codec_selftest();

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
        system("rm -rf lzdata_shots && mkdir -p lzdata_shots");
    }
    lz_svc_init(datadir, true);
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
                if(k == SDLK_UP)            lz_ui_key(LZ_K_UP, 0);
                else if(k == SDLK_DOWN)     lz_ui_key(LZ_K_DOWN, 0);
                else if(k == SDLK_LEFT)     lz_ui_key(LZ_K_LEFT, 0);
                else if(k == SDLK_RIGHT)    lz_ui_key(LZ_K_RIGHT, 0);
                else if(k == SDLK_PAGEUP)   lz_ui_key(LZ_K_ENTER, 0);  /* trackball press */
                else if(k == SDLK_RETURN)   lz_ui_key(LZ_K_ENTER, 0);
                else if(k == SDLK_ESCAPE || k == SDLK_BACKSPACE) lz_ui_key(LZ_K_BACK, 0);
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
