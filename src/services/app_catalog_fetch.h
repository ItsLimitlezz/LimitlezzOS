#ifndef LZ_APP_CATALOG_FETCH_H
#define LZ_APP_CATALOG_FETCH_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LZ_APP_CATALOG_FETCH_MAX 4096

bool lz_app_catalog_fetch(const char *url, char *out_json, int out_cap,
                          int *out_len, char *err, int err_cap);

#ifdef __cplusplus
}
#endif

#endif
