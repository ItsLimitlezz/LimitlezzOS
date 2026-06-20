#ifdef LZ_TARGET_TDECK

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <string.h>
#include <string>
#include <vector>
#include "services/mesh.h"
#include "services/wifi.h"

#define MCC_BLE_SERVICE_UUID "b8f13f20-6a6f-4f8d-8b0d-4d4330000001"
#define MCC_BLE_RX_UUID      "b8f13f20-6a6f-4f8d-8b0d-4d4330000002"
#define MCC_BLE_TX_UUID      "b8f13f20-6a6f-4f8d-8b0d-4d4330000003"

#define MCC_BLE_ATT_MTU     517
#define MCC_BLE_RX_QUEUE    4
#define MCC_BLE_TX_QUEUE    8
#define MCC_BLE_LINE_MAX    512
#define MCC_BLE_CHUNK_MAX   512

typedef struct {
    uint16_t len;
    char     data[MCC_BLE_LINE_MAX];
} mcc_rx_t;

typedef struct {
    uint32_t seq;
    uint16_t len;
    char     data[MCC_BLE_CHUNK_MAX];
} mcc_tx_t;

static NimBLEServer *g_mcc_server;
static NimBLECharacteristic *g_mcc_rx_chr;
static NimBLECharacteristic *g_mcc_tx_chr;
static volatile bool g_mcc_ready;
static volatile bool g_mcc_enabled;
static volatile bool g_mcc_connected;
static volatile bool g_mcc_tx_subscribed;
static volatile uint32_t g_mcc_connect_count;
static volatile uint32_t g_mcc_disconnect_count;
static volatile uint32_t g_mcc_connected_since_ms;
static volatile uint32_t g_mcc_last_disconnect_ms;
static volatile uint32_t g_mcc_last_io_ms;
static volatile int g_mcc_last_disconnect_reason = -1;
static volatile uint16_t g_mcc_last_mtu;
static volatile uint16_t g_mcc_last_rx_len;
static volatile uint32_t g_mcc_rx_write_count;
static volatile uint32_t g_mcc_tx_read_count;
static volatile uint32_t g_mcc_tx_notify_count;
static volatile uint32_t g_mcc_overflow_count;

static mcc_rx_t g_mcc_rx[MCC_BLE_RX_QUEUE];
static mcc_tx_t g_mcc_tx[MCC_BLE_TX_QUEUE];
static int g_mcc_rx_head, g_mcc_rx_count;
static int g_mcc_tx_head, g_mcc_tx_count;
static uint32_t g_mcc_next_seq = 1;
static portMUX_TYPE g_mcc_mux = portMUX_INITIALIZER_UNLOCKED;

extern "C" bool btStarted(void);

extern "C" bool lz_mcc_ble_enabled(void) { return g_mcc_enabled; }
extern "C" bool lz_mcc_ble_connected(void) { return g_mcc_connected; }

static void mcc_ble_clear_queues_locked(void)
{
    g_mcc_rx_head = g_mcc_rx_count = 0;
    g_mcc_tx_head = g_mcc_tx_count = 0;
}

static void mcc_ble_enqueue_tx_raw(const char *data, int len)
{
    if(!data || len <= 0) return;
    if(len >= MCC_BLE_CHUNK_MAX) len = MCC_BLE_CHUNK_MAX - 1;
    taskENTER_CRITICAL(&g_mcc_mux);
    if(g_mcc_tx_count >= MCC_BLE_TX_QUEUE) {
        g_mcc_tx_head = (g_mcc_tx_head + 1) % MCC_BLE_TX_QUEUE;
        g_mcc_tx_count--;
        g_mcc_overflow_count++;
    }
    int idx = (g_mcc_tx_head + g_mcc_tx_count) % MCC_BLE_TX_QUEUE;
    g_mcc_tx[idx].seq = g_mcc_next_seq++;
    if(!g_mcc_next_seq) g_mcc_next_seq = 1;
    g_mcc_tx[idx].len = (uint16_t)len;
    memcpy(g_mcc_tx[idx].data, data, (size_t)len);
    g_mcc_tx[idx].data[len] = 0;
    g_mcc_tx_count++;
    taskEXIT_CRITICAL(&g_mcc_mux);
}

static void mcc_ble_enqueue_tx_text(const char *text)
{
    if(!text || !text[0]) return;
    const char *p = text;
    while(*p) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) + 1 : (int)strlen(p);
        while(len > 0) {
            int chunk = len;
            if(chunk >= MCC_BLE_CHUNK_MAX) chunk = MCC_BLE_CHUNK_MAX - 1;
            mcc_ble_enqueue_tx_raw(p, chunk);
            p += chunk;
            len -= chunk;
        }
        if(!nl) break;
    }
}

static bool mcc_ble_pop_rx(char *out, int cap)
{
    if(!out || cap <= 0) return false;
    bool ok = false;
    taskENTER_CRITICAL(&g_mcc_mux);
    if(g_mcc_rx_count > 0) {
        mcc_rx_t *rx = &g_mcc_rx[g_mcc_rx_head];
        int len = rx->len;
        if(len >= cap) len = cap - 1;
        memcpy(out, rx->data, (size_t)len);
        out[len] = 0;
        g_mcc_rx_head = (g_mcc_rx_head + 1) % MCC_BLE_RX_QUEUE;
        g_mcc_rx_count--;
        ok = true;
    }
    taskEXIT_CRITICAL(&g_mcc_mux);
    return ok;
}

static bool mcc_ble_pop_tx(char *out, int cap, uint32_t *seq_out)
{
    if(!out || cap <= 0) return false;
    bool ok = false;
    taskENTER_CRITICAL(&g_mcc_mux);
    if(g_mcc_tx_count > 0) {
        mcc_tx_t *tx = &g_mcc_tx[g_mcc_tx_head];
        int len = tx->len;
        if(len >= cap) len = cap - 1;
        memcpy(out, tx->data, (size_t)len);
        out[len] = 0;
        if(seq_out) *seq_out = tx->seq;
        g_mcc_tx_head = (g_mcc_tx_head + 1) % MCC_BLE_TX_QUEUE;
        g_mcc_tx_count--;
        ok = true;
    }
    taskEXIT_CRITICAL(&g_mcc_mux);
    return ok;
}

static void mcc_ble_prepare_tx_value(void)
{
    if(!g_mcc_tx_chr) return;
    char out[MCC_BLE_CHUNK_MAX];
    uint32_t seq = 0;
    if(mcc_ble_pop_tx(out, sizeof out, &seq)) {
        g_mcc_tx_chr->setValue((const uint8_t *)out, strlen(out));
    } else {
        g_mcc_tx_chr->setValue((const uint8_t *)NULL, 0);
    }
    (void)seq;
}

static void mcc_ble_notify_latest(void)
{
    if(!g_mcc_connected || !g_mcc_tx_subscribed || !g_mcc_tx_chr) return;
    char out[MCC_BLE_CHUNK_MAX];
    uint32_t seq = 0;
    if(!mcc_ble_pop_tx(out, sizeof out, &seq)) return;
    g_mcc_tx_chr->setValue((const uint8_t *)out, strlen(out));
    g_mcc_tx_chr->notify((const uint8_t *)out, strlen(out));
    taskENTER_CRITICAL(&g_mcc_mux);
    g_mcc_tx_notify_count++;
    g_mcc_last_io_ms = millis();
    taskEXIT_CRITICAL(&g_mcc_mux);
    (void)seq;
}

class MccBleServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *server, NimBLEConnInfo &connInfo) override
    {
        uint32_t now = millis();
        taskENTER_CRITICAL(&g_mcc_mux);
        mcc_ble_clear_queues_locked();
        g_mcc_connected = true;
        g_mcc_tx_subscribed = false;
        g_mcc_connect_count++;
        g_mcc_connected_since_ms = now;
        g_mcc_last_io_ms = 0;
        g_mcc_last_mtu = 0;
        g_mcc_last_rx_len = 0;
        g_mcc_rx_write_count = 0;
        g_mcc_tx_read_count = 0;
        g_mcc_tx_notify_count = 0;
        taskEXIT_CRITICAL(&g_mcc_mux);
        if(server) {
            server->updateConnParams(connInfo.getConnHandle(), 24, 48, 0, 180);
            server->setDataLen(connInfo.getConnHandle(), 251);
        }
    }
    void onDisconnect(NimBLEServer *server, NimBLEConnInfo &connInfo, int reason) override
    {
        (void)server; (void)connInfo;
        uint32_t now = millis();
        taskENTER_CRITICAL(&g_mcc_mux);
        g_mcc_connected = false;
        g_mcc_tx_subscribed = false;
        mcc_ble_clear_queues_locked();
        g_mcc_disconnect_count++;
        g_mcc_last_disconnect_reason = reason;
        g_mcc_last_disconnect_ms = now;
        g_mcc_connected_since_ms = 0;
        taskEXIT_CRITICAL(&g_mcc_mux);
        lz_svc_mc_companion_reset_session();
        if(g_mcc_enabled) NimBLEDevice::startAdvertising();
    }
    void onMTUChange(uint16_t mtu, NimBLEConnInfo &connInfo) override
    {
        (void)connInfo;
        taskENTER_CRITICAL(&g_mcc_mux);
        g_mcc_last_mtu = mtu;
        taskEXIT_CRITICAL(&g_mcc_mux);
    }
};

class MccBleRxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &connInfo) override
    {
        (void)connInfo;
        std::string v = chr->getValue();
        if(v.empty()) return;
        uint32_t now = millis();
        int len = (int)v.size();
        if(len >= MCC_BLE_LINE_MAX) len = MCC_BLE_LINE_MAX - 1;
        taskENTER_CRITICAL(&g_mcc_mux);
        if(g_mcc_rx_count >= MCC_BLE_RX_QUEUE) {
            g_mcc_rx_head = (g_mcc_rx_head + 1) % MCC_BLE_RX_QUEUE;
            g_mcc_rx_count--;
            g_mcc_overflow_count++;
        }
        int idx = (g_mcc_rx_head + g_mcc_rx_count) % MCC_BLE_RX_QUEUE;
        memcpy(g_mcc_rx[idx].data, v.data(), (size_t)len);
        while(len > 0 && (g_mcc_rx[idx].data[len - 1] == '\r' ||
                          g_mcc_rx[idx].data[len - 1] == '\n')) len--;
        g_mcc_rx[idx].data[len] = 0;
        g_mcc_rx[idx].len = (uint16_t)len;
        g_mcc_rx_count++;
        g_mcc_rx_write_count++;
        g_mcc_last_rx_len = (uint16_t)len;
        g_mcc_last_io_ms = now;
        taskEXIT_CRITICAL(&g_mcc_mux);
    }
};

class MccBleTxCallbacks : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic *chr, NimBLEConnInfo &connInfo) override
    {
        (void)chr; (void)connInfo;
        taskENTER_CRITICAL(&g_mcc_mux);
        g_mcc_tx_read_count++;
        g_mcc_last_io_ms = millis();
        taskEXIT_CRITICAL(&g_mcc_mux);
        mcc_ble_prepare_tx_value();
    }
    void onSubscribe(NimBLECharacteristic *chr, NimBLEConnInfo &connInfo, uint16_t subValue) override
    {
        (void)chr; (void)connInfo;
        taskENTER_CRITICAL(&g_mcc_mux);
        g_mcc_tx_subscribed = subValue != 0;
        g_mcc_last_io_ms = millis();
        taskEXIT_CRITICAL(&g_mcc_mux);
    }
};

static MccBleServerCallbacks g_mcc_server_cb;
static MccBleRxCallbacks g_mcc_rx_cb;
static MccBleTxCallbacks g_mcc_tx_cb;

static void mcc_ble_begin(void)
{
    if(g_mcc_ready) return;
    const lz_identity_t *id = lz_svc_identity();
    char name[32];
    snprintf(name, sizeof name, "Limitlezz-MC0-%s", id->short_name[0] ? id->short_name : "TD");

    if(!NimBLEDevice::init(name)) return;
    NimBLEDevice::setMTU(MCC_BLE_ATT_MTU);
    g_mcc_server = NimBLEDevice::createServer();
    g_mcc_server->setCallbacks(&g_mcc_server_cb, false);

    NimBLEService *svc = g_mcc_server->createService(MCC_BLE_SERVICE_UUID);
    g_mcc_rx_chr = svc->createCharacteristic(MCC_BLE_RX_UUID,
                                             NIMBLE_PROPERTY::WRITE,
                                             MCC_BLE_LINE_MAX);
    g_mcc_tx_chr = svc->createCharacteristic(MCC_BLE_TX_UUID,
                                             NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY,
                                             MCC_BLE_CHUNK_MAX);
    g_mcc_rx_chr->setCallbacks(&g_mcc_rx_cb);
    g_mcc_tx_chr->setCallbacks(&g_mcc_tx_cb);

    g_mcc_server->start();
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->setName(name);
    adv->addServiceUUID(MCC_BLE_SERVICE_UUID);
    adv->enableScanResponse(true);
    g_mcc_ready = true;
}

extern "C" void lz_mcc_ble_set_enabled(bool on)
{
    if(on) {
        if(lz_wifi_enabled()) lz_wifi_set_enabled(false);
        if(lz_mtc_active()) lz_mtc_set_active(false);
        if(lz_mtc_ble_enabled()) lz_mtc_ble_set_enabled(false);
        if(lz_mcc_usb_active()) lz_mcc_usb_set_active(false);
        if(!g_mcc_ready) mcc_ble_begin();
        if(!g_mcc_ready) {
            lz_wifi_set_enabled(true);
            return;
        }
        taskENTER_CRITICAL(&g_mcc_mux);
        mcc_ble_clear_queues_locked();
        g_mcc_enabled = true;
        g_mcc_tx_subscribed = false;
        taskEXIT_CRITICAL(&g_mcc_mux);
        lz_svc_mc_companion_reset_session();
        NimBLEDevice::startAdvertising();
    } else {
        g_mcc_enabled = false;
        if(!g_mcc_ready) return;
        lz_svc_mc_companion_reset_session();
        NimBLEDevice::stopAdvertising();
        if(g_mcc_server && g_mcc_connected) {
            std::vector<uint16_t> peers = g_mcc_server->getPeerDevices();
            for(size_t i = 0; i < peers.size(); i++)
                g_mcc_server->disconnect(peers[i]);
        }
        taskENTER_CRITICAL(&g_mcc_mux);
        g_mcc_connected = false;
        g_mcc_tx_subscribed = false;
        mcc_ble_clear_queues_locked();
        taskEXIT_CRITICAL(&g_mcc_mux);
        vTaskDelay(pdMS_TO_TICKS(100));
        NimBLEDevice::deinit(true);
        for(int i = 0; i < 50 && btStarted(); i++) vTaskDelay(pdMS_TO_TICKS(10));
        g_mcc_server = NULL;
        g_mcc_rx_chr = NULL;
        g_mcc_tx_chr = NULL;
        g_mcc_ready = false;
    }
}

extern "C" void lz_mcc_ble_poll(void)
{
    if(!g_mcc_enabled) return;
    char line[MCC_BLE_LINE_MAX];
    while(mcc_ble_pop_rx(line, sizeof line)) {
        char out[1200];
        bool exit_mode = false;
        lz_svc_mc_companion_handle_line_for(line, "ble", out, sizeof out, &exit_mode);
        mcc_ble_enqueue_tx_text(out);
        if(exit_mode) {
            lz_mcc_ble_set_enabled(false);
            return;
        }
    }
    char events[700];
    if(lz_svc_mc_companion_drain_events(events, sizeof events) > 0)
        mcc_ble_enqueue_tx_text(events);
    mcc_ble_notify_latest();
    if(g_mcc_ready && !g_mcc_connected && !NimBLEDevice::getAdvertising()->isAdvertising())
        NimBLEDevice::startAdvertising();
}

extern "C" int lz_mcc_ble_status(char *buf, int n)
{
    uint32_t connects, disconnects, connected_since, last_disconnect, last_io;
    uint32_t rx_writes, tx_reads, tx_notifies, overflow, next_seq;
    int rx_q, tx_q, last_reason;
    uint16_t mtu, last_rx_len;
    bool enabled, connected, subscribed;
    taskENTER_CRITICAL(&g_mcc_mux);
    enabled = g_mcc_enabled;
    connected = g_mcc_connected;
    subscribed = g_mcc_tx_subscribed;
    rx_q = g_mcc_rx_count;
    tx_q = g_mcc_tx_count;
    connects = g_mcc_connect_count;
    disconnects = g_mcc_disconnect_count;
    connected_since = g_mcc_connected_since_ms;
    last_disconnect = g_mcc_last_disconnect_ms;
    last_io = g_mcc_last_io_ms;
    last_reason = g_mcc_last_disconnect_reason;
    mtu = g_mcc_last_mtu;
    last_rx_len = g_mcc_last_rx_len;
    rx_writes = g_mcc_rx_write_count;
    tx_reads = g_mcc_tx_read_count;
    tx_notifies = g_mcc_tx_notify_count;
    overflow = g_mcc_overflow_count;
    next_seq = g_mcc_next_seq;
    taskEXIT_CRITICAL(&g_mcc_mux);
    uint32_t now = millis();
    const char *state = !enabled ? "off" : connected ? "connected" : "advertising";
    const char *age_label = connected ? "up" : "down";
    uint32_t age_ms = connected ? (connected_since ? now - connected_since : 0)
                                : (last_disconnect ? now - last_disconnect : 0);
    uint32_t io_age_ms = last_io ? now - last_io : 0;
    return snprintf(buf, n,
                    "MeshCore BLE MC0: %s | sub=%d rxq=%d/%d txq=%d/%d seq=%lu c=%lu d=%lu "
                    "r=%d mtu=%u rx=%lu/%uB tx=%lu/%lu ov=%lu %s=%lums io=%lums svc=%.8s",
                    state, subscribed ? 1 : 0,
                    rx_q, MCC_BLE_RX_QUEUE, tx_q, MCC_BLE_TX_QUEUE,
                    (unsigned long)(next_seq ? next_seq - 1 : 0),
                    (unsigned long)connects, (unsigned long)disconnects,
                    last_reason, (unsigned)mtu, (unsigned long)rx_writes,
                    (unsigned)last_rx_len, (unsigned long)tx_reads,
                    (unsigned long)tx_notifies, (unsigned long)overflow,
                    age_label, (unsigned long)age_ms, (unsigned long)io_age_ms,
                    MCC_BLE_SERVICE_UUID);
}

extern "C" int lz_mcc_ble_selftest(char *buf, int n)
{
    if(!g_mcc_ready)
        return snprintf(buf, n, "MeshCore BLE MC0 selftest: enable BLE first (companion mc ble on)");
    if(g_mcc_connected)
        return snprintf(buf, n, "MeshCore BLE MC0 selftest skipped (active connection)");
    taskENTER_CRITICAL(&g_mcc_mux);
    mcc_ble_clear_queues_locked();
    taskEXIT_CRITICAL(&g_mcc_mux);

    char out[1200];
    bool exit_mode = false;
    lz_svc_mc_companion_handle_line_for("MC0 1 HELLO proto=0 app=ble-selftest",
                                        "ble", out, sizeof out, &exit_mode);
    bool ok = strstr(out, "MC0 1 OK proto=0") && strstr(out, "event_seq=");
    mcc_ble_enqueue_tx_text(out);
    lz_svc_mc_companion_handle_line_for("MC0 2 STATUS", "ble", out, sizeof out, &exit_mode);
    ok = ok && strstr(out, "bridge=ble");
    mcc_ble_enqueue_tx_text(out);
    char popped[MCC_BLE_CHUNK_MAX];
    uint32_t seq = 0;
    ok = ok && mcc_ble_pop_tx(popped, sizeof popped, &seq) &&
         strstr(popped, "MC0 1 OK") && seq > 0 && !exit_mode;
    taskENTER_CRITICAL(&g_mcc_mux);
    mcc_ble_clear_queues_locked();
    taskEXIT_CRITICAL(&g_mcc_mux);
    return snprintf(buf, n, "MeshCore BLE MC0 selftest: %s", ok ? "PASS" : "FAIL");
}

#endif /* LZ_TARGET_TDECK */
