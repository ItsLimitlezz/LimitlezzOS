#ifndef LZ_OTA_INSTALL_H
#define LZ_OTA_INSTALL_H

#include <stdbool.h>
#include <stdint.h>
#include "mesh.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lz_ota_install_file_to_inactive(const char *path, uint32_t expected_size,
                                     lz_ota_install_t *out, char *err, int err_cap);
bool lz_ota_install_running_copy_test(lz_ota_install_t *out, char *err, int err_cap);

#ifdef __cplusplus
}
#endif

#endif
