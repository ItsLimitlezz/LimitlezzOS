#ifndef LZ_OTA_FETCH_H
#define LZ_OTA_FETCH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool lz_ota_fetch_to_file(const char *url, const char *path,
                          uint32_t expected_size,
                          char *err, int err_cap);

#ifdef __cplusplus
}
#endif

#endif
