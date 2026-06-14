/**
 * LimitlezzOS — LilyGO T-Deck hardware target (ESP32-S3).
 *
 * Bring-up (spec phase 1.0/1.4 — UI portion):
 *   - board power rail (GPIO 10 must go HIGH before peripherals respond)
 *   - ST7789 320x240 over SPI via LovyanGFX (config below — the driver
 *     Meshtastic uses on the T-Deck; manages the shared SPI bus cleanly)
 *   - I2C keyboard (ESP32-C3 slave @ 0x55) — returns one ASCII char per poll
 *   - trackball: 4 direction pins pulse on roll; click on GPIO 0
 *   - touch: GT911 capacitive @ 0x5D (alt 0x14), INT GPIO 16
 *
 * Radio stacks, Lua VM, and the rest of the backend land behind the
 * mock data layer in src/ui/data.c (spec phases 1.3+).
 */
#ifdef LZ_TARGET_TDECK

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "lvgl.h"
#include "ui/ui.h"
#include "services/mesh.h"
#include "services/wifi.h"

/* All pins verified against LilyGO T-Deck (Xinyuan-LilyGO/T-Deck utilities.h)
 * and meshtastic/firmware variants/esp32s3/t-deck/variant.h. */
#define BOARD_POWERON      10    /* master peripheral power rail — HIGH first  */
#define BOARD_I2C_SDA      18
#define BOARD_I2C_SCL      8
#define KEYBOARD_I2C_ADDR  0x55

/* one SPI bus shared by TFT + SD + LoRa */
#define BOARD_SPI_SCK      40
#define BOARD_SPI_MOSI     41
#define BOARD_SPI_MISO     38
#define BOARD_TFT_CS       12
#define SDCARD_CS          39
#define RADIO_CS           9

#define BOARD_BL_PIN       42    /* display backlight (LEDC PWM) */
#define BL_LEDC_CH         7
#define BL_LEDC_FREQ       44100
#define BL_LEDC_BITS       8

#define TRACKBALL_UP       3
#define TRACKBALL_DOWN     15
#define TRACKBALL_LEFT     1
#define TRACKBALL_RIGHT    2
#define TRACKBALL_CLICK    0     /* shared with BOOT strapping pin */
#define TOUCH_INT          16
#define KB_BL_PIN          46    /* keyboard backlight enable (Meshtastic KB_BL) */
#define KB_BL_TIMEOUT_MS   8000  /* Auto: stay lit this long after input */
/* lz_backend_ok / lz_backend_begin_state declared in services/mesh.h */

/* GT911 reports panel-native portrait coordinates (240x320); the display runs
 * landscape (rotation 1). The transform is runtime-adjustable so it can be
 * calibrated live over serial (`touch ...`) without reflashing. */
static int  g_touch_swap = 1;            /* swap X/Y (portrait->landscape) */
static int  g_touch_invx = 0;
static int  g_touch_invy = 1;
static bool g_touch_debug = false;       /* log raw + mapped coords per touch */
static volatile int g_touch_raw_x, g_touch_raw_y, g_touch_map_x, g_touch_map_y;

extern "C" void lz_touch_set_transform(int swap, int invx, int invy)
{
    if(swap >= 0) g_touch_swap = swap ? 1 : 0;
    if(invx >= 0) g_touch_invx = invx ? 1 : 0;
    if(invy >= 0) g_touch_invy = invy ? 1 : 0;
}
extern "C" void lz_touch_set_debug(bool on) { g_touch_debug = on; }
extern "C" int  lz_touch_info(char *buf, int n)
{
    return snprintf(buf, n, "swap=%d invx=%d invy=%d  last raw=(%d,%d) -> (%d,%d)  debug=%s",
                    g_touch_swap, g_touch_invx, g_touch_invy,
                    g_touch_raw_x, g_touch_raw_y, g_touch_map_x, g_touch_map_y,
                    g_touch_debug ? "on" : "off");
}

/* touch-calibration capture: 3 raw points (TL, TR, BL) -> swap/invert transform.
 * Tap edges are detected in touch_read_cb and processed in loop() (g_cal_pending),
 * never inside the LVGL input callback. */
extern "C" void lz_store_save_touch(int swap, int invx, int invy);
extern "C" bool lz_store_load_touch(int *swap, int *invx, int *invy);
static volatile bool g_cal_pending;
static volatile int  g_cal_px, g_cal_py;
static bool g_touch_down;
static bool g_ignore_touch;        /* swallow presses until the finger lifts (post-cal) */
static int  g_cal_rx[3], g_cal_ry[3];

static void touchcal_register(int rx, int ry)
{
    int step = (S.cal_step >= 0 && S.cal_step <= 2) ? S.cal_step : 0;
    g_cal_rx[step] = rx; g_cal_ry[step] = ry;
    if(step < 2) { S.cal_step = step + 1; lz_rebuild(); return; }

    /* derive the transform from the three taps */
    int dx_tx = g_cal_rx[1] - g_cal_rx[0], dx_ty = g_cal_ry[1] - g_cal_ry[0];  /* +screenX */
    int swap = (abs(dx_ty) > abs(dx_tx)) ? 1 : 0;
    int sxd  = swap ? dx_ty : dx_tx;
    int invx = (sxd < 0) ? 1 : 0;
    int dy_tx = g_cal_rx[2] - g_cal_rx[0], dy_ty = g_cal_ry[2] - g_cal_ry[0];  /* +screenY */
    int syd  = swap ? dy_tx : dy_ty;
    int invy = (syd < 0) ? 1 : 0;

    lz_touch_set_transform(swap, invx, invy);
    lz_store_save_touch(swap, invx, invy);
    Serial.printf("[ok] touch calibrated: swap=%d invx=%d invy=%d\n", swap, invx, invy);
    S.cal_step = 0;
    g_ignore_touch = true;          /* don't let the finishing tap click the Settings screen */
    lz_back();
    lz_rebuild();
}

/* ---- LovyanGFX config for the LilyGO T-Deck ST7789V (landscape 320x240) ----
 * ST7789 on the shared SPI bus (SCK40/MOSI41/MISO38), CS12/DC11, invert on,
 * bus_shared so it coexists with SD + LoRa. Matches Meshtastic's T-Deck LGFX. */
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789 _panel;
    lgfx::Bus_SPI      _bus;
public:
    LGFX() {
        { auto c = _bus.config();
          c.spi_host    = SPI2_HOST;     /* FSPI */
          c.spi_mode    = 0;
          c.freq_write  = 40000000;
          c.freq_read   = 16000000;
          c.spi_3wire   = false;
          c.use_lock    = true;
          c.dma_channel = SPI_DMA_CH_AUTO;
          c.pin_sclk    = 40;
          c.pin_mosi    = 41;
          c.pin_miso    = 38;
          c.pin_dc      = 11;
          _bus.config(c);
          _panel.setBus(&_bus);
        }
        { auto c = _panel.config();
          c.pin_cs          = 12;
          c.pin_rst         = -1;
          c.pin_busy        = -1;
          c.panel_width     = 240;
          c.panel_height    = 320;
          c.offset_x        = 0;
          c.offset_y        = 0;
          c.offset_rotation = 0;
          c.readable        = false;
          c.invert          = true;      /* T-Deck panel needs INVON */
          c.rgb_order       = false;
          c.dlen_16bit      = false;
          c.bus_shared      = true;      /* shares the bus with SD + LoRa */
          _panel.config(c);
        }
        setPanel(&_panel);
    }
};

static LGFX lcd;
static lv_color_t draw_buf_mem[LZ_W * 40];   /* internal-RAM fallback */

extern "C" uint32_t lz_tick_ms(void) { return millis(); }

void lz_cli_begin(void);                 /* serial console (serial_cli.cpp) */
void lz_cli_poll(void);
extern "C" void lz_mtc_poll(void);       /* Meshtastic companion bridge (mt_companion.cpp) */
extern "C" void lz_mtc_ble_begin(void);
extern "C" void lz_mtc_ble_poll(void);

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    lcd.startWrite();
    lcd.setAddrWindow(area->x1, area->y1, w, h);
    lcd.writePixels((lgfx::rgb565_t *)px, w * h);
    lcd.endWrite();
    lv_disp_flush_ready(drv);
}

/* trackball edges counted in ISRs, consumed in loop() */
static volatile int tb_up, tb_down, tb_left, tb_right;
static void IRAM_ATTR isr_up(void)    { tb_up++; }
static void IRAM_ATTR isr_down(void)  { tb_down++; }
static void IRAM_ATTR isr_left(void)  { tb_left++; }
static void IRAM_ATTR isr_right(void) { tb_right++; }

/* ---- GT911 touch (minimal poll-mode driver) ---- */
static uint8_t gt911_addr;   /* 0 = not found */

static bool gt911_read(uint16_t reg, uint8_t *buf, int len)
{
    Wire.beginTransmission(gt911_addr);
    Wire.write(reg >> 8);
    Wire.write(reg & 0xFF);
    if(Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom((int)gt911_addr, len);
    for(int i = 0; i < len; i++) {
        if(!Wire.available()) return false;
        buf[i] = Wire.read();
    }
    return true;
}

static void gt911_write8(uint16_t reg, uint8_t v)
{
    Wire.beginTransmission(gt911_addr);
    Wire.write(reg >> 8);
    Wire.write(reg & 0xFF);
    Wire.write(v);
    Wire.endTransmission();
}

static void gt911_init(void)
{
    static const uint8_t addrs[2] = { 0x5D, 0x14 };
    for(int i = 0; i < 2; i++) {
        Wire.beginTransmission(addrs[i]);
        if(Wire.endTransmission() == 0) {
            gt911_addr = addrs[i];
            break;
        }
    }
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    static int16_t last_x, last_y;
    data->point.x = last_x;
    data->point.y = last_y;
    data->state = LV_INDEV_STATE_RELEASED;
    if(!gt911_addr) return;

    uint8_t status;
    if(!gt911_read(0x814E, &status, 1)) return;
    if(!(status & 0x80)) return;                 /* no fresh data */

    int points = status & 0x0F;
    if(points > 0) {
        uint8_t p[4];
        if(gt911_read(0x8150, p, 4)) {           /* first point: x lo/hi, y lo/hi */
            int tx = p[0] | (p[1] << 8);
            int ty = p[2] | (p[3] << 8);
            int sx = g_touch_swap ? ty : tx;
            int sy = g_touch_swap ? tx : ty;
            if(g_touch_invx) sx = LZ_W - 1 - sx;
            if(g_touch_invy) sy = LZ_H - 1 - sy;
            if(sx < 0) sx = 0; if(sx >= LZ_W) sx = LZ_W - 1;
            if(sy < 0) sy = 0; if(sy >= LZ_H) sy = LZ_H - 1;
            g_touch_raw_x = tx; g_touch_raw_y = ty;
            g_touch_map_x = sx; g_touch_map_y = sy;
            if(g_touch_debug) {
                static uint32_t last_log;
                if(millis() - last_log > 150) { last_log = millis();
                    Serial.printf("[touch] raw=(%d,%d) -> screen=(%d,%d)\n", tx, ty, sx, sy); }
            }
            /* calibration: capture the raw coord once per touch-down */
            bool cal = (S.view == LZ_V_TOUCHCAL);
            if(cal && !g_touch_down && !g_cal_pending) {
                g_cal_px = tx; g_cal_py = ty; g_cal_pending = true;
            }
            g_touch_down = true;
            /* tap-to-wake: if the screen is asleep, swallow this ENTIRE gesture
             * (until the finger lifts), not just the first frame — otherwise the
             * next poll, now awake, delivers a real press/click that would open
             * the notification card or a chat row. Lift + tap again to act. */
            if(lz_is_dimmed()) g_ignore_touch = true;
            lz_note_activity();                  /* wake the screen + keep it awake */
            /* don't deliver calibration taps (or the lingering post-cal / wake
             * press) to the UI — they would otherwise click whatever's underneath */
            if(!cal && !g_ignore_touch) {
                last_x = sx;
                last_y = sy;
                data->point.x = sx;
                data->point.y = sy;
                data->state = LV_INDEV_STATE_PRESSED;
            }
        }
    } else {
        g_touch_down = false;                    /* finger lifted */
        g_ignore_touch = false;                  /* clear the post-cal swallow */
    }
    gt911_write8(0x814E, 0);                     /* ack: clear buffer status */
}

/* display backlight via LEDC PWM (Arduino-ESP32 core 2.x API) */
static void backlight_set(int pct)
{
    if(pct < 0) pct = 0; if(pct > 100) pct = 100;
    ledcWrite(BL_LEDC_CH, (pct * 255) / 100);
}

static bool kb_present;
static void kb_set_brightness(uint8_t duty);   /* defined below */

/* real system info for the System screen + status bar (no fake 87%) */
#define BATTERY_PIN     4        /* ADC1 GPIO4, 2:1 divider (Meshtastic T-Deck) */
#define BATTERY_MULT    2.11f
extern "C" uint8_t temprature_sens_read();   /* legacy fallback */
static void tdeck_sysinfo(lz_sysinfo_t *o)
{
    memset(o, 0, sizeof *o);
    /* battery via ADC; T-Deck has no charge-detect line, so report voltage */
    int mv = analogReadMilliVolts(BATTERY_PIN);
    float v = (mv / 1000.0f) * BATTERY_MULT;
    o->battery_v = v;
    /* Li-ion 3.3V(0%)..4.2V(100%) rough curve */
    int pct = (int)((v - 3.3f) / (4.2f - 3.3f) * 100.0f);
    if(pct < 0) pct = 0; if(pct > 100) pct = 100;
    o->battery_pct = (v > 2.5f) ? pct : -1;       /* <2.5V => no/over-USB, unknown */
    o->usb = (v < 2.5f) || (v > 4.3f);
    o->charging = false;
    o->cpu_mhz = getCpuFrequencyMhz();
    uint32_t heap_total = ESP.getHeapSize(), heap_free = ESP.getFreeHeap();
    o->ram_total_kb = heap_total / 1024;
    o->ram_used_kb  = (heap_total - heap_free) / 1024;
    o->flash_total_kb = ESP.getFlashChipSize() / 1024;
    o->flash_used_kb  = ESP.getSketchSize() / 1024;
    o->temp_c = (int)temperatureRead();           /* on-die temp (approx) */
    o->uptime_s = millis() / 1000;
}

void setup()
{
    Serial.begin(115200);
    /* native USB CDC: give the host a moment to enumerate so the early boot
     * lines aren't lost (don't block forever if running headless on battery) */
    for(int i = 0; i < 40 && !Serial; i++) delay(25);
    delay(200);
    Serial.println("\n=== LimitlezzOS boot ===");

    /* 1) master power rail FIRST — without this the display/SD/radio/keyboard
     *    are unpowered and the board looks bricked */
    pinMode(BOARD_POWERON, OUTPUT);
    digitalWrite(BOARD_POWERON, HIGH);
    delay(100);
    Serial.println("[ok] peripheral power (GPIO10) HIGH");

    /* 2) deselect ALL SPI devices before touching the bus (the #1 cause of a
     *    dead first flash is a floating/low CS corrupting another transfer) */
    pinMode(BOARD_TFT_CS, OUTPUT); digitalWrite(BOARD_TFT_CS, HIGH);
    pinMode(SDCARD_CS,    OUTPUT); digitalWrite(SDCARD_CS,    HIGH);
    pinMode(RADIO_CS,     OUTPUT); digitalWrite(RADIO_CS,     HIGH);
    pinMode(BOARD_SPI_MISO, INPUT_PULLUP);

    /* 3) one shared SPI bus (SCK, MISO, MOSI) for SD + LoRa; TFT_eSPI runs
     *    its own instance on the same physical pins */
    SPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI);
    Serial.println("[ok] shared SPI bus up (SCK40/MISO38/MOSI41)");

    /* 4) display (LovyanGFX owns the bus transactions; bus_shared=true) */
    lcd.init();
    lcd.setRotation(1);                  /* landscape 320x240, keyboard at bottom */
    lcd.fillScreen(0x0000);
    /* backlight: LEDC is the SOLE owner of GPIO42, set up AFTER the display so
     * nothing else can detach it (the dual-owner bug). Full on now — before
     * SD/radio init — so a later stall never looks like a dead screen. */
    ledcSetup(BL_LEDC_CH, BL_LEDC_FREQ, BL_LEDC_BITS);
    ledcAttachPin(BOARD_BL_PIN, BL_LEDC_CH);
    backlight_set(100);
    Serial.println("[ok] ST7789 display init + backlight on");

    lv_init();
    static lv_disp_draw_buf_t draw_buf;
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    /* Two full-frame buffers in PSRAM + full_refresh = each frame is composed
     * whole and pushed in one DMA transfer, so you never see a half-updated
     * frame (the panel has no TE/vsync line, so this is the practical fix for
     * the tearing). Falls back to a small internal-RAM buffer if PSRAM is out. */
    size_t fb_px = (size_t)LZ_W * LZ_H;
    lv_color_t *b1 = (lv_color_t *)heap_caps_malloc(fb_px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    lv_color_t *b2 = (lv_color_t *)heap_caps_malloc(fb_px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if(b1 && b2) {
        lv_disp_draw_buf_init(&draw_buf, b1, b2, fb_px);
        disp_drv.full_refresh = 1;
        Serial.println("[ok] LVGL double full-frame buffers in PSRAM (tear-free)");
    } else {
        if(b1) heap_caps_free(b1);
        if(b2) heap_caps_free(b2);
        lv_disp_draw_buf_init(&draw_buf, draw_buf_mem, NULL, LZ_W * 40);
        Serial.println("[--] PSRAM framebuffers unavailable; partial buffer");
    }
    disp_drv.hor_res = LZ_W;
    disp_drv.ver_res = LZ_H;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    /* 5) I2C peripherals: keyboard (0x55) + GT911 touch */
    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
    Wire.requestFrom(KEYBOARD_I2C_ADDR, 1);
    kb_present = Wire.available() > 0;
    Wire.read();
    Serial.printf("[%s] keyboard @0x55\n", kb_present ? "ok" : "--");

    pinMode(TOUCH_INT, INPUT);
    gt911_init();
    Serial.printf("[%s] GT911 touch%s\n", gt911_addr ? "ok" : "--",
                  gt911_addr ? (gt911_addr == 0x5D ? " @0x5D" : " @0x14") : " (not found)");
    static lv_indev_drv_t touch_drv;
    lv_indev_drv_init(&touch_drv);
    touch_drv.type = LV_INDEV_TYPE_POINTER;
    touch_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&touch_drv);

    /* 6) trackball on main-MCU GPIOs (active-low pulse trains) */
    pinMode(TRACKBALL_UP, INPUT_PULLUP);
    pinMode(TRACKBALL_DOWN, INPUT_PULLUP);
    pinMode(TRACKBALL_LEFT, INPUT_PULLUP);
    pinMode(TRACKBALL_RIGHT, INPUT_PULLUP);
    pinMode(TRACKBALL_CLICK, INPUT_PULLUP);
    attachInterrupt(TRACKBALL_UP, isr_up, FALLING);
    attachInterrupt(TRACKBALL_DOWN, isr_down, FALLING);
    attachInterrupt(TRACKBALL_LEFT, isr_left, FALLING);
    attachInterrupt(TRACKBALL_RIGHT, isr_right, FALLING);
    Serial.println("[ok] trackball + keyboard input");

    /* 7) SD card (shared bus) hosts the message store; RAM-only if absent.
     * The Arduino SD lib is rooted at the card (VFS mount "/sd"), so mkdir is
     * SD-relative ("/limitlezz") while POSIX fopen uses the VFS path
     * ("/sd/limitlezz/..."). Getting this wrong = writes silently fail and
     * messages vanish on reopen. */
    digitalWrite(BOARD_TFT_CS, HIGH); digitalWrite(RADIO_CS, HIGH);
    const char *datadir = NULL;
    if(SD.begin(SDCARD_CS, SPI, 4000000U)) {
        if(SD.exists("/limitlezz") || SD.mkdir("/limitlezz")) datadir = "/sd/limitlezz";
        else datadir = "/sd";                 /* fall back to the card root */
    }
    Serial.printf("[%s] microSD %s\n", datadir ? "ok" : "--",
                  datadir ? datadir : "absent (RAM-only this session)");

    /* persistence probe: write then read a file so the boot log proves the
     * message store actually round-trips on this card */
    if(datadir) {
        char probe[64]; snprintf(probe, sizeof probe, "%s/.probe", datadir);
        FILE *pf = fopen(probe, "wb");
        bool wrote = pf && fwrite("LZ", 1, 2, pf) == 2; if(pf) fclose(pf);
        char rb[3] = {0}; FILE *rf = fopen(probe, "rb");
        bool read_ok = rf && fread(rb, 1, 2, rf) == 2 && rb[0] == 'L' && rb[1] == 'Z';
        if(rf) fclose(rf);
        Serial.printf("[%s] message store read/write\n", (wrote && read_ok) ? "ok" : "FAIL");
    }

    /* 8) services. Real device: node id from the chip MAC (low 4 bytes, like
     * Meshtastic), real system info, and NO demo seed — start empty and fill
     * from actual radio traffic. */
    digitalWrite(BOARD_TFT_CS, HIGH); digitalWrite(SDCARD_CS, HIGH);
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint32_t nodenum = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                       ((uint32_t)mac[4] << 8)  |  (uint32_t)mac[5];
    lz_svc_set_node_num(nodenum);
    lz_set_backlight_cb(backlight_set);
    lz_set_sysinfo_cb(tdeck_sysinfo);
    lz_svc_init(datadir, false);          /* false = no demo contacts/messages */
    lz_svc_set_dirty_cb(lz_rebuild);
    lz_mtc_ble_begin();
    lz_wifi_init();
    Serial.printf("[%s] SX1262 radio (RadioLib begin=%d)\n",
                  lz_backend_ok() ? "ok" : "FAIL", lz_backend_begin_state());
    Serial.printf("[ok] node id !%08x\n", (unsigned)nodenum);

    kb_set_brightness(200);              /* keyboard backlight on (via I2C to the C3) */

    lz_ui_init(lv_scr_act());            /* applies brightness via backlight_set */
    { int sw, ix, iy;                    /* apply a saved touch calibration, if any */
      if(lz_store_load_touch(&sw, &ix, &iy)) {
          lz_touch_set_transform(sw, ix, iy);
          Serial.printf("[ok] touch calibration loaded: swap=%d invx=%d invy=%d\n", sw, ix, iy);
      } }
    Serial.println("=== boot complete ===");
    lz_cli_begin();                      /* serial command console */
}

static uint32_t g_last_input_ms;     /* for the keyboard-light Auto timeout */

/* The keyboard backlight LED is on the keyboard's ESP32-C3 (its GPIO9), NOT a
 * main-MCU pin. Control it over I2C: command 0x01 then a 0..255 PWM duty. */
static void kb_set_brightness(uint8_t duty)
{
    Wire.beginTransmission(KEYBOARD_I2C_ADDR);
    Wire.write(0x01);
    Wire.write(duty);
    Wire.endTransmission();
}

static void kb_backlight_update(void)
{
    int want;
    switch(S.settings.kb_light) {
        case 1: want = 200; break;                                   /* On      */
        case 2: want = 0; break;                                     /* Off     */
        default: want = (millis() - g_last_input_ms) < KB_BL_TIMEOUT_MS ? 200 : 0; break; /* Auto */
    }
    static int last = -1;
    if(want != last) { last = want; kb_set_brightness((uint8_t)want); }  /* only on change */
}

static char read_kb(void)
{
    Wire.requestFrom(KEYBOARD_I2C_ADDR, 1);
    if(Wire.available()) {
        char c = Wire.read();
        if(c != 0) return c;
    }
    return 0;
}

void loop()
{
    /* trackball roll: 2 pulses per focus step debounces jitter; a strong left
     * flick (many pulses at once) is a deliberate back gesture */
    /* vertical is responsive; horizontal has a bigger deadzone so left/right
     * (tab switch / back / column move) isn't twitchy */
    static const int STEP_V = 2, STEP_H = 3;
    bool input = false;
    if(tb_up >= STEP_V)    { tb_up = 0;    lz_ui_key(LZ_K_UP, 0);    input = true; }
    if(tb_down >= STEP_V)  { tb_down = 0;  lz_ui_key(LZ_K_DOWN, 0);  input = true; }
    if(tb_left >= STEP_H)  { tb_left = 0;  lz_ui_key(LZ_K_LEFT, 0);  input = true; }
    if(tb_right >= STEP_H) { tb_right = 0; lz_ui_key(LZ_K_RIGHT, 0); input = true; }

    static bool click_was = false;
    bool click = digitalRead(TRACKBALL_CLICK) == LOW;
    if(click && !click_was) { lz_ui_key(LZ_K_ENTER, 0); input = true; }
    click_was = click;

    static uint32_t last_kb = 0;
    if(millis() - last_kb > 40) {
        last_kb = millis();
        char c = read_kb();
        if(c) input = true;
        if(c == '\r' || c == '\n') lz_ui_key(LZ_K_ENTER, 0);
        else if(c == 8 || c == 127) lz_ui_key(LZ_K_DEL, 0);   /* backspace = delete char / back */
        else if(c) lz_ui_key(LZ_K_CHAR, c);
    }
    if(input) g_last_input_ms = millis();
    kb_backlight_update();

    lz_mtc_ble_poll();                   /* BLE advert/connection maintenance */
    if(lz_mtc_active()) lz_mtc_poll();   /* companion mode: USB speaks the Meshtastic app protocol */
    else lz_cli_poll();                  /* otherwise: the text command console */
    lz_svc_loop();
    static int last_wifi = -1;
    lz_wifi_loop();
    if(lz_wifi_status() != last_wifi) {
        int now = lz_wifi_status();
        /* on first WiFi connect, auto-sync the clock from NTP (UTC) */
        if(now == LZ_WIFI_CONNECTED && last_wifi != LZ_WIFI_CONNECTED && !lz_svc_time_synced()) {
            configTime(0, 0, "pool.ntp.org", "time.nist.gov");
            struct tm tmv;
            if(getLocalTime(&tmv, 4000)) {
                time_t t = mktime(&tmv);
                lz_svc_set_time((uint32_t)t);
                Serial.printf("[ok] time synced via NTP: %04d-%02d-%02d %02d:%02d UTC\n",
                              tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
            }
        }
        last_wifi = now;
        if(S.view == LZ_V_WIFI && !S.wifi_pw_mode) lz_rebuild();
    }

    /* Power saving: drop the CPU to 80 MHz (min for WiFi) when enabled */
    static int last_save = -1;
    if((int)S.settings.save != last_save) {
        last_save = S.settings.save;
        setCpuFrequencyMhz(S.settings.save ? 80 : 240);
    }

    /* touch-calibration tap captured in the input callback — apply it here,
     * outside LVGL's input processing (touchcal_register may rebuild) */
    if(g_cal_pending) { g_cal_pending = false; touchcal_register(g_cal_px, g_cal_py); }

    lz_idle_tick();                      /* sleep-after: dim + lock when idle */
    lv_timer_handler();
    delay(5);
}

#endif /* LZ_TARGET_TDECK */
