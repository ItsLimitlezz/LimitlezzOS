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
/* lz_backend_ok / lz_backend_begin_state declared in services/mesh.h */

/* GT911 reports panel-native portrait coordinates (240x320); the display
 * runs landscape (rotation 1). Flip these if touch tracks rotated on a
 * different board/config revision. */
#ifndef LZ_TOUCH_SWAP_XY
#define LZ_TOUCH_SWAP_XY 1
#endif
#ifndef LZ_TOUCH_INVERT_X
#define LZ_TOUCH_INVERT_X 0
#endif
#ifndef LZ_TOUCH_INVERT_Y
#define LZ_TOUCH_INVERT_Y 1
#endif

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
static lv_color_t draw_buf_mem[LZ_W * 40];

extern "C" uint32_t lz_tick_ms(void) { return millis(); }

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
#if LZ_TOUCH_SWAP_XY
            int sx = ty, sy = tx;
#else
            int sx = tx, sy = ty;
#endif
#if LZ_TOUCH_INVERT_X
            sx = LZ_W - 1 - sx;
#endif
#if LZ_TOUCH_INVERT_Y
            sy = LZ_H - 1 - sy;
#endif
            if(sx < 0) sx = 0; if(sx >= LZ_W) sx = LZ_W - 1;
            if(sy < 0) sy = 0; if(sy >= LZ_H) sy = LZ_H - 1;
            last_x = sx;
            last_y = sy;
            data->point.x = sx;
            data->point.y = sy;
            data->state = LV_INDEV_STATE_PRESSED;
            lz_note_activity();                  /* touch keeps the screen awake */
        }
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
    /* boot self-test: a brief R/G/B flash (RGB565). Seeing colors => panel +
     * backlight + color order are good and any later black is the UI layer;
     * staying black => display init or backlight. */
    lcd.fillScreen(0xF800); delay(250);   /* red   */
    lcd.fillScreen(0x07E0); delay(250);   /* green */
    lcd.fillScreen(0x001F); delay(250);   /* blue  */
    lcd.fillScreen(0x0000);
    Serial.println("[ok] ST7789 display init + backlight on (R/G/B test shown)");

    lv_init();
    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, draw_buf_mem, NULL, LZ_W * 40);
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
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

    /* 7) SD card (shared bus) hosts the message store; RAM-only if absent */
    digitalWrite(BOARD_TFT_CS, HIGH); digitalWrite(RADIO_CS, HIGH);
    const char *datadir = NULL;
    if(SD.begin(SDCARD_CS, SPI, 4000000U)) { datadir = "/sd/limitlezz"; SD.mkdir("/sd/limitlezz"); }
    Serial.printf("[%s] microSD %s\n", datadir ? "ok" : "--",
                  datadir ? "mounted -> /sd/limitlezz" : "absent (RAM-only this session)");

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
    lz_wifi_init();
    Serial.printf("[%s] SX1262 radio (RadioLib begin=%d)\n",
                  lz_backend_ok() ? "ok" : "FAIL", lz_backend_begin_state());
    Serial.printf("[ok] node id !%08x\n", (unsigned)nodenum);

    lz_ui_init(lv_scr_act());            /* applies brightness via backlight_set */
    Serial.println("=== boot complete ===");
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
    /* trackball roll: 2 pulses per focus step debounces jitter */
    static const int STEP = 2;
    if(tb_up >= STEP)    { tb_up = 0;    lz_ui_key(LZ_K_UP, 0); }
    if(tb_down >= STEP)  { tb_down = 0;  lz_ui_key(LZ_K_DOWN, 0); }
    if(tb_left >= STEP)  { tb_left = 0;  lz_ui_key(LZ_K_LEFT, 0); }
    if(tb_right >= STEP) { tb_right = 0; lz_ui_key(LZ_K_RIGHT, 0); }

    static bool click_was = false;
    bool click = digitalRead(TRACKBALL_CLICK) == LOW;
    if(click && !click_was) lz_ui_key(LZ_K_ENTER, 0);
    click_was = click;

    static uint32_t last_kb = 0;
    if(millis() - last_kb > 40) {
        last_kb = millis();
        char c = read_kb();
        if(c == '\r' || c == '\n') lz_ui_key(LZ_K_ENTER, 0);
        else if(c == 8 || c == 127) lz_ui_key(LZ_K_DEL, 0);   /* backspace = delete char / back */
        else if(c) lz_ui_key(LZ_K_CHAR, c);
    }

    lz_svc_loop();
    static int last_wifi = -1;
    lz_wifi_loop();
    if(lz_wifi_status() != last_wifi) {
        last_wifi = lz_wifi_status();
        if(S.view == LZ_V_WIFI && !S.wifi_pw_mode) lz_rebuild();
    }
    lz_idle_tick();                      /* sleep-after: dim + lock when idle */
    lv_timer_handler();
    delay(5);
}

#endif /* LZ_TARGET_TDECK */
