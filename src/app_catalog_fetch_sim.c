#ifdef LZ_TARGET_SIM

#include "services/app_catalog_fetch.h"
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

bool lz_app_catalog_fetch(const char *url, char *out_json, int out_cap,
                          int *out_len, char *err, int err_cap)
{
    static const char fixture_url[] = "https://example.invalid/limitlezz/app-catalog-valid.json";
    static const char fixture_json[] =
        "{\"schema\":\"limitlezz.app.catalog.v1\",\"generated_at\":\"2026-06-20T00:00:00Z\","
        "\"apps\":[{\"id\":\"weather.mesh\",\"name\":\"Weather Mesh\",\"version\":\"0.1.0\","
        "\"author\":\"Limitless\",\"summary\":\"Local weather dashboard\","
        "\"description\":\"Shows a compact local weather panel.\","
        "\"icon\":\"weather\",\"hue\":48,\"api_version\":\"0.1\","
        "\"permissions\":[\"display\",\"input\",\"storage\",\"network_wifi\"],"
        "\"package_url\":\"https://example.invalid/limitlezz/apps/weather.mesh.zip\","
        "\"package_sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
        "\"package_bytes\":4096,"
        "\"compatibility\":{\"min_os\":\"0.95.0\",\"api_versions\":[\"0.1\"],\"targets\":[\"tdeck\",\"sim\"]},"
        "\"screenshots\":[{\"url\":\"https://example.invalid/limitlezz/apps/weather.bmp\","
        "\"width\":320,\"height\":240,"
        "\"sha256\":\"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789\"}]}]}";
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
    if(strcmp(url, fixture_url) == 0) {
        int len = (int)strlen(fixture_json);
        if(len >= LZ_APP_CATALOG_FETCH_MAX || len + 1 >= out_cap) {
            fetch_err(err, err_cap, "catalog too large");
            return false;
        }
        memcpy(out_json, fixture_json, (size_t)len + 1);
        if(out_len) *out_len = len;
        return true;
    }
    fetch_err(err, err_cap, "fetch unavailable");
    return false;
}

#endif
