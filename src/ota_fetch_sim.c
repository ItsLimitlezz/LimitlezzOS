#ifdef LZ_TARGET_SIM

#include "services/ota_fetch.h"
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

bool lz_ota_fetch_to_file(const char *url, const char *path,
                          uint32_t expected_size,
                          char *err, int err_cap)
{
    (void)expected_size;
    ota_fetch_err(err, err_cap, "");
    if(!path || !path[0]) {
        ota_fetch_err(err, err_cap, "missing target");
        return false;
    }
    if(!ota_fetch_url_ok(url)) {
        ota_fetch_err(err, err_cap, "bad url");
        return false;
    }
    ota_fetch_err(err, err_cap, "fetch unavailable");
    return false;
}

#endif
