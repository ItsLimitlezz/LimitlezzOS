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

static void pump(int ms)
{
    for(int t = 0; t < ms; t += 5) {
        lv_timer_handler();
        SDL_Delay(5);
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
    for(unsigned i = 0; i < sizeof(SHOTS) / sizeof(SHOTS[0]); i++) {
        S.view = SHOTS[i].v;
        S.focus = 0;
        if(SHOTS[i].v == LZ_V_CONVO) { S.convo = &LZ_THREADS[0]; S.sent_count = 0; S.draft[0] = 0; }
        if(SHOTS[i].v == LZ_V_CONTACT) S.contact_sel = &LZ_NODES[2];
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

    /* filter to MeshCore via key '3' */
    lz_ui_key(LZ_K_CHAR, '3');
    pump(60);
    snprintf(path, sizeof path, "%s/16-messages-filter-mc.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);

    /* type a draft and send it */
    S.view = LZ_V_CONVO; S.convo = &LZ_THREADS[0]; S.sent_count = 0; S.draft[0] = 0;
    S.msg_filter = LZ_FILT_ALL; lz_rebuild();
    for(const char *p = "on my way up now"; *p; p++) lz_ui_key(LZ_K_CHAR, *p);
    pump(60);
    snprintf(path, sizeof path, "%s/17-convo-draft.bmp", dir);
    write_bmp(path); printf("wrote %s\n", path);
    lz_ui_key(LZ_K_ENTER, 0);
    pump(60);
    snprintf(path, sizeof path, "%s/18-convo-sent.bmp", dir);
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
}

int main(int argc, char **argv)
{
    bool headless = argc >= 3 && strcmp(argv[1], "--shots") == 0;

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
            }
            else if(e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
                m_x = e.button.x / SCALE;
                m_y = e.button.y / SCALE;
                m_down = false;
            }
        }
        lv_timer_handler();
        SDL_Delay(5);
    }
    SDL_Quit();
    return 0;
}
