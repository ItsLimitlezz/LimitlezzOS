#ifndef LZ_APP_PACKAGE_FETCH_H
#define LZ_APP_PACKAGE_FETCH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool lz_app_package_fetch(const char *url, const char *dest_path,
                          uint32_t expected_bytes, uint32_t max_bytes,
                          uint32_t *out_bytes, char *err, int err_cap);

#ifdef __cplusplus
}
#endif

#endif
