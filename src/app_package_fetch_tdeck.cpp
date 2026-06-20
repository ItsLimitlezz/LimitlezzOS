#ifdef LZ_TARGET_TDECK

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "services/app_package_fetch.h"
#include "services/wifi.h"
#include <stdio.h>
#include <string.h>

static void package_fetch_err(char *err, int err_cap, const char *msg)
{
    if(err && err_cap > 0) snprintf(err, (size_t)err_cap, "%s", msg);
}

static bool package_fetch_url_ok(const char *url)
{
    return url &&
           (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0);
}

static bool package_fetch_read_body(HTTPClient &http, const char *dest_path,
                                    uint32_t expected_bytes, uint32_t max_bytes,
                                    uint32_t *out_bytes, char *err, int err_cap)
{
    int declared = http.getSize();
    if(declared >= 0 && (uint32_t)declared != expected_bytes) {
        package_fetch_err(err, err_cap, "size mismatch");
        return false;
    }
    if(expected_bytes == 0 || expected_bytes > max_bytes) {
        package_fetch_err(err, err_cap, "package too large");
        return false;
    }

    FILE *f = fopen(dest_path, "wb");
    if(!f) {
        package_fetch_err(err, err_cap, "package write failed");
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
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
        while(avail-- > 0) {
            int c = stream->read();
            if(c < 0) break;
            if(total >= expected_bytes || total >= max_bytes) {
                package_fetch_err(err, err_cap, "package too large");
                ok = false;
                break;
            }
            if(fputc(c, f) == EOF) {
                package_fetch_err(err, err_cap, "package write failed");
                ok = false;
                break;
            }
            total++;
        }
        if(!ok) break;
    }
    if(fclose(f) != 0 && ok) {
        package_fetch_err(err, err_cap, "package write failed");
        ok = false;
    }
    if(!ok) {
        remove(dest_path);
        return false;
    }
    if(total != expected_bytes) {
        remove(dest_path);
        package_fetch_err(err, err_cap, "size mismatch");
        return false;
    }
    if(out_bytes) *out_bytes = total;
    return true;
}

static bool package_fetch_with_client(WiFiClient &client, const char *url,
                                      const char *dest_path,
                                      uint32_t expected_bytes, uint32_t max_bytes,
                                      uint32_t *out_bytes, char *err, int err_cap)
{
    HTTPClient http;
    http.setTimeout(10000);
    if(!http.begin(client, url)) {
        package_fetch_err(err, err_cap, "http begin failed");
        return false;
    }
    int code = http.GET();
    if(code != HTTP_CODE_OK) {
        char msg[32];
        snprintf(msg, sizeof msg, "http %d", code);
        package_fetch_err(err, err_cap, msg);
        http.end();
        return false;
    }
    bool ok = package_fetch_read_body(http, dest_path, expected_bytes, max_bytes,
                                      out_bytes, err, err_cap);
    http.end();
    return ok;
}

bool lz_app_package_fetch(const char *url, const char *dest_path,
                          uint32_t expected_bytes, uint32_t max_bytes,
                          uint32_t *out_bytes, char *err, int err_cap)
{
    if(out_bytes) *out_bytes = 0;
    package_fetch_err(err, err_cap, "");
    if(!package_fetch_url_ok(url)) {
        package_fetch_err(err, err_cap, "bad package url");
        return false;
    }
    if(!dest_path || !dest_path[0] || expected_bytes == 0 || expected_bytes > max_bytes) {
        package_fetch_err(err, err_cap, "bad package request");
        return false;
    }
    if(lz_wifi_status() != LZ_WIFI_CONNECTED || WiFi.status() != WL_CONNECTED) {
        package_fetch_err(err, err_cap, "wifi offline");
        return false;
    }

    if(strncmp(url, "https://", 8) == 0) {
        WiFiClientSecure client;
        client.setInsecure();  /* TODO: pin package host certificate before broad release. */
        return package_fetch_with_client(client, url, dest_path, expected_bytes, max_bytes,
                                         out_bytes, err, err_cap);
    }

    WiFiClient client;
    return package_fetch_with_client(client, url, dest_path, expected_bytes, max_bytes,
                                     out_bytes, err, err_cap);
}

#endif
