#ifdef LZ_TARGET_TDECK

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "services/mesh.h"
#include "services/ota_fetch.h"
#include "services/wifi.h"
#include <stdio.h>
#include <string.h>

static void ota_fetch_err(char *err, int err_cap, const char *msg)
{
    if(err && err_cap > 0) snprintf(err, (size_t)err_cap, "%s", msg);
}

static bool ota_fetch_url_ok(const char *url)
{
    return url &&
           (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0);
}

static bool ota_fetch_read_body(HTTPClient &http, const char *path,
                                uint32_t expected_size,
                                char *err, int err_cap)
{
    int declared = http.getSize();
    if(declared > 0 && (uint32_t)declared != expected_size) {
        ota_fetch_err(err, err_cap, "size mismatch");
        return false;
    }
    if(expected_size == 0 || expected_size > LZ_OTA_SLOT_MAX_BYTES) {
        ota_fetch_err(err, err_cap, "bad size");
        return false;
    }

    FILE *f = fopen(path, "wb");
    if(!f) {
        ota_fetch_err(err, err_cap, "candidate write failed");
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    uint8_t buf[512];
    uint32_t total = 0;
    uint32_t idle_deadline = millis() + 8000u;
    bool ok = true;
    while(http.connected() || stream->available()) {
        int avail = stream->available();
        if(avail <= 0) {
            if((int32_t)(millis() - idle_deadline) >= 0) break;
            delay(1);
            continue;
        }
        idle_deadline = millis() + 8000u;
        int want = avail;
        if(want > (int)sizeof buf) want = (int)sizeof buf;
        int got = stream->readBytes(buf, (size_t)want);
        if(got <= 0) continue;
        if(UINT32_MAX - total < (uint32_t)got || total + (uint32_t)got > expected_size) {
            ota_fetch_err(err, err_cap, "candidate too large");
            ok = false;
            break;
        }
        if(fwrite(buf, 1, (size_t)got, f) != (size_t)got) {
            ota_fetch_err(err, err_cap, "candidate write failed");
            ok = false;
            break;
        }
        total += (uint32_t)got;
    }

    if(fclose(f) != 0 && ok) {
        ota_fetch_err(err, err_cap, "candidate write failed");
        ok = false;
    }
    if(ok && total != expected_size) {
        ota_fetch_err(err, err_cap, "size mismatch");
        ok = false;
    }
    if(!ok) remove(path);
    return ok;
}

static bool ota_fetch_with_client(WiFiClient &client, const char *url,
                                  const char *path, uint32_t expected_size,
                                  char *err, int err_cap)
{
    HTTPClient http;
    http.setTimeout(8000);
    if(!http.begin(client, url)) {
        ota_fetch_err(err, err_cap, "http begin failed");
        return false;
    }
    int code = http.GET();
    if(code != HTTP_CODE_OK) {
        char msg[32];
        snprintf(msg, sizeof msg, "http %d", code);
        ota_fetch_err(err, err_cap, msg);
        http.end();
        return false;
    }
    bool ok = ota_fetch_read_body(http, path, expected_size, err, err_cap);
    http.end();
    return ok;
}

bool lz_ota_fetch_to_file(const char *url, const char *path,
                          uint32_t expected_size,
                          char *err, int err_cap)
{
    ota_fetch_err(err, err_cap, "");
    if(!path || !path[0]) {
        ota_fetch_err(err, err_cap, "missing target");
        return false;
    }
    if(!ota_fetch_url_ok(url)) {
        ota_fetch_err(err, err_cap, "bad url");
        return false;
    }
    if(lz_wifi_status() != LZ_WIFI_CONNECTED || WiFi.status() != WL_CONNECTED) {
        ota_fetch_err(err, err_cap, "wifi offline");
        return false;
    }

    remove(path);
    if(strncmp(url, "https://", 8) == 0) {
        WiFiClientSecure client;
        client.setInsecure();  /* TODO: pin the update host before public OTA release. */
        return ota_fetch_with_client(client, url, path, expected_size, err, err_cap);
    }

    WiFiClient client;
    return ota_fetch_with_client(client, url, path, expected_size, err, err_cap);
}

#endif
