#ifdef LZ_TARGET_SIM

#include "services/app_package_fetch.h"
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
    package_fetch_err(err, err_cap, "package fetch unavailable");
    return false;
}

#endif
