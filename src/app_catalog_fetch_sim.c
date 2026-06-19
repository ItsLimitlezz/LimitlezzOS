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
    fetch_err(err, err_cap, "fetch unavailable");
    return false;
}

#endif
