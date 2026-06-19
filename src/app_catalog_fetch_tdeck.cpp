#ifdef LZ_TARGET_TDECK

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "services/app_catalog_fetch.h"
#include "services/wifi.h"
#include <stdio.h>
#include <string.h>

static void fetch_err(char *err, int err_cap, const char *msg)
{
    if(err && err_cap > 0) snprintf(err, (size_t)err_cap, "%s", msg);
}

static bool fetch_url_ok(const char *url)
{
    return url &&
           (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0);
}

static bool catalog_too_large(int total, int out_cap)
{
    return total >= LZ_APP_CATALOG_FETCH_MAX || total + 1 >= out_cap;
}

static bool fetch_read_body(HTTPClient &http, char *out_json, int out_cap,
                            int *out_len, char *err, int err_cap)
{
    int declared = http.getSize();
    if(declared > LZ_APP_CATALOG_FETCH_MAX || declared + 1 >= out_cap) {
        fetch_err(err, err_cap, "catalog too large");
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    int total = 0;
    uint32_t idle_deadline = millis() + 5000u;
    while(http.connected() || stream->available()) {
        int avail = stream->available();
        if(avail <= 0) {
            if((int32_t)(millis() - idle_deadline) >= 0) break;
            delay(1);
            continue;
        }
        idle_deadline = millis() + 5000u;
        while(avail-- > 0) {
            int c = stream->read();
            if(c < 0) break;
            if(catalog_too_large(total, out_cap)) {
                fetch_err(err, err_cap, "catalog too large");
                return false;
            }
            out_json[total++] = (char)c;
        }
    }
    if(total <= 0) {
        fetch_err(err, err_cap, "catalog empty");
        return false;
    }
    out_json[total] = 0;
    if(out_len) *out_len = total;
    return true;
}

static bool fetch_with_client(WiFiClient &client, const char *url, char *out_json,
                              int out_cap, int *out_len, char *err, int err_cap)
{
    HTTPClient http;
    http.setTimeout(6000);
    if(!http.begin(client, url)) {
        fetch_err(err, err_cap, "http begin failed");
        return false;
    }
    int code = http.GET();
    if(code != HTTP_CODE_OK) {
        char msg[32];
        snprintf(msg, sizeof msg, "http %d", code);
        fetch_err(err, err_cap, msg);
        http.end();
        return false;
    }
    bool ok = fetch_read_body(http, out_json, out_cap, out_len, err, err_cap);
    http.end();
    return ok;
}

bool lz_app_catalog_fetch(const char *url, char *out_json, int out_cap,
                          int *out_len, char *err, int err_cap)
{
    if(out_json && out_cap > 0) out_json[0] = 0;
    if(out_len) *out_len = 0;
    fetch_err(err, err_cap, "");
    if(!out_json || out_cap <= 1) {
        fetch_err(err, err_cap, "catalog buffer small");
        return false;
    }
    if(!fetch_url_ok(url)) {
        fetch_err(err, err_cap, "bad url");
        return false;
    }
    if(lz_wifi_status() != LZ_WIFI_CONNECTED || WiFi.status() != WL_CONNECTED) {
        fetch_err(err, err_cap, "wifi offline");
        return false;
    }

    if(strncmp(url, "https://", 8) == 0) {
        WiFiClientSecure client;
        client.setInsecure();  /* TODO: pin catalog host certificate before broad release. */
        return fetch_with_client(client, url, out_json, out_cap, out_len, err, err_cap);
    }

    WiFiClient client;
    return fetch_with_client(client, url, out_json, out_cap, out_len, err, err_cap);
}

#endif
